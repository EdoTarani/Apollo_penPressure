/**
 * @file src/platform/windows/extra_screens.cpp
 * @brief Extra screens: companion Apollo instances, each streaming its own virtual display.
 */
// platform includes
#include <Windows.h>

// standard includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "extra_screens.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"

namespace fs = std::filesystem;

namespace platf::extra_screens {
  using namespace std::literals;

  namespace {
    struct child_t {
      int screen;  // 2, 3, ...
      std::wstring config_path;
      HANDLE process = nullptr;
    };

    HANDLE g_job = nullptr;
    HANDLE g_stop_event = nullptr;
    std::thread g_monitor;
    std::vector<child_t> g_children;

    /// Keys an extra screen must not inherit from the main configuration
    const std::set<std::string> own_keys {
      "port", "sunshine_name", "file_state", "credentials_file", "paired_devices_file", "extra_screen_index", "log_path",
      "output_name", "headless_mode", "stream_audio", "system_tray", "extra_screens",
      "pen_virtual_tablet_host", "pen_virtual_tablet_desktop",
    };

    std::string trim(const std::string &s) {
      auto b = s.find_first_not_of(" \t\r\n");
      auto e = s.find_last_not_of(" \t\r\n");
      return b == std::string::npos ? std::string {} : s.substr(b, e - b + 1);
    }

    /// Write the config for extra screen `screen`, derived from the main instance's
    fs::path write_config(int screen) {
      auto dir = fs::path(config::sunshine.config_file).parent_path();
      auto suffix = "screen" + std::to_string(screen);
      std::ostringstream out;

      // Everything the user configured (encoder, bitrate, apps, ...) carries over
      std::ifstream main_config(config::sunshine.config_file);
      std::string line;
      while (std::getline(main_config, line)) {
        auto eq = line.find('=');
        auto key = trim(eq == std::string::npos ? line : line.substr(0, eq));
        if (key.empty() || key[0] == '#' || own_keys.contains(key)) {
          continue;
        }
        out << trim(line) << '\n';
      }

      out << "port = " << config::sunshine.port + 1000 * (screen - 1) << '\n';
      out << "sunshine_name = " << config::nvhttp.sunshine_name << " Screen " << screen << '\n';
      out << "file_state = " << (dir / ("sunshine_state_" + suffix + ".json")).string() << '\n';
      out << "credentials_file = " << fs::absolute(config::sunshine.credentials_file).string() << '\n';
      out << "paired_devices_file = " << fs::absolute(config::nvhttp.file_state).string() << '\n';
      out << "extra_screen_index = " << screen << '\n';  // its own virtual display, not the main one's
      out << "log_path = " << (dir / ("sunshine_" + suffix + ".log")).string() << '\n';
      out << "headless_mode = enabled\n";  // always its own virtual display
      out << "stream_audio = disabled\n";  // only the main screen plays sound
      out << "system_tray = disabled\n";
      out << "extra_screens = 0\n";
      out << "pen_virtual_tablet_host = disabled\n";  // forward to the main instance's tablet
      out << "pen_virtual_tablet_desktop = enabled\n";

      auto path = dir / ("sunshine_" + suffix + ".conf");
      std::ofstream(path, std::ios::trunc) << out.str();
      return path;
    }

    constexpr const wchar_t *FIREWALL_RULE = L"Apollo extra screens";

    /// Run netsh hidden and wait for it
    DWORD run_netsh(const std::wstring &args) {
      wchar_t system_dir[MAX_PATH];
      GetSystemDirectoryW(system_dir, MAX_PATH);
      std::wstring exe = std::wstring(system_dir) + L"\\netsh.exe";
      std::wstring cmd = L"\"" + exe + L"\" " + args;

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi {};
      if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return GetLastError();
      }
      WaitForSingleObject(pi.hProcess, 15000);
      DWORD code = 1;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      return code;
    }

    /**
     * Inbound firewall rules for exactly the extra screens' ports (each screen uses its base
     * port -5 .. +21, like the main instance). Some managed firewalls don't honour Apollo's
     * per-program rule for new ports, so allow the ports themselves. Replaced on every start,
     * removed when extra screens are turned off.
     */
    void update_firewall(int count) {
      run_netsh(L"advfirewall firewall delete rule name=\"" + std::wstring(FIREWALL_RULE) + L"\"");
      if (count <= 0) {
        return;
      }

      std::wstring ports;
      for (int n = 1; n <= count; n++) {
        int base = config::sunshine.port + 1000 * n;
        ports += (ports.empty() ? L"" : L",") + std::to_wstring(base - 5) + L"-" + std::to_wstring(base + 21);
      }
      for (const wchar_t *protocol : {L"TCP", L"UDP"}) {
        auto code = run_netsh(L"advfirewall firewall add rule name=\"" + std::wstring(FIREWALL_RULE) +
                              L"\" dir=in action=allow protocol=" + protocol + L" localport=" + ports);
        if (code != 0) {
          BOOST_LOG(warning) << "Extra screens: couldn't add the firewall rule ("sv << code << "); other PCs may not reach them"sv;
        }
      }
      BOOST_LOG(info) << "Extra screens: firewall allows ports "sv << platf::to_utf8(ports);
    }

    bool launch(child_t &child) {
      wchar_t exe[MAX_PATH];
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + child.config_path + L"\"";
      auto dir = fs::path(exe).parent_path().wstring();

      // Capture the child's console output (the virtual display driver helpers only printf)
      // next to its log: config\sunshine_screenN.console.log. Only that handle is inherited.
      auto console_path = (fs::path(child.config_path).parent_path() /
                           ("sunshine_screen" + std::to_string(child.screen) + ".console.log")).wstring();
      SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
      HANDLE console = CreateFileW(console_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);

      STARTUPINFOEXW si {};
      si.StartupInfo.cb = sizeof(si);
      SIZE_T attr_size = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
      std::vector<uint8_t> attr_buf(attr_size);
      bool inherit = false;
      if (console != INVALID_HANDLE_VALUE) {
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST) attr_buf.data();
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) &&
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &console, sizeof(console), nullptr, nullptr)) {
          si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
          si.StartupInfo.hStdOutput = console;
          si.StartupInfo.hStdError = console;
          inherit = true;
        } else {
          si.lpAttributeList = nullptr;
        }
      }

      PROCESS_INFORMATION pi {};
      BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, inherit ? TRUE : FALSE,
                               CREATE_NO_WINDOW | CREATE_SUSPENDED | (inherit ? EXTENDED_STARTUPINFO_PRESENT : 0),
                               nullptr, dir.c_str(), (LPSTARTUPINFOW) &si, &pi);
      DWORD create_error = GetLastError();
      if (si.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
      }
      if (console != INVALID_HANDLE_VALUE) {
        CloseHandle(console);  // the child has its own copy
      }
      if (!ok) {
        BOOST_LOG(error) << "Extra screen "sv << child.screen << ": couldn't start ("sv << create_error << ')';
        return false;
      }
      AssignProcessToJobObject(g_job, pi.hProcess);  // ends together with this process
      ResumeThread(pi.hThread);
      CloseHandle(pi.hThread);
      child.process = pi.hProcess;
      BOOST_LOG(info) << "Extra screen "sv << child.screen << ": started on port "sv
                      << config::sunshine.port + 1000 * (child.screen - 1);
      return true;
    }

    void monitor() {
      while (true) {
        std::vector<HANDLE> handles {g_stop_event};
        for (auto &c : g_children) {
          if (c.process) {
            handles.push_back(c.process);
          }
        }
        DWORD r = WaitForMultipleObjects((DWORD) handles.size(), handles.data(), FALSE, 5000);
        if (r == WAIT_OBJECT_0) {
          return;
        }
        // Restart any extra screen that exited (or never started)
        for (auto &c : g_children) {
          if (c.process && WaitForSingleObject(c.process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(c.process, &code);
            CloseHandle(c.process);
            c.process = nullptr;
            BOOST_LOG(warning) << "Extra screen "sv << c.screen << ": exited ("sv << code << "), restarting"sv;
          }
          if (!c.process) {
            if (WaitForSingleObject(g_stop_event, 2000) == WAIT_OBJECT_0) {
              return;
            }
            launch(c);
          }
        }
      }
    }
  }  // namespace

  void start() {
    int count = config::nvhttp.extra_screens;
    if (config::nvhttp.extra_screen_index > 0 || g_job) {
      return;  // an extra screen itself, or already started
    }

    // Open (or, when turned off, close) exactly the extra screens' ports
    update_firewall(count);
    if (count <= 0) {
      return;
    }

    // With several screens, one virtual tablet spans them all
    config::input.pen_virtual_tablet_desktop = true;

    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    for (int i = 0; i < count; i++) {
      child_t child;
      child.screen = i + 2;
      try {
        child.config_path = write_config(child.screen).wstring();
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Extra screen "sv << child.screen << ": couldn't write its config: "sv << e.what();
        continue;
      }
      launch(child);
      g_children.push_back(child);
    }

    g_monitor = std::thread(monitor);
  }

  void stop() {
    if (!g_job) {
      return;
    }
    SetEvent(g_stop_event);
    if (g_monitor.joinable()) {
      g_monitor.join();
    }
    for (auto &c : g_children) {
      if (c.process) {
        TerminateProcess(c.process, 0);
        CloseHandle(c.process);
      }
    }
    g_children.clear();
    CloseHandle(g_job);
    CloseHandle(g_stop_event);
    g_job = nullptr;
    g_stop_event = nullptr;
  }
}  // namespace platf::extra_screens
