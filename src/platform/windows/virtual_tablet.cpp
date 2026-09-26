/**
 * @file src/platform/windows/virtual_tablet.cpp
 * @brief Built-in virtual Wacom tablet (Cintiq 22) presented to Windows over USB/IP.
 *
 * The tablet is a USB/IP server on 127.0.0.1 that answers exactly like a Wacom Cintiq 22:
 * captured descriptors, the Wacom driver's feature-report handshake answered with the real
 * device's replies, and pen state encoded as the device's native report 0x10 (or the generic
 * report 0x06 until the driver switches the tablet to its native mode). usbip-win2 attaches it,
 * the real Wacom driver binds, and applications get full pressure, tilt, buttons, hover and
 * Wintab. Port of vwacom.py.
 */
// platform includes
#include <winsock2.h>  // must precede Windows.h
#include <ws2tcpip.h>
#include <Windows.h>

// standard includes
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
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
    constexpr uint32_t PEN_SERIAL_LO = 0x97800A01, PEN_SERIAL_HI = 0x00100842;  // Pro Pen 2
    constexpr uint16_t TOOL_PRO_PEN2 = 0x0842, TOOL_PRO_PEN2_ERASER = 0x084A;
    constexpr const char *SERIAL = "9GQ00Y1003861";
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
      bool alive = true;  // guarded by g_mutex
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    pen_state_t g_pen;
    std::deque<std::vector<uint8_t>> g_reports;
    std::shared_ptr<session_t> g_session;
    uint8_t g_data_mode = 1;
    std::map<uint8_t, std::vector<uint8_t>> g_features;

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
      std::vector<uint8_t> serial_report {0x14};
      for (size_t i = 0; i < 13; i++) {
        serial_report.push_back(i < std::strlen(SERIAL) ? SERIAL[i] : 0);
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
    void queue_report_locked() {
      auto report = g_data_mode == 2 ? g_pen.report10() : g_pen.report06();

      // If the host fell behind, the newest position is all that matters: replace a queued
      // report that differs only in motion (same report ID and tip/button/eraser/range flags)
      // instead of lining up stale positions. Presses, releases and button changes always
      // get their own report, so nothing is lost; when the host keeps up this never triggers.
      if (!g_reports.empty()) {
        auto &last = g_reports.back();
        if (last.size() == report.size() && last.size() > 1 && last[0] == report[0] && last[1] == report[1]) {
          last = std::move(report);
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
      uint16_t w_value = get16le(setup + 2), w_length = get16le(setup + 6);
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
                return reply(descriptors::device, sizeof(descriptors::device));
              }
              if (dtype == 0x02) {
                return reply(descriptors::config, sizeof(descriptors::config));
              }
              if (dtype == 0x22) {
                return reply(descriptors::hid_report, sizeof(descriptors::hid_report));
              }
              if (dtype == 0x21) {
                // The 9-byte HID descriptor inside the configuration descriptor
                for (size_t i = 0; i + 1 < sizeof(descriptors::config); i += descriptors::config[i]) {
                  if (descriptors::config[i + 1] == 0x21) {
                    return reply(descriptors::config + i, descriptors::config[i]);
                  }
                  if (descriptors::config[i] == 0) {
                    break;
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
                  return reply(descriptors::string_manufacturer, sizeof(descriptors::string_manufacturer));
                }
                if (index == 2) {
                  return reply(descriptors::string_product, sizeof(descriptors::string_product));
                }
                if (index == 3) {
                  auto s = string_descriptor(SERIAL);
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
              } else if (g_features.find(report_id) == g_features.end()) {
                g_features[report_id] = out;
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
              if (report_type == 1 && (report_id == 0x10 || report_id == 0x06)) {
                auto r = report_id == 0x10 ? g_pen.report10() : g_pen.report06();
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
      const uint8_t *dev = descriptors::device;
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
      put8(v, 1);  // bNumInterfaces
      return v;
    }

    void send_ret_submit(session_t &s, uint32_t seqnum, uint32_t devid, uint32_t direction, uint32_t ep, int32_t status, const std::vector<uint8_t> &data) {
      std::vector<uint8_t> v;
      put32be(v, RET_SUBMIT);
      put32be(v, seqnum);
      put32be(v, devid);
      put32be(v, direction);
      put32be(v, ep);
      put32be(v, (uint32_t) status);
      put32be(v, (uint32_t) data.size());
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
          } else {
            send_ret_submit(*s, seqnum, devid, direction, ep, EPIPE_STATUS, {});
          }
        } else if (cmd == CMD_UNLINK) {
          uint32_t target = get32be(hdr + 20);
          int32_t status = 0;
          {
            std::lock_guard lg(g_mutex);
            auto it = std::find_if(s->pending_in.begin(), s->pending_in.end(), [&](const urb_t &u) {
              return u.seqnum == target;
            });
            if (it != s->pending_in.end()) {
              s->pending_in.erase(it);
              status = ECONNRESET_STATUS;
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
        // Interface: class/subclass/protocol from the interface descriptor inside the config
        put8(v, descriptors::config[9 + 5]);
        put8(v, descriptors::config[9 + 6]);
        put8(v, descriptors::config[9 + 7]);
        put8(v, 0);
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
        BOOST_LOG(info) << "Virtual tablet: attached (Wacom Cintiq 22, serial "sv << SERIAL << ')';
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

    void attach_to_windows() {
      // After an Apollo restart usbip-win2 may already be reconnecting on its own
      std::this_thread::sleep_for(3s);
      if (g_stopping) {
        return;
      }

      std::string out;
      DWORD code = 0;
      std::string ours = "usbip://127.0.0.1:" + std::to_string(USBIP_TCP_PORT) + "/" + BUSID;
      if (!run_usbip(L"port", out, code)) {
        BOOST_LOG(warning) << "Virtual tablet: usbip-win2 not found ("sv << "C:\\Program Files\\USBip\\usbip.exe"sv
                           << "); install it from https://github.com/vadimgrn/usbip-win2 so Windows can see the tablet"sv;
        return;
      }
      if (out.find(ours) != std::string::npos) {
        BOOST_LOG(info) << "Virtual tablet: already plugged in"sv;
        return;
      }

      out.clear();
      auto args = L"-t " + std::to_wstring(USBIP_TCP_PORT) + L" attach -r 127.0.0.1 -b " + std::wstring(BUSID, BUSID + std::strlen(BUSID));
      run_usbip(args, out, code);
      if (code == 0) {
        BOOST_LOG(info) << "Virtual tablet: plugged in via usbip-win2"sv;
      } else {
        BOOST_LOG(warning) << "Virtual tablet: usbip attach failed ("sv << code << "): "sv << out;
      }
    }
  }  // namespace

  void start() {
    if (g_running.exchange(true)) {
      return;
    }
    g_stopping = false;

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

    BOOST_LOG(info) << "Virtual tablet: serving Wacom Cintiq 22 on 127.0.0.1:"sv << USBIP_TCP_PORT;
    std::thread(attach_to_windows).detach();
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
    g_running = false;
  }

  bool running() {
    return g_running;
  }

  void submit(const pen_input_t &pen, float x, float y) {
    std::lock_guard lg(g_mutex);
    g_pen.apply(pen, x, y);
    queue_report_locked();
  }
}  // namespace platf::virtual_tablet
