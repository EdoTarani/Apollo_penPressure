/**
 * @file src/platform/windows/virtual_tablet.h
 * @brief Built-in virtual Wacom tablet (Cintiq 22) presented to Windows over USB/IP.
 */
#pragma once

#include "src/platform/common.h"

namespace platf::virtual_tablet {
  /**
   * @brief Start the virtual tablet and plug it into Windows.
   * @details Serves a USB/IP device on 127.0.0.1 that is byte-for-byte a Wacom Cintiq 22
   * (descriptors and the Wacom driver's handshake answers captured from real hardware), then
   * attaches it with usbip-win2 so the real Wacom driver binds to it. Also listens on
   * udp 127.0.0.1:pen_virtual_tablet_port for pen events from other Apollo instances.
   * Safe to call more than once.
   */
  void start();

  /**
   * @brief Stop serving the tablet (the Wacom driver sees it unplugged).
   */
  void stop();

  /**
   * @brief True while this process serves the tablet.
   */
  bool running();

  /**
   * @brief Feed a pen event.
   * @param pen The pen event from the client.
   * @param x Horizontal position on the tablet area, 0..1.
   * @param y Vertical position on the tablet area, 0..1.
   */
  void submit(const pen_input_t &pen, float x, float y);
}  // namespace platf::virtual_tablet
