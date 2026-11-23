// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <json.hpp>
#include <string>
#include <vector>

namespace controlfloor {
// Pure CF-to-input translation. No networking, sleeps or hardware ownership here.
struct NavigationPoint {
    double x, y, seconds; // Absolute offset after mouseDown settles.
};
struct Navigation {
    std::string gesture, shortcut, key;
    std::vector<NavigationPoint> points;
};
bool handlesNavigation(const std::string &action);
Navigation navigationPlan(const std::string &action, const nlohmann::json &args,
                          double width, double height);
} // namespace controlfloor
