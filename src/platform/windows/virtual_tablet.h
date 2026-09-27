/**
 * @file src/platform/windows/virtual_tablet.h
 * @brief Built-in virtual Wacom tablet (Intuos Pro M or Cintiq 22) presented to Windows over USB/IP.
 */
#pragma once

#include "src/platform/common.h"

namespace platf::virtual_tablet {
  /**
   * @brief Start the virtual tablet and plug it into Windows.
   * @details Serves a USB/IP device on 127.0.0.1 that answers like a Wacom Intuos Pro M (the
   * default) or Cintiq 22 (pen_virtual_tablet_model; descriptors from real hardware), which
   * usbip-win2 attaches while a stream runs so the real Wacom driver binds to it. Also listens on
   * udp 127.0.0.1:pen_virtual_tablet_port for pen events from other Apollo instances.
   * Safe to call more than once.
   */
  void start();

  /**
   * @brief True when the tablet can be used on this PC: the Wacom driver (free from wacom.com)
   * and usbip-win2 (installed with Apollo) are there. Otherwise pen input goes through Windows
   * Ink. Checked again every 10 s, so a driver installed later works from the next stream.
   */
  bool available();

  /**
   * @brief Stop serving the tablet (the Wacom driver sees it unplugged).
   */
  void stop();

  /**
   * @brief True while this process serves the tablet.
   */
  bool running();

  /**
   * @brief Plug the tablet into Windows (a stream started) or unplug it (the last client left),
   * in the background. Does nothing in an instance that doesn't serve the tablet.
   */
  void set_plugged(bool plugged);

  /**
   * @brief Feed a pen event.
   * @param pen The pen event from the client.
   * @param x Horizontal position on the tablet area, 0..1.
   * @param y Vertical position on the tablet area, 0..1.
   */
  void submit(const pen_input_t &pen, float x, float y);
}  // namespace platf::virtual_tablet
