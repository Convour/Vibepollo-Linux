/**
 * @file src/platform/linux/display_device.cpp
 * @brief See display_device.h.
 */
#include "display_device.h"

#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/rtsp.h"

using namespace std::literals;

namespace platf::display_device_linux {

  namespace {

    struct kscreen_mode_t {
      std::string id;
      unsigned width {};
      unsigned height {};
      double refresh_rate {};
    };

    struct kscreen_output_t {
      std::string name;
      bool connected {};
      bool enabled {};
      int priority {};
      std::string current_mode_id;
      std::vector<kscreen_mode_t> modes;
    };

    struct output_snapshot_t {
      std::string name;
      bool enabled {};
      int priority {};
      std::string mode_id;
    };

    // The pre-apply arrangement, populated by apply_session_display() and
    // consumed (and cleared) by revert_session_display(). Guarded on the
    // assumption that only one streaming session drives display state at a
    // time, matching proc::proc's own single-active-app model.
    std::mutex g_snapshot_mutex;
    std::optional<std::vector<output_snapshot_t>> g_pre_apply_snapshot;

    // Runs an external command with an explicit argv (no shell involved, so
    // no quoting/injection concerns even though output names ultimately come
    // from local config). Returns {exit_code, captured_stdout}; exit_code is
    // -1 if the command could not be spawned at all.
    std::pair<int, std::string> run_capture(const std::vector<std::string> &args) {
      int out_pipe[2];
      if (pipe(out_pipe) != 0) {
        return {-1, {}};
      }

      const pid_t pid = fork();
      if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return {-1, {}};
      }

      if (pid == 0) {
        // Child
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
          dup2(devnull, STDERR_FILENO);
          close(devnull);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) {
          argv.push_back(const_cast<char *>(a.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
      }

      // Parent
      close(out_pipe[1]);
      std::string output;
      char buf[4096];
      ssize_t n;
      while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<std::size_t>(n));
      }
      close(out_pipe[0]);

      int status = 0;
      waitpid(pid, &status, 0);
      const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      return {exit_code, output};
    }

    bool kscreen_query(std::vector<kscreen_output_t> &outputs) {
      auto [exit_code, output] = run_capture({"kscreen-doctor", "-j"});
      if (exit_code != 0 || output.empty()) {
        BOOST_LOG(warning) << "kscreen-doctor query failed (exit code " << exit_code << ").";
        return false;
      }

      try {
        const auto parsed = nlohmann::json::parse(output);
        if (!parsed.contains("outputs") || !parsed["outputs"].is_array()) {
          return false;
        }

        for (const auto &o : parsed["outputs"]) {
          kscreen_output_t entry;
          entry.name = o.value("name", "");
          entry.connected = o.value("connected", false);
          entry.enabled = o.value("enabled", false);
          entry.priority = o.value("priority", 0);
          entry.current_mode_id = o.value("currentModeId", "");

          if (o.contains("modes") && o["modes"].is_array()) {
            for (const auto &m : o["modes"]) {
              kscreen_mode_t mode;
              mode.id = m.value("id", "");
              mode.refresh_rate = m.value("refreshRate", 0.0);
              if (m.contains("size")) {
                mode.width = m["size"].value("width", 0u);
                mode.height = m["size"].value("height", 0u);
              }
              entry.modes.push_back(std::move(mode));
            }
          }

          outputs.push_back(std::move(entry));
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed to parse kscreen-doctor JSON output: " << e.what();
        return false;
      }

      return true;
    }

    const kscreen_output_t *find_output(const std::vector<kscreen_output_t> &outputs, const std::string &name) {
      for (const auto &o : outputs) {
        if (o.name == name) {
          return &o;
        }
      }
      return nullptr;
    }

    double refresh_rate_to_hz(const display_device::FloatingPoint &fp) {
      if (std::holds_alternative<double>(fp)) {
        return std::get<double>(fp);
      }
      const auto &rational = std::get<display_device::Rational>(fp);
      return rational.m_denominator ? static_cast<double>(rational.m_numerator) / rational.m_denominator : 0.0;
    }

    // Finds the mode whose resolution matches exactly and whose refresh rate
    // is closest to the requested one (if a refresh rate was requested).
    //
    // If resolution is unset but a refresh rate was requested, candidates are
    // constrained to the output's *current* resolution rather than scanning
    // every mode - otherwise a refresh-only request (resolution_option =
    // disabled, refresh_rate_option = automatic, a reachable combination per
    // parse_resolution_option()/parse_refresh_rate_option()) could match the
    // closest-refresh mode at any resolution, including a much smaller one.
    std::optional<std::string> find_mode_id(
      const kscreen_output_t &output,
      const std::optional<display_device::Resolution> &resolution,
      const std::optional<display_device::FloatingPoint> &refresh_rate
    ) {
      if (!resolution && !refresh_rate) {
        return std::nullopt;
      }

      std::optional<display_device::Resolution> resolution_constraint = resolution;
      if (!resolution_constraint) {
        for (const auto &mode : output.modes) {
          if (mode.id == output.current_mode_id) {
            resolution_constraint = display_device::Resolution {mode.width, mode.height};
            break;
          }
        }
      }

      const kscreen_mode_t *best = nullptr;
      double best_refresh_delta = std::numeric_limits<double>::max();
      const double requested_hz = refresh_rate ? refresh_rate_to_hz(*refresh_rate) : 0.0;

      for (const auto &mode : output.modes) {
        if (resolution_constraint && (mode.width != resolution_constraint->m_width || mode.height != resolution_constraint->m_height)) {
          continue;
        }

        if (!refresh_rate) {
          // No refresh preference: take the first resolution match.
          return mode.id;
        }

        const double delta = std::abs(mode.refresh_rate - requested_hz);
        if (delta < best_refresh_delta) {
          best_refresh_delta = delta;
          best = &mode;
        }
      }

      return best ? std::optional<std::string> {best->id} : std::nullopt;
    }

    bool run_kscreen_mutation(const std::vector<std::string> &output_args) {
      if (output_args.empty()) {
        return true;
      }

      std::vector<std::string> args {"kscreen-doctor"};
      args.insert(args.end(), output_args.begin(), output_args.end());

      auto [exit_code, output] = run_capture(args);
      if (exit_code != 0) {
        BOOST_LOG(warning) << "kscreen-doctor apply failed (exit code " << exit_code << ").";
        return false;
      }
      return true;
    }

  }  // namespace

  bool kscreen_doctor_available() {
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
        const auto candidate = dir + "/kscreen-doctor";
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

  bool apply_session_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    if (!kscreen_doctor_available()) {
      return true;
    }

    const auto parsed = display_device::parse_configuration(video_config, session);
    if (std::holds_alternative<display_device::configuration_disabled_tag_t>(parsed)) {
      return true;
    }
    if (std::holds_alternative<display_device::failed_to_parse_tag_t>(parsed)) {
      BOOST_LOG(error) << "Linux display device: failed to parse session display configuration.";
      return false;
    }

    const auto &config = std::get<display_device::SingleDisplayConfiguration>(parsed);
    if (config.m_device_id.empty()) {
      BOOST_LOG(warning) << "Linux display device: no output_name configured; skipping virtual display automation.";
      return true;
    }

    std::vector<kscreen_output_t> outputs;
    if (!kscreen_query(outputs)) {
      return false;
    }

    const auto *target = find_output(outputs, config.m_device_id);
    if (!target) {
      BOOST_LOG(warning) << "Linux display device: output '" << config.m_device_id << "' not found by kscreen-doctor "
                          << "(is the EDID-injected virtual output present? see docs/linux/LEARNINGS.md S11).";
      return false;
    }
    if (!target->connected) {
      BOOST_LOG(warning) << "Linux display device: output '" << config.m_device_id << "' is not connected.";
      return false;
    }

    using DevicePreparation = display_device::SingleDisplayConfiguration::DevicePreparation;

    if (config.m_device_prep == DevicePreparation::VerifyOnly) {
      return true;
    }

    // Snapshot the pre-apply arrangement so revert can restore it precisely.
    {
      std::lock_guard lock {g_snapshot_mutex};
      if (!g_pre_apply_snapshot) {
        std::vector<output_snapshot_t> snapshot;
        for (const auto &o : outputs) {
          snapshot.push_back({o.name, o.enabled, o.priority, o.current_mode_id});
        }
        g_pre_apply_snapshot = std::move(snapshot);
      }
    }

    std::vector<std::string> args;
    const std::string prefix = "output." + target->name + ".";
    args.push_back(prefix + "enable");

    if (config.m_device_prep == DevicePreparation::EnsurePrimary || config.m_device_prep == DevicePreparation::EnsureOnlyDisplay) {
      args.push_back(prefix + "primary");
    }

    if (const auto mode_id = find_mode_id(*target, config.m_resolution, config.m_refresh_rate)) {
      args.push_back(prefix + "mode." + *mode_id);
    } else if (config.m_resolution || config.m_refresh_rate) {
      BOOST_LOG(warning) << "Linux display device: no matching mode found on '" << target->name
                          << "' for the requested resolution/refresh rate; enabling at its current mode instead.";
    }

    if (config.m_hdr_state) {
      args.push_back(prefix + "hdr." + (*config.m_hdr_state == display_device::HdrState::Enabled ? "enable" : "disable"));
    }

    if (config.m_device_prep == DevicePreparation::EnsureOnlyDisplay) {
      for (const auto &o : outputs) {
        if (o.name != target->name && o.connected && o.enabled) {
          args.push_back("output." + o.name + ".disable");
        }
      }
    }

    if (!run_kscreen_mutation(args)) {
      return false;
    }

    BOOST_LOG(info) << "Linux display device: applied session display configuration for output '" << target->name << "'.";
    return true;
  }

  bool revert_session_display() {
    std::vector<output_snapshot_t> snapshot;
    {
      std::lock_guard lock {g_snapshot_mutex};
      if (!g_pre_apply_snapshot) {
        return true;
      }
      snapshot = std::move(*g_pre_apply_snapshot);
      g_pre_apply_snapshot.reset();
    }

    if (!kscreen_doctor_available()) {
      return true;
    }

    std::vector<std::string> args;
    for (const auto &o : snapshot) {
      const std::string prefix = "output." + o.name + ".";
      args.push_back(prefix + (o.enabled ? "enable" : "disable"));
      if (o.enabled && !o.mode_id.empty()) {
        args.push_back(prefix + "mode." + o.mode_id);
      }
    }
    // Set priorities (primary output) in a second pass so "primary" is applied
    // to whichever output previously had priority 1, after all enable/disable
    // state above has been queued in the same atomic invocation.
    for (const auto &o : snapshot) {
      if (o.enabled && o.priority == 1) {
        args.push_back("output." + o.name + ".primary");
      }
    }

    const bool ok = run_kscreen_mutation(args);
    if (ok) {
      BOOST_LOG(info) << "Linux display device: reverted to pre-session display arrangement.";
    } else {
      BOOST_LOG(warning) << "Linux display device: failed to revert display arrangement; system may still be in "
                          << "the session's display state. Run 'kscreen-doctor -o' to check.";
    }
    return ok;
  }

}  // namespace platf::display_device_linux
