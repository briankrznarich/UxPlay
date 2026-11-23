// SPDX-License-Identifier: GPL-3.0-or-later
#include "navigation.h"
#include <cmath>
#include <stdexcept>
#include <iostream>
using namespace controlfloor;
using J = nlohmann::json;
static void require(bool ok) { if (!ok) throw std::runtime_error("Navigation assertion failed"); }
int main() {
    auto plan = [](const char *gesture, const char *model, double w = 375, double h = 667) {
        return navigationPlan("performGesture", {{"gesture", gesture}, {"modelName", model}}, w, h);
    };
    for (const auto &model : {"iPhone SE", "iPhone SE (3rd generation)", "iPhone 8 Plus", "iPhone 6s"}) {
        auto p = plan("ShowControlCenter", model);
        require(p.points.front().y == 666 && p.points.front().x == 187.5);
        require(p.points.back().y < p.points.front().y);
        require(plan("ShowHomeScreen", model).points.empty());
        require(plan("ShowAppSwitcher", model).points.empty());
    }
    for (const auto &model : {"iPhone X", "iPhone 16 Pro", "iPad Pro"}) {
        auto p = plan("ShowControlCenter", model, 667, 375);
        require(p.points.front().x == 666 && p.points.front().y == 1);
        require(p.points.back().y > p.points.front().y);
        require(plan("ShowAppSwitcher", model).points.back().seconds >= .7);
    }
    require(plan("ShowControlCenter", "Synthetic video").points.empty());
    require(plan("ShowHomeScreen", "iPhone SE").key == "GoHome");
    for (const auto &gesture : {"CenterSwipeUp", "CenterSwipeDown", "CenterSwipeLeft", "CenterSwipeRight",
                               "EdgeSwipeUp", "EdgeSwipeDown", "EdgeSwipeLeft", "EdgeSwipeRight",
                               "PageUp", "PageDown", "JiggleScreenContents"}) {
        for (auto size : {std::pair<int,int>{375,667}, {667,375}}) {
            auto p = plan(gesture, "iPhone SE", size.first, size.second);
            require(p.points.size() >= 2);
            double previous = -1;
            for (auto point : p.points) {
                require(point.x >= 0 && point.x < size.first && point.y >= 0 && point.y < size.second);
                require(point.seconds > previous); previous = point.seconds;
            }
        }
    }
    auto page = navigationPlan("performGesture", {{"gesture", "PageDown"}, {"x", 123}, {"y", 321}}, 375, 667);
    require(page.points.front().x == 123 && page.points.front().y == 321 && page.points.back().x == 123);
    for (J args : {J{{"gesture", "TwoFingerTouch"}}, J{{"gesture", "SwitchToNextOpenApp"}},
                   J{{"gesture", "ShowHomeScreen"}, {"duration", 1}}, J{{"gesture", 4}},
                   J{{"gesture", "PageUp"}, {"x", 4}}, J{{"gesture", "PageUp"}, {"x", 999}, {"y", 4}}}) {
        bool rejected = false;
        try { navigationPlan("performGesture", args, 375, 667); }
        catch (const std::exception &) { rejected = true; }
        require(rejected);
    }
    std::cout << "PASS SE/older-home-button vs modern/unknown models, landscape, bounded geometry, page origin, unsupported gestures\n";
}
