/**
 * @file src/platform/linux/steam_library.cpp
 * @brief See steam_library.h.
 */
#include "steam_library.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::steam_library {

  namespace fs = std::filesystem;

  // ---- Minimal VDF ("KeyValues") tokenizer/parser ----
  //
  // Valve's KeyValues text format: every key and every leaf value is a double-quoted string;
  // nested structure is delimited with '{' '}'. Numbers (appids, sizes, flags) are represented
  // as quoted strings too, so the whole tree maps naturally onto JSON objects/strings without
  // needing a bespoke value type.

  namespace {

    struct token_t {
      enum class kind_e { string, open_brace, close_brace } kind;
      std::string text;
    };

    std::vector<token_t> tokenize(const std::string &text) {
      std::vector<token_t> tokens;
      std::size_t i = 0;
      const std::size_t n = text.size();

      while (i < n) {
        const char ch = text[i];

        if (std::isspace(static_cast<unsigned char>(ch))) {
          ++i;
          continue;
        }

        if (ch == '/' && i + 1 < n && text[i + 1] == '/') {
          while (i < n && text[i] != '\n') {
            ++i;
          }
          continue;
        }

        if (ch == '{') {
          tokens.push_back({token_t::kind_e::open_brace, {}});
          ++i;
          continue;
        }

        if (ch == '}') {
          tokens.push_back({token_t::kind_e::close_brace, {}});
          ++i;
          continue;
        }

        if (ch == '"') {
          ++i;
          std::string value;
          while (i < n && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < n) {
              value.push_back(text[i + 1]);
              i += 2;
            } else {
              value.push_back(text[i]);
              ++i;
            }
          }
          ++i;  // closing quote
          tokens.push_back({token_t::kind_e::string, std::move(value)});
          continue;
        }

        // Unquoted stray token (not expected in well-formed Steam VDF files): skip one char to
        // avoid an infinite loop rather than failing the whole parse.
        ++i;
      }

      return tokens;
    }

    bool parse_object(const std::vector<token_t> &tokens, std::size_t &pos, nlohmann::json &out) {
      out = nlohmann::json::object();

      while (pos < tokens.size() && tokens[pos].kind != token_t::kind_e::close_brace) {
        if (tokens[pos].kind != token_t::kind_e::string) {
          return false;
        }
        const std::string key = tokens[pos].text;
        ++pos;

        if (pos >= tokens.size()) {
          return false;
        }

        if (tokens[pos].kind == token_t::kind_e::open_brace) {
          ++pos;
          nlohmann::json child;
          if (!parse_object(tokens, pos, child)) {
            return false;
          }
          if (pos >= tokens.size() || tokens[pos].kind != token_t::kind_e::close_brace) {
            return false;
          }
          ++pos;
          out[key] = std::move(child);
        } else if (tokens[pos].kind == token_t::kind_e::string) {
          out[key] = tokens[pos].text;
          ++pos;
        } else {
          return false;
        }
      }

      return true;
    }

  }  // namespace

  std::optional<nlohmann::json> parse_vdf(const std::string &text) {
    const auto tokens = tokenize(text);
    std::size_t pos = 0;
    nlohmann::json root;
    if (!parse_object(tokens, pos, root)) {
      return std::nullopt;
    }
    return root;
  }

  namespace {

    std::optional<fs::path> steam_root() {
      const char *home = std::getenv("HOME");
      if (!home) {
        return std::nullopt;
      }

      // Native package layout, then the older ~/.steam/steam symlink/layout.
      for (const auto candidate : {".local/share/Steam"sv, ".steam/steam"sv}) {
        auto path = fs::path(home) / candidate;
        std::error_code ec;
        if (fs::exists(path / "steamapps", ec)) {
          return path;
        }
      }
      return std::nullopt;
    }

    std::optional<std::string> read_file(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return std::nullopt;
      }
      std::ostringstream buffer;
      buffer << file.rdbuf();
      return buffer.str();
    }

    // Every library folder Steam knows about, starting with the root install itself.
    std::vector<fs::path> discover_library_paths(const fs::path &root) {
      std::vector<fs::path> libraries {root};

      const auto vdf_path = root / "steamapps" / "libraryfolders.vdf";
      const auto contents = read_file(vdf_path);
      if (!contents) {
        return libraries;
      }

      const auto parsed = parse_vdf(*contents);
      if (!parsed || !parsed->contains("libraryfolders") || !(*parsed)["libraryfolders"].is_object()) {
        return libraries;
      }

      for (const auto &[key, entry] : (*parsed)["libraryfolders"].items()) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()) {
          continue;
        }
        fs::path lib_path = entry["path"].get<std::string>();
        if (lib_path != root) {
          libraries.push_back(lib_path);
        }
      }

      return libraries;
    }

    std::optional<int> get_int(const nlohmann::json &obj, const char *key) {
      if (!obj.contains(key) || !obj[key].is_string()) {
        return std::nullopt;
      }
      try {
        return std::stoi(obj[key].get<std::string>());
      } catch (...) {
        return std::nullopt;
      }
    }

    std::string find_box_art(const fs::path &library_path, const std::string &appid) {
      // Current Steam layout: appcache/librarycache/<appid>/library_600x900.jpg
      const auto nested = library_path / "appcache" / "librarycache" / appid / "library_600x900.jpg";
      std::error_code ec;
      if (fs::exists(nested, ec)) {
        return nested.string();
      }
      // Older layout: appcache/librarycache/<appid>_library_600x900.jpg
      const auto flat = library_path / "appcache" / "librarycache" / (appid + "_library_600x900.jpg");
      if (fs::exists(flat, ec)) {
        return flat.string();
      }
      return {};
    }

    constexpr int kStateFlagFullyInstalled = 4;

    // Steam represents its own compatibility-layer packages (Proton builds, the "Steam Linux
    // Runtime" sandboxes other games depend on, shared redistributables) as regular
    // appmanifest_*.acf entries with StateFlags=4 like any real game, but they aren't
    // independently launchable. There's no "is this a real game" bit in the manifest itself
    // (that classification lives in Steam's binary appinfo.vdf cache, not parsed here); filter
    // on the well-known naming patterns these packages use instead.
    bool looks_like_runtime_package(const std::string &name, const std::string &install_dir) {
      static constexpr std::array<std::string_view, 4> name_prefixes {
        "Steam Linux Runtime"sv, "Proton"sv, "Steamworks Common Redistributables"sv, "Proton EasyAntiCheat Runtime"sv
      };
      for (const auto prefix : name_prefixes) {
        if (name.rfind(prefix, 0) == 0) {
          return true;
        }
      }
      return install_dir.rfind("SteamLinuxRuntime"sv, 0) == 0;
    }

  }  // namespace

  std::vector<game_t> discover_steam_games() {
    std::vector<game_t> games;
    std::unordered_set<std::string> seen_appids;

    const auto root = steam_root();
    if (!root) {
      BOOST_LOG(debug) << "Steam library sync: no Steam installation found under $HOME.";
      return games;
    }

    for (const auto &library_path : discover_library_paths(*root)) {
      const auto steamapps_dir = library_path / "steamapps";
      std::error_code ec;
      if (!fs::exists(steamapps_dir, ec)) {
        continue;
      }

      for (auto it = fs::directory_iterator(steamapps_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto &path = it->path();
        if (path.extension() != ".acf" || path.filename().string().rfind("appmanifest_", 0) != 0) {
          continue;
        }

        const auto contents = read_file(path);
        if (!contents) {
          continue;
        }
        const auto parsed = parse_vdf(*contents);
        if (!parsed || !parsed->contains("AppState") || !(*parsed)["AppState"].is_object()) {
          continue;
        }
        const auto &state = (*parsed)["AppState"];

        const auto state_flags = get_int(state, "StateFlags");
        if (!state_flags || (*state_flags & kStateFlagFullyInstalled) == 0) {
          // Manifest exists for an uninstalled, partially-downloaded, or update-in-progress
          // game; only fully-installed games are launchable.
          continue;
        }

        if (!state.contains("appid") || !state["appid"].is_string() || !state.contains("name") || !state["name"].is_string()) {
          continue;
        }

        std::string appid = state["appid"].get<std::string>();
        std::string name = state["name"].get<std::string>();
        std::string install_dir = state.value("installdir", "");

        if (looks_like_runtime_package(name, install_dir)) {
          continue;
        }

        // The same appid can have a manifest in more than one library folder (observed with
        // shared runtime/redistributable packages on this machine); keep only the first.
        if (!seen_appids.insert(appid).second) {
          continue;
        }

        game_t game;
        game.appid = std::move(appid);
        game.name = std::move(name);
        game.install_dir = std::move(install_dir);
        game.box_art_path = find_box_art(library_path, game.appid);
        games.push_back(std::move(game));
      }
    }

    BOOST_LOG(info) << "Steam library sync: discovered " << games.size() << " fully-installed game(s).";
    return games;
  }

  reconcile_result_t reconcile_games_into_apps(nlohmann::json &apps_array, const std::vector<game_t> &games, const config::steam_sync_t &cfg) {
    reconcile_result_t result;

    if (!apps_array.is_array()) {
      apps_array = nlohmann::json::array();
    }

    const auto now = std::chrono::system_clock::now();
    const auto now_iso = [&] {
      const auto t = std::chrono::system_clock::to_time_t(now);
      std::tm tm {};
      gmtime_r(&t, &tm);
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
      return std::string(buf);
    }();

    std::unordered_map<std::string, const game_t *> games_by_appid;
    for (const auto &game : games) {
      games_by_appid[game.appid] = &game;
    }

    nlohmann::json new_apps = nlohmann::json::array();
    std::unordered_map<std::string, bool> matched_appids;

    for (auto &app : apps_array) {
      const bool is_steam_managed = app.contains("steam-managed") && app["steam-managed"].is_string() && app["steam-managed"].get<std::string>() == "auto";
      if (!is_steam_managed) {
        new_apps.push_back(app);
        continue;
      }

      const std::string appid = app.value("steam-id", "");
      const auto it = games_by_appid.find(appid);
      if (it == games_by_appid.end()) {
        // No longer installed.
        if (cfg.autosync_remove_uninstalled) {
          ++result.removed;
          continue;
        }
        new_apps.push_back(app);
        continue;
      }

      matched_appids[appid] = true;

      if (cfg.autosync_delete_after_days > 0) {
        const std::string added_at = app.value("steam-added-at", "");
        // TTL is measured from steam-added-at; still-installed games simply keep their
        // original timestamp rather than being deleted while actively installed.
        if (!added_at.empty()) {
          std::tm tm {};
          if (strptime(added_at.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
            const auto added_time = std::chrono::system_clock::from_time_t(timegm(&tm));
            const auto age_days = std::chrono::duration_cast<std::chrono::hours>(now - added_time).count() / 24;
            if (age_days >= cfg.autosync_delete_after_days) {
              ++result.removed;
              continue;
            }
          }
        }
      }

      // Still installed: refresh name/box-art in case they changed, keep everything else
      // (including any manual edits the user made to the entry) untouched.
      const auto &game = *it->second;
      if (app.value("name", "") != game.name) {
        app["name"] = game.name;
        ++result.updated;
      }
      new_apps.push_back(app);
    }

    for (const auto &game : games) {
      if (matched_appids.count(game.appid)) {
        continue;
      }

      nlohmann::json entry;
      entry["name"] = game.name;
      entry["cmd"] = "steam steam://rungameid/" + game.appid;
      entry["auto-detach"] = true;
      entry["steam-id"] = game.appid;
      entry["steam-managed"] = "auto";
      entry["steam-added-at"] = now_iso;
      // Box art is JPEG in Steam's cache; apps.json requires PNG (see
      // proc::validate_app_image_path), so it's intentionally not wired here in v1 - entries
      // fall back to Sunshine's default box image until JPEG->PNG conversion is added.

      new_apps.push_back(std::move(entry));
      ++result.added;
    }

    apps_array = std::move(new_apps);
    return result;
  }

}  // namespace platf::steam_library
