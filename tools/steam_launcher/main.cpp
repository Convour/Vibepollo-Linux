/**
 * @file tools/steam_launcher/main.cpp
 * @brief Companion launcher for Steam-synced apps (see src/platform/linux/steam_library.cpp).
 *
 * Steam always forks a launched game as a child of the already-running Steam client, never of
 * whatever process asked it to launch (`steam steam://rungameid/<appid>` itself exits within
 * milliseconds once it has forwarded the request). If Sunshine invoked that command directly, it
 * would have nothing left to track: the stream would never end when the game is quit in-game,
 * and ending the stream would have no process to kill. This binary is a proxy Sunshine tracks
 * instead - the same pattern the Windows Playnite integration uses for the same reason.
 *
 * It (1) asks Steam to launch the game, (2) identifies the real game process(es) by scanning
 * `/proc/<pid>/environ` for `SteamAppId=<appid>`/`SteamGameId=<appid>` - the one signal present on
 * every process in the launch chain, native or Proton, all the way down to the actual game
 * engine binary - and (3) either forwards a termination signal to those processes when Sunshine
 * ends the stream, or exits on its own once they disappear, which ends the stream automatically
 * when the player quits from inside the game.
 *
 * The Steam client's own process group is captured once at startup, before the launch request is
 * sent, and is always excluded from matching/signaling: Steam's own launch-supervisor helpers
 * (`reaper`, `srt-bwrap`, Proton's wrapper scripts) inherit the SteamAppId environment variable
 * too, but live in the client's group, not the game's - killing them would take Steam down.
 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace {

  void log_line(const std::string &msg) {
    std::fprintf(stderr, "[steam_launcher] %s\n", msg.c_str());
    std::fflush(stderr);
  }

  bool is_all_digits(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
      return std::isdigit(c);
    });
  }

  std::optional<std::string> read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  std::string comm_of(pid_t pid) {
    auto contents = read_file("/proc/" + std::to_string(pid) + "/comm");
    if (!contents) {
      return {};
    }
    while (!contents->empty() && (contents->back() == '\n' || contents->back() == '\0')) {
      contents->pop_back();
    }
    return *contents;
  }

  std::vector<pid_t> all_pids() {
    std::vector<pid_t> pids;
    DIR *dir = opendir("/proc");
    if (!dir) {
      return pids;
    }
    while (struct dirent *entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (is_all_digits(name)) {
        pids.push_back(static_cast<pid_t>(std::stol(name)));
      }
    }
    closedir(dir);
    return pids;
  }

  // Exact match against a NUL-separated `KEY=value` entry, not a substring match, so appid "123"
  // can't false-positive against a real appid of "1234".
  bool environ_has_appid(const std::string &environ_blob, const std::string &appid) {
    const std::string needle_app = "SteamAppId=" + appid;
    const std::string needle_game = "SteamGameId=" + appid;
    std::size_t pos = 0;
    while (pos < environ_blob.size()) {
      std::size_t end = environ_blob.find('\0', pos);
      if (end == std::string::npos) {
        end = environ_blob.size();
      }
      const std::string entry = environ_blob.substr(pos, end - pos);
      if (entry == needle_app || entry == needle_game) {
        return true;
      }
      pos = end + 1;
    }
    return false;
  }

  std::optional<pid_t> pgid_of(pid_t pid) {
    const pid_t pgid = getpgid(pid);
    if (pgid < 0) {
      return std::nullopt;
    }
    return pgid;
  }

  // Distinct process groups among every currently-running PID whose environment carries the
  // target appid, excluding the Steam client's own group.
  std::unordered_set<pid_t> matching_pgids(const std::string &appid, pid_t client_pgid) {
    std::unordered_set<pid_t> pgids;
    for (pid_t pid : all_pids()) {
      auto environ_blob = read_file("/proc/" + std::to_string(pid) + "/environ");
      if (!environ_blob || !environ_has_appid(*environ_blob, appid)) {
        continue;
      }
      auto pgid = pgid_of(pid);
      if (!pgid || *pgid == client_pgid) {
        continue;
      }
      pgids.insert(*pgid);
    }
    return pgids;
  }

  // Native Arch/CachyOS Steam installs run the client binary as `comm == "steam"` (distinct from
  // `steamwebhelper` and any per-game processes). Captured before the launch request is sent.
  std::optional<pid_t> find_steam_client_pgid() {
    for (pid_t pid : all_pids()) {
      if (comm_of(pid) == "steam") {
        if (auto pgid = pgid_of(pid)) {
          return pgid;
        }
      }
    }
    return std::nullopt;
  }

  // Fires `steam steam://rungameid/<appid>`, detached into its own session so that neither a
  // quickly-exiting forwarder (the common case, when Steam is already running) nor a cold-started
  // Steam client itself ever ends up sharing this launcher's process group - which Sunshine
  // tracks and will eventually terminate.
  bool request_steam_launch(const std::string &appid) {
    const pid_t pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      setsid();
      const std::string uri = "steam://rungameid/" + appid;
      execlp("steam", "steam", uri.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
  }

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || !is_all_digits(argv[1])) {
    log_line("usage: sunshine-steam-launcher <appid>");
    return 1;
  }
  const std::string appid = argv[1];

  // Block SIGTERM/SIGINT and pick them up via signalfd in the poll loop below instead of an
  // async-signal handler, since reacting to them means reading /proc and doing string work.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  const int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
  if (sigfd < 0) {
    log_line("signalfd() failed; this session cannot forward termination to the game");
  }

  const auto client_pgid = find_steam_client_pgid();
  if (!client_pgid) {
    log_line("no running Steam client process found; is Steam running?");
    return 1;
  }
  log_line("Steam client pgid=" + std::to_string(*client_pgid) + "; requesting launch of appid " + appid);

  if (!request_steam_launch(appid)) {
    log_line("failed to invoke steam://rungameid/" + appid);
    return 1;
  }

  bool game_seen = false;
  int consecutive_empty = 0;
  std::unordered_set<pid_t> last_pgids;
  const auto start = std::chrono::steady_clock::now();
  // First-run shader precompilation / Proton DLL registration can take 30-60+ seconds before the
  // real game process appears; only fail if it never shows up at all.
  constexpr auto startup_grace = 120s;
  constexpr int kConsecutiveEmptyToExit = 3;

  pollfd pfd {sigfd, POLLIN, 0};

  while (true) {
    const int rc = poll(&pfd, 1, 1000);
    if (rc > 0 && (pfd.revents & POLLIN)) {
      signalfd_siginfo si {};
      if (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
        log_line("received termination signal; forwarding to " + std::to_string(last_pgids.size()) + " game process group(s)");
        for (pid_t pgid : last_pgids) {
          kill(-pgid, SIGTERM);
        }
        // Best-effort only: Sunshine's own process-group teardown may SIGKILL this launcher
        // shortly after delivering this signal, so no further escalation is attempted here.
        // Steam's reaper/Proton scaffolding self-terminates once the game's own process group(s)
        // exit (validated live against both a native and a Proton title).
        std::this_thread::sleep_for(500ms);
        return 0;
      }
    }

    const auto pgids = matching_pgids(appid, *client_pgid);
    if (!pgids.empty()) {
      game_seen = true;
      last_pgids = pgids;
      consecutive_empty = 0;
    } else if (game_seen) {
      if (++consecutive_empty >= kConsecutiveEmptyToExit) {
        log_line("game process(es) gone; ending session");
        return 0;
      }
    } else if (std::chrono::steady_clock::now() - start > startup_grace) {
      log_line("game never appeared within the startup grace period; giving up");
      return 1;
    }
  }
}
