// SPDX-License-Identifier: GPL-3.0-or-later
#include "navigation.h"
#include <cmath>
#include <map>
#include <stdexcept>

namespace controlfloor {
namespace {
const std::map<std::string, std::string> actions = {
    {"simulateHomeButton", "ShowHomeScreen"}, {"showHomeScreen", "ShowHomeScreen"},
    {"showControlCenter", "ShowControlCenter"}, {"showAppSwitcher", "ShowAppSwitcher"},
    {"showNotificationCenter", "ShowNotificationCenter"}, {"simulateLockButton", "LockScreen"},
    {"simulateVolumeUpButton", "VolumeUp"}, {"simulateVolumeDownButton", "VolumeDown"},
    {"centerSwipeUp", "CenterSwipeUp"}, {"centerSwipeDown", "CenterSwipeDown"},
    {"centerSwipeLeft", "CenterSwipeLeft"}, {"centerSwipeRight", "CenterSwipeRight"},
    {"edgeSwipeUp", "EdgeSwipeUp"}, {"edgeSwipeDown", "EdgeSwipeDown"},
    {"edgeSwipeLeft", "EdgeSwipeLeft"}, {"edgeSwipeRight", "EdgeSwipeRight"}
};
}
bool handlesNavigation(const std::string &action) {
    return action == "performGesture" || actions.count(action);
}
Navigation navigationPlan(const std::string &action, const nlohmann::json &args,
                          double width, double height) {
    Navigation plan;
    if (action == "performGesture") {
        if (!args.contains("gesture") || !args["gesture"].is_string())
            throw std::runtime_error("gesture must be a string");
        plan.gesture = args["gesture"].get<std::string>();
    } else {
        auto found = actions.find(action);
        if (found == actions.end()) throw std::runtime_error("Unsupported navigation action: " + action);
        plan.gesture = found->second;
    }
    if (args.contains("duration") && (!args["duration"].is_number() ||
        !std::isfinite(args["duration"].get<double>()) || args["duration"] != 0))
        throw std::runtime_error("Navigation supports single presses and fixed gestures, not duration/held buttons");
    const auto &g = plan.gesture;
    // Mouse endpoints must stay inside the logical screen. The model is supplied
    // by the provider, never accepted from the viewer. Unknown models get no
    // model-dependent system swipe; keyboard shortcuts still work normally.
    std::string model = args.value("modelName", std::string());
    bool iphone = model.find("iPhone") != std::string::npos;
    bool homeButton = iphone && model.find("SE") != std::string::npos;
    if (iphone && model.compare(0, 7, "iPhone ") == 0 && model.size() > 7 &&
        model[7] >= '1' && model[7] <= '8' &&
        (model.size() == 8 || model[8] < '0' || model[8] > '9')) homeButton = true;
    bool modern = (iphone && !homeButton) || model.find("iPad") != std::string::npos;
    double left = 1, top = 1, right = width - 1, bottom = height - 1;
    double cx = width / 2, cy = height / 2;
    auto swipe = [&](double x1, double y1, double x2, double y2, double time) {
        plan.points = {{x1, y1, 0}, {x2, y2, time}};
    };
    if (g == "ShowHomeScreen") {
        plan.key = "GoHome"; plan.shortcut = "home";
        if (modern) swipe(cx, bottom, cx, cy, .15);
    } else if (g == "ShowControlCenter") {
        plan.shortcut = "controlCenter";
        if (homeButton) swipe(cx, bottom, cx, cy, .15);
        else if (modern) swipe(right, top, right, cy, .15);
    } else if (g == "ShowAppSwitcher") {
        plan.shortcut = "appSwitcher";
        if (modern) { swipe(cx, bottom, cx, cy, .25); plan.points.push_back({cx, cy, .8}); }
    } else if (g == "ShowNotificationCenter") {
        plan.shortcut = "notificationCenter";
        swipe(cx, top, cx, cy, .25);
    } else if (g == "LockScreen") plan.shortcut = "lockScreen";
    else if (g == "AskSiri") plan.shortcut = "siri";
    else if (g == "AccessibilityShortcut") plan.shortcut = "accessibilityShortcut";
    else if (g == "VolumeUp") plan.key = "AudioVolumeUp";
    else if (g == "VolumeDown") plan.key = "AudioVolumeDown";
    else if (g == "CenterSwipeUp") swipe(cx, cy, cx, top, .2);
    else if (g == "CenterSwipeDown") swipe(cx, cy, cx, bottom, .2);
    else if (g == "CenterSwipeLeft") swipe(cx, cy, width * .2, cy, .5);
    else if (g == "CenterSwipeRight") swipe(cx, cy, width * .8, cy, .5);
    else if (g == "EdgeSwipeUp") swipe(cx, bottom, cx, height * .25, .15);
    else if (g == "EdgeSwipeDown") swipe(cx, top, cx, height * .75, .15);
    else if (g == "EdgeSwipeLeft") swipe(right, cy, width * .25, cy, .15);
    else if (g == "EdgeSwipeRight") swipe(left, cy, width * .75, cy, .15);
    else if (g == "PageDown" || g == "PageUp") {
        if (args.contains("x") || args.contains("y")) {
            for (auto key : {"x", "y"})
                if (!args.contains(key) || !args[key].is_number() || !std::isfinite(args[key].get<double>()))
                    throw std::runtime_error("Page gestures require both finite x and y coordinates");
            cx = args["x"]; cy = args["y"];
            if (cx < 0 || cx >= width || cy < 0 || cy >= height)
                throw std::runtime_error("Page gesture coordinates are outside the logical screen");
            // CFA treats the default (0,0) as screen center.
            if (cx == 0 && cy == 0) { cx = width / 2; cy = height / 2; }
        }
        double end = g == "PageDown" ? top : bottom;
        swipe(cx, cy, cx, end, .2);
        plan.points.push_back({cx, end + (g == "PageDown" ? 5 : -5), .25});
        plan.points.push_back({cx, end, .3});
    } else if (g == "JiggleScreenContents") {
        swipe(cx, bottom, cx, height * .95, .3);
        plan.points.push_back({cx, bottom, .6});
        plan.points.push_back({cx, bottom, 1.1});
    } else {
        // Several CFA gesture names are placeholders; do not return success for
        // those, multitouch, or actions that would need a Shortcuts callback.
        throw std::runtime_error("Unsupported UxPlay gesture: " + g);
    }
    return plan;
}
} // namespace controlfloor
