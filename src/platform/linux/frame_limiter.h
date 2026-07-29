/**
 * @file src/platform/linux/frame_limiter.h
 * @brief Linux frame-limiter env-var injection (MangoHud config poke + NVIDIA vsync toggle).
 *
 * Unlike the Windows RTSS/NVCP integration (src/platform/windows/frame_limiter.h), this is not a
 * daemon-managed external tool with a start/stop lifecycle - MangoHud and the NVIDIA `__GL_*`
 * driver env vars are plain per-process environment, set once when the game process is spawned.
 * There is nothing to tear down when the process exits, so there are no
 * streaming_start/streaming_stop/prepare_launch equivalents here.
 */
#pragma once

#include "src/boost_process_shim.h"
#include "src/framegen_policy.h"

namespace bp = boost_process_shim;

namespace platf::frame_limiter_linux {

  /**
   * @brief Injects frame-limiter env vars into `env` for the about-to-be-launched game process,
   * based on config::frame_limiter and the computed stream-start policy.
   *
   * No-op if config::frame_limiter.enable is false. Appends to (never overwrites) any
   * pre-existing MANGOHUD_CONFIG value already present in `env`, since the launched app or the
   * user's own launch-command wrapper may already set one.
   *
   * Important: setting MANGOHUD_CONFIG only has an effect if the launched process actually runs
   * under MangoHud (e.g. the user's app command is `mangohud %command%`, or MANGOHUD=1/LD_PRELOAD
   * is already set some other way). This function does not force-inject MangoHud via LD_PRELOAD.
   */
  void apply_launch_env(bp::environment &env, const framegen::stream_start_policy_t &policy);

}  // namespace platf::frame_limiter_linux
