/**
 * @file src/platform/windows/extra_screens.h
 * @brief Extra screens: companion Apollo instances, each streaming its own virtual display.
 */
#pragma once

namespace platf::extra_screens {
  /**
   * @brief Start the configured number of extra screens (config `extra_screens`).
   * @details Each extra screen is a child Apollo process with its own port (+1000 per screen),
   * its own virtual display (headless mode), no audio and no tray icon. It trusts the devices
   * paired with this instance, forwards pen input to this instance's virtual tablet, and is
   * restarted if it exits. Children end together with this process. No-op when this process is
   * itself an extra screen or none are configured.
   */
  void start();

  /**
   * @brief Stop the extra screens.
   */
  void stop();
}  // namespace platf::extra_screens
