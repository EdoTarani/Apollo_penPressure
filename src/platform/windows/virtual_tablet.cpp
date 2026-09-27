/**
 * @file src/platform/windows/virtual_tablet.cpp
 * @brief Built-in virtual Wacom tablet (Intuos Pro M or Cintiq 22) presented to Windows over USB/IP.
 *
 * The tablet is a USB/IP server on 127.0.0.1 that answers like a real Wacom: its descriptors,
 * the Wacom driver's feature-report handshake, and pen state encoded as the device's native
 * report (or the generic report 6 until the driver switches the tablet to its native mode).
 * usbip-win2 attaches it, the real Wacom driver binds, and applications get full pressure,
 * tilt, buttons, hover and Wintab. Port of vwacom.py.
 *
 * - Intuos Pro M (3rd gen, PTK-670; the default): a pen tablet, so the Wacom driver maps it to
 *   the whole desktop (every screen) with no setup, and it never pairs with, or shares settings
 *   with, a real Cintiq on the same PC. Descriptors from the public linuxwacom recording.
 * - Cintiq 22: a pen display, captured from the real device. Wacom ties it to one monitor
 *   (or All displays) and pairs it with a real Cintiq 22's screen if there is one.
 */
// platform includes
#include <winsock2.h>  // must precede Windows.h
#include <ws2tcpip.h>
#include <Windows.h>

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "virtual_tablet.h"
#include "virtual_tablet_descriptors.h"
#include "virtual_tablet_descriptors_intuos.h"

namespace platf::virtual_tablet {
  using namespace std::literals;

  namespace {
    // ---------------------------------------------------------------- USB/IP
    constexpr uint16_t USBIP_VERSION = 0x0111;
    constexpr uint16_t OP_REQ_DEVLIST = 0x8005, OP_REP_DEVLIST = 0x0005;
    constexpr uint16_t OP_REQ_IMPORT = 0x8003, OP_REP_IMPORT = 0x0003;
    constexpr uint32_t CMD_SUBMIT = 1, CMD_UNLINK = 2, RET_SUBMIT = 3, RET_UNLINK = 4;
    constexpr uint32_t DIR_OUT = 0, DIR_IN = 1;
    constexpr uint32_t ST_OK = 0, ST_NODEV = 4;
    constexpr int32_t EPIPE_STATUS = -32, ECONNRESET_STATUS = -104;
    constexpr uint16_t USBIP_TCP_PORT = 47100;  ///< Our own port, so a usbipd on 3240 never conflicts
    constexpr const char *BUSID = "1-1";

    // ---------------------------------------------------------------- the tablet
    constexpr int X_MAX = 96012, Y_MAX = 54358, P_MAX = 8191, DIST_MAX = 63;  // report 0x10
    constexpr int X_MAX_GENERIC = 32767, Y_MAX_GENERIC = 32767, P_MAX_GENERIC = 2047;  // report 0x06
    // A Pro Pen 2 of its own: the captured pen's serial (0x97800A01) made the Wacom driver ignore
    // the virtual pen once the real pen had been on a real tablet
    constexpr uint32_t PEN_SERIAL_LO = 0x5157A0D1, PEN_SERIAL_HI = 0x00100842;
    constexpr uint16_t TOOL_PRO_PEN2 = 0x0842, TOOL_PRO_PEN2_ERASER = 0x084A;
    constexpr const char *SERIAL = "9GQ00Y1003861";

    // Intuos Pro M (3rd gen): report 30 (native) and report 6 (generic) ranges. The Wacom driver
    // maps the active area 397 counts inside each edge to the whole desktop (measured).
    constexpr int IPM_X_MAX = 52600, IPM_Y_MAX = 29600, IPM_P_MAX = 8191, IPM_DIST_MAX = 255, IPM_MARGIN = 397;
    constexpr int IPM_X_MAX_GENERIC = 26300, IPM_Y_MAX_GENERIC = 14800, IPM_P_MAX_GENERIC = 4095;
    constexpr const char *IPM_SERIAL = "4HHS1K1000713";
    constexpr uint8_t PEN_BUTTON_TERTIARY = 0x04;  // LI_PEN_BUTTON_TERTIARY
    constexpr size_t MAX_QUEUED_REPORTS = 16;  // motion is merged, so this only holds state changes

    // ---------------------------------------------------------------- byte helpers
    void put8(std::vector<uint8_t> &v, uint8_t x) {
      v.push_back(x);
    }

    void put16be(std::vector<uint8_t> &v, uint16_t x) {
      v.push_back(x >> 8);
      v.push_back(x & 0xFF);
    }

    void put32be(std::vector<uint8_t> &v, uint32_t x) {
      for (int s = 24; s >= 0; s -= 8) {
        v.push_back((x >> s) & 0xFF);
      }
    }

    void put16le(std::vector<uint8_t> &v, uint16_t x) {
      v.push_back(x & 0xFF);
      v.push_back(x >> 8);
    }

    void put24le(std::vector<uint8_t> &v, uint32_t x) {
      v.push_back(x & 0xFF);
      v.push_back((x >> 8) & 0xFF);
      v.push_back((x >> 16) & 0xFF);
    }

    void put32le(std::vector<uint8_t> &v, uint32_t x) {
      for (int s = 0; s <= 24; s += 8) {
        v.push_back((x >> s) & 0xFF);
      }
    }

    uint32_t get32be(const uint8_t *p) {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    uint16_t get16le(const uint8_t *p) {
      return uint16_t(p[0] | (p[1] << 8));
    }

    bool recv_exact(SOCKET s, uint8_t *buf, int n) {
      int got = 0;
      while (got < n) {
        int r = ::recv(s, (char *) buf + got, n - got, 0);
        if (r <= 0) {
          return false;
        }
        got += r;
      }
      return true;
    }

    bool send_all(SOCKET s, const std::vector<uint8_t> &data) {
      size_t sent = 0;
      while (sent < data.size()) {
        int r = ::send(s, (const char *) data.data() + sent, (int) (data.size() - sent), 0);
        if (r <= 0) {
          return false;
        }
        sent += r;
      }
      return true;
    }

    // ---------------------------------------------------------------- pen state
    struct pen_state_t {
      bool in_range = false;
      bool tip = false;
      uint8_t tool = LI_TOOL_TYPE_PEN;
      uint8_t buttons = 0;
      float x = 0.5f;
      float y = 0.5f;
      float pressure = 0.0f;
      float distance = 1.0f;
      int tilt_x = 0;
      int tilt_y = 0;

      void apply(const pen_input_t &pen, float px, float py) {
        if (pen.toolType == LI_TOOL_TYPE_PEN || pen.toolType == LI_TOOL_TYPE_ERASER) {
          tool = pen.toolType;
        }
        if (pen.eventType == LI_TOUCH_EVENT_BUTTON_ONLY) {
          buttons = pen.penButtons;
          return;
        }
        if (pen.eventType == LI_TOUCH_EVENT_HOVER_LEAVE || pen.eventType == LI_TOUCH_EVENT_CANCEL || pen.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
          in_range = false;
          tip = false;
          pressure = 0.0f;
          buttons = 0;
          return;
        }

        in_range = true;
        x = std::clamp(px, 0.0f, 1.0f);
        y = std::clamp(py, 0.0f, 1.0f);
        buttons = pen.penButtons;
        float pod = pen.pressureOrDistance;

        if (pen.eventType == LI_TOUCH_EVENT_DOWN || (pen.eventType == LI_TOUCH_EVENT_MOVE && tip)) {
          // Pressure 0 means "unknown": keep the last one while moving, a light touch on first contact
          if (pod > 0.0f) {
            pressure = std::clamp(pod, 0.0f, 1.0f);
          } else if (pen.eventType == LI_TOUCH_EVENT_DOWN) {
            pressure = 0.01f;
          }
          tip = true;
          distance = 0.0f;
        } else if (pen.eventType == LI_TOUCH_EVENT_UP) {
          tip = false;
          pressure = 0.0f;
          distance = 0.05f;
        } else {
          // Hover (or a move that never touched): distance 0 means "unknown"
          tip = false;
          pressure = 0.0f;
          distance = pod > 0.0f ? std::clamp(pod, 0.0f, 1.0f) : 0.3f;
        }

        if (pen.tilt != LI_TILT_UNKNOWN && pen.rotation != LI_ROT_UNKNOWN) {
          // Same polar -> X/Y conversion as the Windows Ink path in pen_update()
          double r = pen.rotation * M_PI / 180.0;
          double t = pen.tilt * M_PI / 180.0;
          tilt_x = (int) std::lround(std::atan2(std::sin(-r) * std::sin(t), std::cos(t)) * 180.0 / M_PI);
          tilt_y = (int) std::lround(std::atan2(std::cos(-r) * std::sin(t), std::cos(t)) * 180.0 / M_PI);
        } else if (pen.tilt == LI_TILT_UNKNOWN) {
          tilt_x = tilt_y = 0;
        }
      }

      uint8_t flags(bool sense) const {
        uint8_t f = sense ? 0x40 : 0x00;
        if (in_range) {
          f |= 0x20;  // in range
          if (tool == LI_TOOL_TYPE_ERASER) {
            f |= 0x10;  // invert
            if (tip) {
              f |= 0x08;  // eraser contact
            }
          } else if (tip) {
            f |= 0x01;  // tip
          }
          if (buttons & LI_PEN_BUTTON_PRIMARY) {
            f |= 0x02;  // barrel switch 1
          }
          if (buttons & LI_PEN_BUTTON_SECONDARY) {
            f |= 0x04;  // barrel switch 2
          }
        }
        return f;
      }

      uint8_t dist() const {
        return tip ? 0 : in_range ? (uint8_t) (distance * DIST_MAX) : DIST_MAX;
      }

      /// The tablet's native report (after the driver sets DataMode 2)
      std::vector<uint8_t> report10() const {
        std::vector<uint8_t> r;
        put8(r, 0x10);
        put8(r, flags(true));
        put24le(r, (uint32_t) (x * X_MAX));
        put24le(r, (uint32_t) (y * Y_MAX));
        put16le(r, tip ? (uint16_t) (pressure * P_MAX) : 0);
        put8(r, (uint8_t) (int8_t) std::clamp(tilt_x, -64, 63));
        put8(r, (uint8_t) (int8_t) std::clamp(tilt_y, -64, 63));
        put16le(r, 0);  // twist
        put16le(r, 0);
        put8(r, dist());
        put32le(r, in_range ? PEN_SERIAL_LO : 0);
        put32le(r, in_range ? PEN_SERIAL_HI : 0);
        put16le(r, in_range ? (tool == LI_TOOL_TYPE_ERASER ? TOOL_PRO_PEN2_ERASER : TOOL_PRO_PEN2) : 0);
        return r;
      }

      // ---------------------------------------------------------- Intuos Pro M
      /// Tip, 3 side switches, eraser, invert, in range (+ sense, report 30 only)
      uint8_t ipm_flags() const {
        uint8_t f = 0;
        if (in_range) {
          f |= 0x40;  // in range
          if (tool == LI_TOOL_TYPE_ERASER) {
            f |= 0x20;  // invert
            if (tip) {
              f |= 0x10;  // eraser contact
            }
          } else if (tip) {
            f |= 0x01;  // tip
          }
          if (buttons & LI_PEN_BUTTON_PRIMARY) {
            f |= 0x02;
          }
          if (buttons & LI_PEN_BUTTON_SECONDARY) {
            f |= 0x04;
          }
          if (buttons & PEN_BUTTON_TERTIARY) {
            f |= 0x08;
          }
        }
        return f;
      }

      /// Native report 30 (after the driver sets DataMode 2), 34 bytes. The sense bit stays set and
      /// the pen's identity stays in the report when it leaves: without them Wacom_Tablet.exe
      /// crashed or ignored the tablet afterwards. Scan time: real time in 100 us units.
      std::vector<uint8_t> report30(uint16_t scan_time, uint16_t seq) const {
        std::vector<uint8_t> r;
        put8(r, 0x1E);
        put8(r, in_range ? 1 : 0);  // contact count
        put8(r, ipm_flags() | 0x80);  // + sense
        put24le(r, (uint32_t) (IPM_MARGIN + x * (IPM_X_MAX - 2 * IPM_MARGIN)));
        put24le(r, (uint32_t) (IPM_MARGIN + y * (IPM_Y_MAX - 2 * IPM_MARGIN)));
        put16le(r, tip ? (uint16_t) (pressure * IPM_P_MAX) : 0);
        put16le(r, (uint16_t) (int16_t) std::clamp(tilt_x, -90, 90));
        put16le(r, (uint16_t) (int16_t) std::clamp(tilt_y, -90, 90));
        put16le(r, 0);  // twist
        put16le(r, 0);
        put8(r, tip ? 0 : in_range ? (uint8_t) (distance * IPM_DIST_MAX) : IPM_DIST_MAX);
        put32le(r, PEN_SERIAL_LO);
        put32le(r, PEN_SERIAL_HI);
        put16le(r, tool == LI_TOOL_TYPE_ERASER ? TOOL_PRO_PEN2_ERASER : TOOL_PRO_PEN2);
        put16le(r, scan_time);
        put16le(r, seq);
        return r;
      }

      /// Generic HID pen report 6 (before the driver switches modes), 18 bytes
      std::vector<uint8_t> report06_ipm(uint16_t scan_time, uint16_t seq) const {
        std::vector<uint8_t> r;
        put8(r, 0x06);
        put8(r, in_range ? 1 : 0);
        put8(r, ipm_flags() & 0x7F);
        put16le(r, (uint16_t) (x * IPM_X_MAX_GENERIC));
        put16le(r, (uint16_t) (y * IPM_Y_MAX_GENERIC));
        put16le(r, tip ? (uint16_t) (pressure * IPM_P_MAX_GENERIC) : 0);
        put16le(r, (uint16_t) (int16_t) std::clamp(tilt_x * 100, -9000, 9000));
        put16le(r, (uint16_t) (int16_t) std::clamp(tilt_y * 100, -9000, 9000));
        put8(r, tip ? 0 : in_range ? (uint8_t) (distance * IPM_DIST_MAX) : IPM_DIST_MAX);
        put16le(r, scan_time);
        put16le(r, seq);
        return r;
      }

      /// Generic HID pen report the tablet sends before the Wacom driver switches modes
      std::vector<uint8_t> report06() const {
        std::vector<uint8_t> r;
        put8(r, 0x06);
        put8(r, flags(false));
        put16le(r, (uint16_t) (x * X_MAX_GENERIC));
        put8(r, 0);
        put16le(r, (uint16_t) (y * Y_MAX_GENERIC));
        put8(r, 0);
        put16le(r, tip ? (uint16_t) (pressure * P_MAX_GENERIC) : 0);
        put8(r, (uint8_t) (int8_t) std::clamp(tilt_x, -90, 90));
        put8(r, (uint8_t) (int8_t) std::clamp(tilt_y, -90, 90));
        put16le(r, 0);
        put16le(r, 0);
        put8(r, dist());
        put32le(r, in_range ? PEN_SERIAL_LO : 0);
        put32le(r, in_range ? PEN_SERIAL_HI : 0);
        put16le(r, in_range ? (tool == LI_TOOL_TYPE_ERASER ? TOOL_PRO_PEN2_ERASER : TOOL_PRO_PEN2) : 0);
        return r;
      }
    };

    // ---------------------------------------------------------------- shared state
    struct urb_t {
      uint32_t seqnum;
      uint32_t devid;
      uint32_t buflen;
    };

    struct session_t {
      SOCKET sock = INVALID_SOCKET;
      std::mutex send_mutex;
      std::deque<urb_t> pending_in;  // interrupt-IN URBs waiting for a report (guarded by g_mutex)
      std::deque<urb_t> pending_other;  // IN URBs on other endpoints: never completed, until unlinked
      bool alive = true;  // guarded by g_mutex
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    pen_state_t g_pen;
    std::deque<std::vector<uint8_t>> g_reports;
    std::shared_ptr<session_t> g_session;
    uint8_t g_data_mode = 1;
    std::map<uint8_t, std::vector<uint8_t>> g_features;
    bool g_intuos = true;  ///< Intuos Pro M (default) or Cintiq 22, from config at start()
    uint16_t g_report_seq = 0;
    std::thread g_battery_thread;

    // The model's descriptors
    struct model_t {
      const uint8_t *device;
      size_t device_size;
      const uint8_t *config;
      size_t config_size;
      const uint8_t *manufacturer;
      size_t manufacturer_size;
      const uint8_t *product;
      size_t product_size;
      const char *serial;
      const char *name;
    };

    model_t model() {
      if (g_intuos) {
        namespace d = descriptors_intuos;
        return {d::device, sizeof(d::device), d::config, sizeof(d::config), d::string_manufacturer, sizeof(d::string_manufacturer),
                d::string_product, sizeof(d::string_product), IPM_SERIAL, "Wacom Intuos Pro M"};
      }
      namespace d = descriptors;
      return {d::device, sizeof(d::device), d::config, sizeof(d::config), d::string_manufacturer, sizeof(d::string_manufacturer),
              d::string_product, sizeof(d::string_product), SERIAL, "Wacom Cintiq 22"};
    }

    /// The HID report descriptor of an interface (nullptr: none)
    const uint8_t *hid_report_descriptor(uint16_t iface, size_t &size) {
      if (g_intuos) {
        if (iface == 0) {
          size = sizeof(descriptors_intuos::hid_report_if0);
          return descriptors_intuos::hid_report_if0;
        }
        if (iface == 2) {
          size = sizeof(descriptors_intuos::hid_report_if2);
          return descriptors_intuos::hid_report_if2;
        }
        return nullptr;
      }
      if (iface == 0) {
        size = sizeof(descriptors::hid_report);
        return descriptors::hid_report;
      }
      return nullptr;
    }

    std::atomic<bool> g_running {false};
    std::atomic<bool> g_stopping {false};
    SOCKET g_listen = INVALID_SOCKET;
    SOCKET g_udp = INVALID_SOCKET;
    std::thread g_accept_thread;
    std::thread g_udp_thread;

    std::vector<uint8_t> string_descriptor(const std::string &ascii) {
      std::vector<uint8_t> d {(uint8_t) (2 + ascii.size() * 2), 0x03};
      for (char c : ascii) {
        put16le(d, (uint8_t) c);
      }
      return d;
    }

    void reset_features() {
      const char *serial = model().serial;
      std::vector<uint8_t> serial_report {0x14};
      for (size_t i = 0; i < 13; i++) {
        serial_report.push_back(i < std::strlen(serial) ? serial[i] : 0);
      }
      // The Wacom driver's reads, answered as the real Cintiq 22 did
      g_features = {
        {0x02, {0x02, 0x01}},
        {0x03, {0x03, 0x00}},
        {0x04, {0x04, 0x00}},
        {0x07, {0x07, 0x02, 0x00, 0x04, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x0C, {0x0C, 0x90, 0x01, 0x90, 0x01, 0x90, 0x01, 0x90, 0x01}},
        {0x14, serial_report},
      };
      g_data_mode = 1;
    }

    /// Queue the current pen state as an interrupt report (caller holds g_mutex)
    // Pen statistics, logged every 2 s while the pen is active ("Virtual tablet: pen ..."):
    // how often and how evenly pen events arrive from the client, and how often the host fell
    // behind (merged). Guarded by g_mutex.
    struct {
      std::chrono::steady_clock::time_point window_start {};
      std::chrono::steady_clock::time_point last_event {};
      int64_t max_gap_ms = 0;
      uint32_t events = 0, merged = 0, delivered = 0;
      float last_x = 0, last_y = 0;  // the latest position sent to the tablet (0..1)
    } g_stats;

    void count_event_locked() {
      auto now = std::chrono::steady_clock::now();
      if (g_stats.last_event.time_since_epoch().count() != 0) {
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_stats.last_event).count();
        if (gap < 500) {
          g_stats.max_gap_ms = std::max<int64_t>(g_stats.max_gap_ms, gap);
        }
      }
      g_stats.last_event = now;
      g_stats.events++;
      if (g_stats.window_start.time_since_epoch().count() == 0) {
        g_stats.window_start = now;
        return;
      }

      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_stats.window_start).count();
      if (elapsed >= 2000) {
        BOOST_LOG(info) << "Virtual tablet: pen "sv << g_stats.events * 1000 / elapsed << " events/s, max gap "sv
                        << g_stats.max_gap_ms << " ms, "sv << g_stats.delivered * 1000 / elapsed << " reports/s to the driver, "sv
                        << g_stats.merged << " merged (host behind)"sv;

        // Mapping check: where the tablet position ends up on screen, and on which monitor
        POINT cursor {};
        GetCursorPos(&cursor);
        MONITORINFOEXW monitor {};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        char mapping[256];
        snprintf(mapping, sizeof(mapping), "Virtual tablet: pen at %.3f,%.3f of the tablet -> cursor %ld,%ld on %ls [%ld,%ld %ldx%ld]",
                 g_stats.last_x, g_stats.last_y, cursor.x, cursor.y, monitor.szDevice, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        BOOST_LOG(info) << mapping;
        g_stats = {};
      }
    }

    /// The Intuos reports' scan time: real time in 100 us units
    uint16_t scan_time_now() {
      auto now = std::chrono::steady_clock::now().time_since_epoch();
      return (uint16_t) (std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 100);
    }

    /// The pen report for the mode the Wacom driver put the tablet in (caller holds g_mutex)
    std::vector<uint8_t> current_report_locked() {
      if (g_intuos) {
        g_report_seq++;
        return g_data_mode == 2 ? g_pen.report30(scan_time_now(), g_report_seq) : g_pen.report06_ipm(scan_time_now(), g_report_seq);
      }
      return g_data_mode == 2 ? g_pen.report10() : g_pen.report06();
    }

    /**
     * An Intuos Pro reports its pad (ExpressKeys, dial buttons, dials) and its battery: the
     * Wacom driver builds its pad state from them, and without it Wacom_Tablet.exe crashed (null
     * object) when the pointer switched between the pen and the mouse. "Nothing pressed, battery
     * full", whenever the driver (re)initializes the tablet. Caller holds g_mutex.
     */
    void queue_pad_state_locked() {
      g_reports.push_back({0x11, 0, 0, 0, 0, 0, 0, 0, 0});  // report 17: pad idle
      g_reports.push_back({0x13, 100 | 0x80, 0, 0, 0, 0, 0, 0, 0});  // report 19: 100 %, powered
      g_cv.notify_all();
    }

    void queue_report_locked() {
      auto report = current_report_locked();

      // If the host fell behind, the newest position is all that matters: replace a queued
      // report that differs only in motion (same report ID and tip/button/eraser/range flags)
      // instead of lining up stale positions. Presses, releases and button changes always
      // get their own report, so nothing is lost; when the host keeps up this never triggers.
      size_t key = g_intuos ? 3 : 2;  // report ID (+ contact count) + flags
      if (!g_reports.empty()) {
        auto &last = g_reports.back();
        if (last.size() == report.size() && last.size() >= key && std::equal(last.begin(), last.begin() + key, report.begin())) {
          last = std::move(report);
          g_stats.merged++;
          g_cv.notify_all();
          return;
        }
      }

      g_reports.push_back(std::move(report));
      while (g_reports.size() > MAX_QUEUED_REPORTS) {
        g_reports.pop_front();  // keep latency bounded
      }
      g_cv.notify_all();
    }

    // ---------------------------------------------------------------- control endpoint
    /// Returns the URB status (0, or EPIPE_STATUS for a STALL) and fills `in` for IN transfers
    int32_t control(const uint8_t *setup, const std::vector<uint8_t> &out, std::vector<uint8_t> &in) {
      uint8_t bm_request_type = setup[0], b_request = setup[1];
      uint16_t w_value = get16le(setup + 2), w_index = get16le(setup + 4), w_length = get16le(setup + 6);
      auto m = model();
      int type = (bm_request_type >> 5) & 3;

      auto reply = [&](const uint8_t *data, size_t size) {
        in.assign(data, data + std::min<size_t>(size, w_length));
        return 0;
      };

      if (type == 0) {  // standard
        switch (b_request) {
          case 6:  // GET_DESCRIPTOR
            {
              uint8_t dtype = w_value >> 8, index = w_value & 0xFF;
              if (dtype == 0x01) {
                return reply(m.device, m.device_size);
              }
              if (dtype == 0x02) {
                return reply(m.config, m.config_size);
              }
              if (dtype == 0x22) {
                size_t size = 0;
                auto report = hid_report_descriptor(w_index, size);
                return report ? reply(report, size) : EPIPE_STATUS;
              }
              if (dtype == 0x21) {
                // The 9-byte HID descriptor of interface w_index, inside the configuration descriptor
                int iface = -1;
                for (size_t i = 0; i + 1 < m.config_size && m.config[i] != 0; i += m.config[i]) {
                  if (m.config[i + 1] == 0x04) {
                    iface = m.config[i + 2];
                  } else if (m.config[i + 1] == 0x21 && iface == w_index) {
                    return reply(m.config + i, m.config[i]);
                  }
                }
                return EPIPE_STATUS;
              }
              if (dtype == 0x03) {
                static const uint8_t langids[] = {0x04, 0x03, 0x09, 0x04};
                if (index == 0) {
                  return reply(langids, sizeof(langids));
                }
                if (index == 1) {
                  return reply(m.manufacturer, m.manufacturer_size);
                }
                if (index == 2) {
                  return reply(m.product, m.product_size);
                }
                if (index == 3) {
                  auto s = string_descriptor(m.serial);
                  return reply(s.data(), s.size());
                }
              }
              return EPIPE_STATUS;  // qualifier, BOS, MS OS descriptors: a full-speed device has none
            }
          case 0:  // GET_STATUS: self-powered, as captured
            {
              static const uint8_t status[] = {0x01, 0x00};
              return reply(status, sizeof(status));
            }
          case 8:  // GET_CONFIGURATION
            {
              static const uint8_t cfg[] = {0x01};
              return reply(cfg, sizeof(cfg));
            }
          case 10:  // GET_INTERFACE
            {
              static const uint8_t alt[] = {0x00};
              return reply(alt, sizeof(alt));
            }
          case 1:  // CLEAR_FEATURE
          case 3:  // SET_FEATURE
          case 9:  // SET_CONFIGURATION
          case 11:  // SET_INTERFACE
            return 0;
          default:
            return EPIPE_STATUS;
        }
      }

      if (type == 1) {  // HID class
        uint8_t report_type = w_value >> 8, report_id = w_value & 0xFF;
        if (w_index != 0) {
          // The Intuos Pro's second HID interface: nothing to report
          return (b_request == 0x0A || b_request == 0x0B || b_request == 0x09) ? 0 : EPIPE_STATUS;
        }
        switch (b_request) {
          case 0x0A:  // SET_IDLE
          case 0x0B:  // SET_PROTOCOL
            return 0;
          case 0x02:  // GET_IDLE
            {
              static const uint8_t idle[] = {0x00};
              return reply(idle, sizeof(idle));
            }
          case 0x03:  // GET_PROTOCOL
            {
              static const uint8_t proto[] = {0x01};
              return reply(proto, sizeof(proto));
            }
          case 0x09:  // SET_REPORT
            {
              if (report_type != 3) {
                return 0;
              }
              if (report_id == 0x83) {
                return EPIPE_STATUS;  // the inbox pen stack's probe; the real tablet stalls it
              }
              std::lock_guard lg(g_mutex);
              if (report_id == 0x02 && out.size() >= 2) {
                if (g_data_mode != out[1]) {
                  BOOST_LOG(info) << "Virtual tablet: Wacom driver set data mode "sv << (int) out[1];
                }
                g_data_mode = out[1];
                g_features[0x02] = {0x02, g_data_mode};
                if (g_intuos && g_data_mode == 2) {
                  queue_pad_state_locked();
                }
              } else {
                if (g_features.find(report_id) == g_features.end()) {
                  g_features[report_id] = out;
                }
                if (report_id == 0x04 && g_intuos) {
                  queue_pad_state_locked();  // a (re)starting driver sets 0x04 first
                }
              }
              return 0;
            }
          case 0x01:  // GET_REPORT
            {
              std::lock_guard lg(g_mutex);
              if (report_type == 3) {
                auto it = g_features.find(report_id);
                if (it == g_features.end()) {
                  return EPIPE_STATUS;
                }
                in.assign(it->second.begin(), it->second.begin() + std::min<size_t>(it->second.size(), w_length));
                return 0;
              }
              if (report_type == 1) {
                std::vector<uint8_t> r;
                if (g_intuos) {
                  if (report_id == 0x1E) {
                    r = g_pen.report30(scan_time_now(), g_report_seq);
                  } else if (report_id == 0x06) {
                    r = g_pen.report06_ipm(scan_time_now(), g_report_seq);
                  }
                } else if (report_id == 0x10) {
                  r = g_pen.report10();
                } else if (report_id == 0x06) {
                  r = g_pen.report06();
                }
                if (r.empty()) {
                  return EPIPE_STATUS;
                }
                in.assign(r.begin(), r.begin() + std::min<size_t>(r.size(), w_length));
                return 0;
              }
              return EPIPE_STATUS;
            }
          default:
            return EPIPE_STATUS;
        }
      }

      return EPIPE_STATUS;  // vendor requests
    }

    // ---------------------------------------------------------------- USB/IP session
    std::vector<uint8_t> usb_device_record() {
      auto m = model();
      const uint8_t *dev = m.device;
      std::vector<uint8_t> v;
      std::string path = "/sys/devices/pci0000:00/0000:00:1d.0/usb1/1-1";
      v.insert(v.end(), path.begin(), path.end());
      v.resize(256, 0);
      std::string busid = BUSID;
      v.insert(v.end(), busid.begin(), busid.end());
      v.resize(256 + 32, 0);
      put32be(v, 1);  // busnum
      put32be(v, 1);  // devnum
      put32be(v, 2);  // full speed
      put16be(v, get16le(dev + 8));  // idVendor
      put16be(v, get16le(dev + 10));  // idProduct
      put16be(v, get16le(dev + 12));  // bcdDevice
      put8(v, dev[4]);
      put8(v, dev[5]);
      put8(v, dev[6]);
      put8(v, 1);  // bConfigurationValue
      put8(v, dev[17]);  // bNumConfigurations
      put8(v, m.config[4]);  // bNumInterfaces
      return v;
    }

    void send_ret_submit(session_t &s, uint32_t seqnum, uint32_t devid, uint32_t direction, uint32_t ep, int32_t status, const std::vector<uint8_t> &data, int64_t actual = -1) {
      std::vector<uint8_t> v;
      put32be(v, RET_SUBMIT);
      put32be(v, seqnum);
      put32be(v, devid);
      put32be(v, direction);
      put32be(v, ep);
      put32be(v, (uint32_t) status);
      put32be(v, (uint32_t) (actual >= 0 ? actual : (int64_t) data.size()));
      put32be(v, 0);
      put32be(v, 0);
      put32be(v, 0);
      v.resize(48, 0);
      v.insert(v.end(), data.begin(), data.end());
      std::lock_guard lg(s.send_mutex);
      send_all(s.sock, v);
    }

    void report_pump(std::shared_ptr<session_t> s) {
      while (true) {
        urb_t urb;
        std::vector<uint8_t> report;
        {
          std::unique_lock lk(g_mutex);
          g_cv.wait(lk, [&] {
            return !s->alive || (!s->pending_in.empty() && !g_reports.empty());
          });
          if (!s->alive) {
            return;
          }
          urb = s->pending_in.front();
          s->pending_in.pop_front();
          report = std::move(g_reports.front());
          g_reports.pop_front();
          g_stats.delivered++;
        }
        if (report.size() > urb.buflen) {
          report.resize(urb.buflen);
        }
        send_ret_submit(*s, urb.seqnum, urb.devid, DIR_IN, 1, 0, report);
      }
    }

    void run_session(std::shared_ptr<session_t> s) {
      std::thread pump(report_pump, s);
      uint8_t hdr[48];
      while (recv_exact(s->sock, hdr, sizeof(hdr))) {
        uint32_t cmd = get32be(hdr), seqnum = get32be(hdr + 4), devid = get32be(hdr + 8);
        uint32_t direction = get32be(hdr + 12), ep = get32be(hdr + 16);

        if (cmd == CMD_SUBMIT) {
          uint32_t buflen = get32be(hdr + 24);
          std::vector<uint8_t> out;
          if (direction == DIR_OUT && buflen) {
            out.resize(buflen);
            if (!recv_exact(s->sock, out.data(), (int) buflen)) {
              break;
            }
          }
          if (ep == 0) {
            std::vector<uint8_t> in;
            int32_t status = control(hdr + 40, out, in);
            if (direction == DIR_IN && in.size() > buflen) {
              in.resize(buflen);
            }
            send_ret_submit(*s, seqnum, devid, direction, ep, status, direction == DIR_IN ? in : std::vector<uint8_t> {});
          } else if (ep == 1 && direction == DIR_IN) {
            std::lock_guard lg(g_mutex);
            s->pending_in.push_back({seqnum, devid, buflen});
            g_cv.notify_all();
          } else if (g_intuos && direction == DIR_IN) {
            // The Intuos Pro's second HID interface: nothing to report, the URB waits until unlinked
            std::lock_guard lg(g_mutex);
            s->pending_other.push_back({seqnum, devid, buflen});
          } else if (g_intuos && direction == DIR_OUT) {
            send_ret_submit(*s, seqnum, devid, direction, ep, 0, {}, (int64_t) out.size());  // vendor bulk OUT: accepted
          } else {
            send_ret_submit(*s, seqnum, devid, direction, ep, EPIPE_STATUS, {});
          }
        } else if (cmd == CMD_UNLINK) {
          uint32_t target = get32be(hdr + 20);
          int32_t status = 0;
          {
            std::lock_guard lg(g_mutex);
            for (auto *pending : {&s->pending_in, &s->pending_other}) {
              auto it = std::find_if(pending->begin(), pending->end(), [&](const urb_t &u) {
                return u.seqnum == target;
              });
              if (it != pending->end()) {
                pending->erase(it);
                status = ECONNRESET_STATUS;
              }
            }
          }
          std::vector<uint8_t> v;
          put32be(v, RET_UNLINK);
          put32be(v, seqnum);
          put32be(v, devid);
          put32be(v, direction);
          put32be(v, ep);
          put32be(v, (uint32_t) status);
          v.resize(48, 0);
          std::lock_guard lg(s->send_mutex);
          send_all(s->sock, v);
        } else {
          break;
        }
      }

      {
        std::lock_guard lg(g_mutex);
        s->alive = false;
        g_cv.notify_all();
      }
      pump.join();
      ::closesocket(s->sock);
      BOOST_LOG(info) << "Virtual tablet: detached"sv;
    }

    void handle_client(SOCKET c) {
      uint8_t hdr[8];
      if (!recv_exact(c, hdr, sizeof(hdr))) {
        ::closesocket(c);
        return;
      }
      uint16_t code = (hdr[2] << 8) | hdr[3];

      if (code == OP_REQ_DEVLIST) {
        std::vector<uint8_t> v;
        put16be(v, USBIP_VERSION);
        put16be(v, OP_REP_DEVLIST);
        put32be(v, ST_OK);
        put32be(v, 1);
        auto dev = usb_device_record();
        v.insert(v.end(), dev.begin(), dev.end());
        // Interfaces: class/subclass/protocol of each interface descriptor inside the config
        auto m = model();
        for (size_t i = 0; i + 1 < m.config_size && m.config[i] != 0; i += m.config[i]) {
          if (m.config[i + 1] == 0x04) {
            put8(v, m.config[i + 5]);
            put8(v, m.config[i + 6]);
            put8(v, m.config[i + 7]);
            put8(v, 0);
          }
        }
        send_all(c, v);
        ::closesocket(c);
        return;
      }

      if (code == OP_REQ_IMPORT) {
        uint8_t busid[32];
        if (!recv_exact(c, busid, sizeof(busid))) {
          ::closesocket(c);
          return;
        }
        std::vector<uint8_t> v;
        put16be(v, USBIP_VERSION);
        put16be(v, OP_REP_IMPORT);
        if (std::strncmp((const char *) busid, BUSID, sizeof(busid)) != 0) {
          put32be(v, ST_NODEV);
          send_all(c, v);
          ::closesocket(c);
          return;
        }
        put32be(v, ST_OK);
        auto dev = usb_device_record();
        v.insert(v.end(), dev.begin(), dev.end());

        auto s = std::make_shared<session_t>();
        s->sock = c;
        {
          std::lock_guard lg(g_mutex);
          // One tablet, one attachment: a new import replaces a stale one
          if (g_session && g_session->alive) {
            ::shutdown(g_session->sock, SD_BOTH);
          }
          g_session = s;
          g_reports.clear();
          reset_features();
        }
        BOOST_LOG(info) << "Virtual tablet: attached ("sv << model().name << ", serial "sv << model().serial << ')';
        if (send_all(c, v)) {
          run_session(s);
        } else {
          ::closesocket(c);
        }
        return;
      }

      ::closesocket(c);
    }

    void accept_loop() {
      while (!g_stopping) {
        SOCKET c = ::accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
          if (g_stopping) {
            break;
          }
          std::this_thread::sleep_for(100ms);
          continue;
        }
        BOOL nodelay = TRUE;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof(nodelay));
        std::thread(handle_client, c).detach();
      }
    }

    // ---------------------------------------------------------------- pen events from other instances
    void udp_loop() {
      char buf[256];
      while (!g_stopping) {
        int n = ::recvfrom(g_udp, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 24) {
          if (n == SOCKET_ERROR && g_stopping) {
            break;
          }
          continue;
        }
        if (std::memcmp(buf, "VWP1", 4) != 0) {
          continue;
        }
        pen_input_t pen {};
        float x, y;
        pen.eventType = (uint8_t) buf[4];
        pen.toolType = (uint8_t) buf[5];
        pen.penButtons = (uint8_t) buf[6];
        std::memcpy(&x, buf + 8, 4);
        std::memcpy(&y, buf + 12, 4);
        std::memcpy(&pen.pressureOrDistance, buf + 16, 4);
        std::memcpy(&pen.rotation, buf + 20, 2);
        pen.tilt = (uint8_t) buf[22];
        submit(pen, x, y);
      }
    }

    // ---------------------------------------------------------------- plugging it in
    std::wstring usbip_exe() {
      wchar_t pf[MAX_PATH];
      DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
      std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(pf) : L"C:\\Program Files";
      return base + L"\\USBip\\usbip.exe";
    }

    /// Run usbip.exe with arguments, capturing its output; returns false if it couldn't run
    bool run_usbip(const std::wstring &args, std::string &output, DWORD &exit_code) {
      auto exe = usbip_exe();
      if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
      }

      SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
      HANDLE read_end, write_end;
      if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return false;
      }
      SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = write_end;
      si.hStdError = write_end;
      PROCESS_INFORMATION pi {};
      std::wstring cmd = L"\"" + exe + L"\" " + args;
      BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
      CloseHandle(write_end);
      if (!ok) {
        CloseHandle(read_end);
        return false;
      }

      char buf[512];
      DWORD got;
      while (ReadFile(read_end, buf, sizeof(buf), &got, nullptr) && got > 0) {
        output.append(buf, got);
      }
      WaitForSingleObject(pi.hProcess, 30000);
      GetExitCodeProcess(pi.hProcess, &exit_code);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      CloseHandle(read_end);
      return true;
    }

    // The tablet is plugged into Windows only while a Moonlight stream runs, so at the office
    // only the real tablet (or Parsec's) is there. One worker applies the latest wish.
    std::mutex g_plug_mutex;
    std::atomic<bool> g_want_plugged {false};

    /// usbip-win2's port number for our tablet ("Port 01: ..." above our usbip:// line), or -1
    int our_usbip_port(const std::string &port_output) {
      std::string ours = "usbip://127.0.0.1:" + std::to_string(USBIP_TCP_PORT) + "/" + BUSID;
      auto at = port_output.find(ours);
      if (at == std::string::npos) {
        return -1;
      }
      auto port = port_output.rfind("Port ", at);
      if (port == std::string::npos) {
        return -1;
      }
      return std::atoi(port_output.c_str() + port + 5);
    }

    /// The screens on the desktop: name, position and size of each
    std::wstring display_layout() {
      std::wstring layout;
      DISPLAY_DEVICEW adapter {};
      adapter.cb = sizeof(adapter);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); i++, adapter.cb = sizeof(adapter)) {
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
          continue;
        }
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0);
        layout += std::wstring(adapter.DeviceName) + L"@" + std::to_wstring(mode.dmPosition.x) + L"," +
                  std::to_wstring(mode.dmPosition.y) + L":" + std::to_wstring(mode.dmPelsWidth) + L"x" +
                  std::to_wstring(mode.dmPelsHeight) + L";";
      }
      return layout;
    }

    /**
     * The Wacom driver maps a pen display with the screen layout it sees when the tablet
     * appears. At a stream's start the screens are still being created and arranged, so wait
     * until the layout has been the same for 4 s (at most 20 s).
     */
    bool wait_for_stable_screens() {
      auto start = std::chrono::steady_clock::now();
      auto stable_since = start;
      auto layout = display_layout();
      while (std::chrono::steady_clock::now() - stable_since < 4s) {
        if (!g_want_plugged || g_stopping) {
          return false;
        }
        if (std::chrono::steady_clock::now() - start > 20s) {
          break;
        }
        std::this_thread::sleep_for(250ms);
        auto now_layout = display_layout();
        if (now_layout != layout) {
          layout = std::move(now_layout);
          stable_since = std::chrono::steady_clock::now();
        }
      }
      return true;
    }

    void apply_plugged() {
      std::lock_guard lg(g_plug_mutex);
      bool want = g_want_plugged;
      if (g_stopping) {
        return;
      }

      std::string out;
      DWORD code = 0;
      if (!run_usbip(L"port", out, code)) {
        BOOST_LOG(warning) << "Virtual tablet: usbip-win2 not found ("sv << "C:\\Program Files\\USBip\\usbip.exe"sv
                           << "); install it from https://github.com/vadimgrn/usbip-win2 so Windows can see the tablet"sv;
        return;
      }
      int port = our_usbip_port(out);

      if (want && port < 0) {
        if (!wait_for_stable_screens()) {
          return;  // the stream ended meanwhile
        }
        out.clear();
        auto args = L"-t " + std::to_wstring(USBIP_TCP_PORT) + L" attach -r 127.0.0.1 -b " + std::wstring(BUSID, BUSID + std::strlen(BUSID));
        run_usbip(args, out, code);
        if (code == 0) {
          BOOST_LOG(info) << "Virtual tablet: plugged in via usbip-win2"sv;
        } else {
          BOOST_LOG(warning) << "Virtual tablet: usbip attach failed ("sv << code << "): "sv << out;
        }
      } else if (!want && port >= 0) {
        out.clear();
        run_usbip(L"detach -p " + std::to_wstring(port), out, code);
        if (code == 0) {
          BOOST_LOG(info) << "Virtual tablet: unplugged (no stream)"sv;
        } else {
          BOOST_LOG(warning) << "Virtual tablet: usbip detach failed ("sv << code << "): "sv << out;
        }
      }
    }
  }  // namespace

  void set_plugged(bool plugged) {
    if (!g_running) {
      return;  // the tablet lives in the main instance
    }
    g_want_plugged = plugged;
    std::thread(apply_plugged).detach();
  }

  void start() {
    if (g_running.exchange(true)) {
      return;
    }
    g_stopping = false;
    g_intuos = config::input.pen_virtual_tablet_model != "cintiq";

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    {
      std::lock_guard lg(g_mutex);
      reset_features();
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only: nothing is exposed to the network

    g_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    addr.sin_port = htons(USBIP_TCP_PORT);
    if (g_listen == INVALID_SOCKET || ::bind(g_listen, (sockaddr *) &addr, sizeof(addr)) != 0 || ::listen(g_listen, 4) != 0) {
      BOOST_LOG(error) << "Virtual tablet: can't listen on 127.0.0.1:"sv << USBIP_TCP_PORT << " ("sv << WSAGetLastError() << ')';
      if (g_listen != INVALID_SOCKET) {
        ::closesocket(g_listen);
        g_listen = INVALID_SOCKET;
      }
      g_running = false;
      return;
    }
    g_accept_thread = std::thread(accept_loop);

    g_udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_port = htons((u_short) config::input.pen_virtual_tablet_port);
    if (g_udp != INVALID_SOCKET && ::bind(g_udp, (sockaddr *) &addr, sizeof(addr)) == 0) {
      g_udp_thread = std::thread(udp_loop);
    } else {
      BOOST_LOG(warning) << "Virtual tablet: can't receive pen events on udp 127.0.0.1:"sv << config::input.pen_virtual_tablet_port
                         << " ("sv << WSAGetLastError() << "); only this instance's pen input will reach the tablet"sv;
      if (g_udp != INVALID_SOCKET) {
        ::closesocket(g_udp);
        g_udp = INVALID_SOCKET;
      }
    }

    BOOST_LOG(info) << "Virtual tablet: serving "sv << model().name << " on 127.0.0.1:"sv << USBIP_TCP_PORT;

    // An Intuos Pro repeats its battery status (every 10 s here, while the pen is away)
    g_battery_thread = std::thread([] {
      for (int tick = 1; !g_stopping; ++tick) {
        std::this_thread::sleep_for(1s);
        std::lock_guard lg(g_mutex);
        if (g_intuos && tick % 10 == 0 && g_data_mode == 2 && !g_pen.in_range && g_session && g_session->alive &&
            g_reports.size() < MAX_QUEUED_REPORTS) {
          g_reports.push_back({0x13, 100 | 0x80, 0, 0, 0, 0, 0, 0, 0});
          g_cv.notify_all();
        }
      }
    });
    // Unplugged until a stream starts (also removes one left plugged in by a previous run:
    // usbip-win2 reconnects that one by itself a few seconds after we start, so look again)
    g_want_plugged = false;
    std::thread([] {
      for (auto delay : {0s, 8s, 20s}) {
        std::this_thread::sleep_for(delay);
        if (g_stopping || g_want_plugged) {
          return;
        }
        apply_plugged();
      }
    }).detach();
  }

  void stop() {
    if (!g_running) {
      return;
    }
    g_stopping = true;
    if (g_listen != INVALID_SOCKET) {
      ::closesocket(g_listen);
      g_listen = INVALID_SOCKET;
    }
    if (g_udp != INVALID_SOCKET) {
      ::closesocket(g_udp);
      g_udp = INVALID_SOCKET;
    }
    {
      std::lock_guard lg(g_mutex);
      if (g_session && g_session->alive) {
        ::shutdown(g_session->sock, SD_BOTH);
      }
    }
    if (g_accept_thread.joinable()) {
      g_accept_thread.join();
    }
    if (g_udp_thread.joinable()) {
      g_udp_thread.join();
    }
    if (g_battery_thread.joinable()) {
      g_battery_thread.join();
    }
    g_running = false;
  }

  bool running() {
    return g_running;
  }

  void submit(const pen_input_t &pen, float x, float y) {
    std::lock_guard lg(g_mutex);
    g_pen.apply(pen, x, y);
    g_stats.last_x = x;
    g_stats.last_y = y;
    queue_report_locked();
    count_event_locked();
  }
}  // namespace platf::virtual_tablet
