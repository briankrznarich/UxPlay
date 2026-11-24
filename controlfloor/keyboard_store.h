// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <json.hpp>
#include <string>
#include <vector>
struct sqlite3;
namespace controlfloor {
// Persist observations/preferences, never in-flight jobs or live session authority.
class KeyboardStore {
public:
    KeyboardStore() = default;
    ~KeyboardStore();
    void open(const std::string &path);
    std::vector<nlohmann::json> load(const std::string &source);
    void save(const std::string &source, const nlohmann::json &record);
private:
    sqlite3 *db = nullptr;
};
}
