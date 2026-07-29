/**
 * @file src/platform/linux/frame_limiter.cpp
 * @brief See frame_limiter.h.
 */
#include "frame_limiter.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "src/config.h"
#include "src/logging.h"

namespace platf::frame_limiter_linux {

  namespace {

    struct resolved_providers_t {
      bool use_mangohud {false};
      bool use_nvidia_env {false};
    };

    std::string normalize(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (ch == '-' || ch == '_' || ch == ' ') {
          continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
      return normalized;
    }

    bool binary_on_path(const char *name) {
      const char *path_env = std::getenv("PATH");
      if (!path_env) {
        return false;
      }

      std::string path {path_env};
      std::size_t start = 0;
      while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
          const auto candidate = dir + "/" + name;
          if (access(candidate.c_str(), X_OK) == 0) {
            return true;
          }
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return false;
    }

    bool mangohud_available() {
      return binary_on_path("mangohud");
    }

    bool nvidia_gpu_present() {
      return std::ifstream("/proc/driver/nvidia/version").good();
    }

    // Resolves the user's config::frame_limiter.provider selection to the
    // concrete Linux mechanism(s) to use. Unlike Windows (where RTSS and NVCP
    // are mutually exclusive alternatives), MangoHud's FPS cap and the
    // NVIDIA vsync-off env var are independent and can both apply: MangoHud
    // needs to actually be loaded by the launched process to do anything,
    // while __GL_SYNC_TO_VBLANK is picked up by any NVIDIA-driven OpenGL/
    // Vulkan process regardless. "rtss" (Windows overlay injector) has no
    // Linux equivalent so it's treated the same as "auto" here.
    resolved_providers_t resolve_providers() {
      const auto normalized = normalize(config::frame_limiter.provider);

      if (normalized == "none" || normalized == "disabled") {
        return {};
      }

      if (normalized == "nvidiacontrolpanel" || normalized == "nvidia" || normalized == "nvcp") {
        // Explicit request to skip MangoHud.
        return {.use_mangohud = false, .use_nvidia_env = nvidia_gpu_present()};
      }

      // "auto", "rtss", empty, or anything unrecognized: use both available paths.
      return {
        .use_mangohud = mangohud_available(),
        .use_nvidia_env = nvidia_gpu_present(),
      };
    }

    // Target FPS precedence: explicit user override > client display refresh
    // rate > stream FPS. Returns 0 if no valid target is available.
    int resolve_target_fps(const framegen::stream_start_policy_t &policy) {
      if (config::frame_limiter.fps_limit_millihz > 0) {
        BOOST_LOG(debug) << "Linux frame limiter: using configured FPS override.";
        return static_cast<int>(std::lround(config::frame_limiter.fps_limit_millihz / 1000.0));
      }
      if (policy.frame_limit_millihz > 0) {
        BOOST_LOG(debug) << "Linux frame limiter: using client display refresh rate.";
        return static_cast<int>(std::lround(policy.frame_limit_millihz / 1000.0));
      }
      if (policy.fps > 0) {
        BOOST_LOG(debug) << "Linux frame limiter: using stream FPS.";
        return policy.fps;
      }
      return 0;
    }

    void apply_mangohud(bp::environment &env, int target_fps) {
      const std::string existing = env["MANGOHUD_CONFIG"].to_string();
      const std::string fps_directive = "fps_limit=" + std::to_string(target_fps);
      env["MANGOHUD_CONFIG"] = existing.empty() ? fps_directive : existing + "," + fps_directive;
      BOOST_LOG(info) << "Linux frame limiter: set MangoHud fps_limit=" << target_fps
                       << " (requires the launched app to actually run under MangoHud).";
    }

    void apply_nvidia_env(bp::environment &env) {
      if (config::frame_limiter.disable_vsync) {
        env["__GL_SYNC_TO_VBLANK"] = "0";
        BOOST_LOG(info) << "Linux frame limiter: disabled VSYNC via __GL_SYNC_TO_VBLANK for the launched app.";
      }
    }

  }  // namespace

  void apply_launch_env(bp::environment &env, const framegen::stream_start_policy_t &policy) {
    if (!config::frame_limiter.enable) {
      return;
    }

    const auto providers = resolve_providers();
    if (!providers.use_mangohud && !providers.use_nvidia_env) {
      BOOST_LOG(debug) << "Linux frame limiter: enabled but no usable provider found (MangoHud not on PATH, no NVIDIA driver detected).";
      return;
    }

    if (providers.use_mangohud) {
      const int target_fps = resolve_target_fps(policy);
      if (target_fps > 0) {
        apply_mangohud(env, target_fps);
      }
    }

    if (providers.use_nvidia_env) {
      apply_nvidia_env(env);
    }
  }

}  // namespace platf::frame_limiter_linux
