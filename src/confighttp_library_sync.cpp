/**
 * @file src/confighttp_library_sync.cpp
 * @brief Steam library sync HTTP endpoints (Linux-only).
 */

#ifdef __linux__

// standard includes
  #include <vector>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include "confighttp.h"
  #include "file_handler.h"
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/linux/steam_library.h"

namespace confighttp {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");

  void getSteamSyncStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json out;
    out["enabled"] = config::steam_sync.enable;
    out["auto_sync"] = config::steam_sync.auto_sync;

    const auto games = platf::steam_library::discover_steam_games();
    out["installed_games_found"] = games.size();
    out["message"] = games.empty() ?
                        "No fully-installed Steam games found (or no Steam installation detected under $HOME)." :
                        std::to_string(games.size()) + " installed Steam game(s) found.";

    send_response(response, out);
  }

  void postSteamSyncTrigger(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    if (!config::steam_sync.enable) {
      bad_request(response, request, "Steam library sync is disabled.");
      return;
    }

    nlohmann::json out;
    try {
      const auto games = platf::steam_library::discover_steam_games();

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps_node = file_tree["apps"];

      const auto result = platf::steam_library::reconcile_games_into_apps(apps_node, games, config::steam_sync);
      refresh_client_apps_cache(file_tree);

      out["status"] = true;
      out["added"] = result.added;
      out["updated"] = result.updated;
      out["removed"] = result.removed;
      send_response(response, out);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "postSteamSyncTrigger: " << e.what();
      bad_request(response, request, e.what());
    }
  }

}  // namespace confighttp

#endif  // __linux__
