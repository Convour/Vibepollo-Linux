/**
 * @file src/platform/linux/steam_library.h
 * @brief Discovers installed Steam games by parsing Steam's local VDF/ACF library files, and
 * reconciles them into Sunshine's apps.json - a Linux-native equivalent of the Windows Playnite
 * integration (src/platform/windows/playnite_sync.h), but without any companion process: Steam
 * requires no IPC, just local file parsing plus a `steam://rungameid/<appid>` launch command.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace config {
  struct steam_sync_t;
}

namespace platf::steam_library {

  struct game_t {
    std::string appid;
    std::string name;
    std::string install_dir;
    /// Absolute path to a library-cover JPEG if one was found in Steam's local cache, empty otherwise.
    /// Not currently wired into apps.json (Sunshine requires PNG box art; see reconcile_games_into_apps()).
    std::string box_art_path;
  };

  /**
   * @brief Parses Valve's KeyValues ("VDF") text format into a JSON tree.
   *
   * Each block becomes a JSON object; leaf `"key" "value"` pairs become JSON strings. Exposed
   * for testing/reuse; discover_steam_games() is the primary entry point.
   */
  std::optional<nlohmann::json> parse_vdf(const std::string &text);

  /**
   * @brief Finds all fully-installed Steam games across every configured Steam library folder.
   *
   * Reads `<steam_root>/steamapps/libraryfolders.vdf` for library paths, then each library's
   * `steamapps/appmanifest_*.acf` for game metadata, filtering to manifests with the
   * "fully installed" StateFlags bit (4) set - manifests also persist for uninstalled/
   * partially-downloaded games, which must not show up as launchable.
   *
   * Returns an empty vector (not an error) if no Steam installation is found.
   */
  std::vector<game_t> discover_steam_games();

  /**
   * @brief Reconciles discovered Steam games into an apps.json "apps" array in place.
   *
   * Mirrors the marker-field convention used by Playnite sync (playnite-id/playnite-managed/
   * playnite-added-at) with a `steam-` prefix instead, so existing manually-created app entries
   * are never touched: only entries with `"steam-managed": "auto"` are updated or removed.
   *
   * @param apps_array The apps.json "apps" JSON array, modified in place.
   * @param games Currently-installed games from discover_steam_games().
   * @param cfg Sync settings (TTL, remove-uninstalled).
   * @return Number of entries added, updated, and removed.
   */
  struct reconcile_result_t {
    int added {0};
    int updated {0};
    int removed {0};
  };
  reconcile_result_t reconcile_games_into_apps(nlohmann::json &apps_array, const std::vector<game_t> &games, const config::steam_sync_t &cfg);

}  // namespace platf::steam_library
