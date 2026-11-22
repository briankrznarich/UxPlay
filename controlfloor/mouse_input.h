// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <json.hpp>
#include <memory>
#include <string>

namespace controlfloor {
// All helper I/O is serialized on its own worker. The provider owns CF routing
// and video; this boundary can later be backed by different input hardware.
class MouseInput {
  public:
    using Json = nlohmann::json;
    struct Result {
        Json request, value;
        std::string error;
    };
    MouseInput();
    ~MouseInput();
    void start(const std::string &url);
    void stop();
    void reset(); // Cancel queued input on phone/provider-session changes.
    Json state();
    bool take(Result &result);
    void submit(const Json &request, const Json &args);
    static bool handles(const std::string &action);
    static bool handlesText(const std::string &action);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace controlfloor
