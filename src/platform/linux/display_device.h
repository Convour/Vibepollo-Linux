/**
 * @file src/platform/linux/display_device.h
 * @brief Linux virtual-display automation driven by KDE's `kscreen-doctor`.
 *
 * This is deliberately not a port of the Windows display-helper subsystem
 * (apply/revert/watchdog/topology capture/EDID-refresh-support). It formalizes
 * the hand-validated kscreen-doctor workflow documented in
 * docs/linux/LEARNINGS.md (S11, S16-18) into native code: shell out to
 * kscreen-doctor to toggle the EDID-injected virtual output that the user
 * already sets up out-of-band (kernel params + initramfs, documentation-only,
 * not automated here).
 */
#pragma once

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace platf::display_device_linux {

  /**
   * @brief Whether the `kscreen-doctor` binary is available on PATH.
   *
   * Drives graceful degradation: on non-KDE compositors (or KDE without
   * kscreen installed) apply/revert become no-ops rather than failing the
   * stream.
   */
  bool kscreen_doctor_available();

  /**
   * @brief Apply the session's display configuration (parsed via
   * display_device::parse_configuration()) by driving kscreen-doctor.
   *
   * Snapshots the pre-apply output arrangement so revert_session_display()
   * can restore it precisely. Honors config::video_t::dd.configuration_option
   * (VerifyOnly/EnsureActive/EnsurePrimary/EnsureOnlyDisplay) exactly like the
   * Windows path does - this module does not independently decide whether to
   * disable other outputs, it only carries out what the user already
   * configured.
   *
   * @return true if configuration is disabled, kscreen-doctor is unavailable
   *         (graceful no-op), or the configuration was applied successfully.
   *         false if configuration was requested but parsing or the
   *         kscreen-doctor invocation failed.
   */
  bool apply_session_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Restore the output arrangement captured by the most recent
   * successful apply_session_display() call.
   *
   * No-op (returns true) if there is nothing to restore.
   */
  bool revert_session_display();

}  // namespace platf::display_device_linux
