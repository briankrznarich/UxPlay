// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <json.hpp>
#include <memory>
#include <string>

namespace controlfloor {
bool isShortcutRequest(const nlohmann::json &request);
struct PairingOptions {
    std::string helper = "http://127.0.0.1:7390", directory = "log", artifact, database;
    unsigned short port = 9876;
    double promptDelay = 2, retryDelay = 30, timeout = 120;
};
struct PairingOutput {
    std::string worker;
    nlohmann::json message;
    bool disconnect = false;
};
// Parent-only coordinator. HTTP, inventory and input workers never decode video.
// event()/take() are safe across threads; the supervisor drains outputs on GLib.
class PairingCoordinator {
public:
    explicit PairingCoordinator(const PairingOptions &options);
    ~PairingCoordinator();
    void start();
    void stop();
    unsigned short port() const;
    void event(const std::string &type, const std::string &worker,
               const std::string &peer, const std::string &payload = "");
    bool take(PairingOutput &output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace controlfloor

// Small application hooks; the generic multi-device layer knows nothing of CF.
bool cf_lan_enabled();
bool cf_lan_start();
void cf_lan_stop();
controlfloor::PairingCoordinator *cf_lan_coordinator();
void cf_lan_worker_transport(void (*send)(const char *));
void cf_lan_worker_message(const std::string &message);
