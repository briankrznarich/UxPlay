// SPDX-License-Identifier: GPL-3.0-or-later
#include "mouse_input.h"
#include "navigation.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <ixwebsocket/IXHttpClient.h>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace controlfloor {
using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
// Keep in sync with iphone-use/Keymap.swift: printable US ASCII + Tab + LF.
// Scan the entire string before enqueueing or sending any part to the helper.
std::string checkedText(const J &args) {
    if (!args.contains("text") || !args["text"].is_string())
        throw std::runtime_error("text must be a string");
    std::string text = args["text"].get<std::string>();
    if (text.size() > 1024)
        throw std::runtime_error("Text is limited to 1024 ASCII characters per call; nothing was typed");
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (!(c >= 0x20 && c <= 0x7e) && c != '\t' && c != '\n')
            throw std::runtime_error(
                "Unsupported character at UTF-8 byte " + std::to_string(i + 1) +
                "; iphone-use supports printable US ASCII, tab and LF newline only. Nothing was typed");
    }
    return text;
}
} // namespace
struct MouseInput::Impl {
    struct Job {
        J request, args;
        unsigned long session;
        Clock::time_point at;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool running = false;
    unsigned long session = 0;
    std::string url;
    std::deque<Job> jobs;
    std::deque<Result> results;
    J published = {{"available", false},
                   {"keyboardAvailable", false},
                   {"enabled", false},
                   {"logicalWidthPortrait", 375},
                   {"logicalHeightPortrait", 667},
                   {"mouse", J::object()},
                   {"error", "Mouse adapter is not enabled"}};

    J call(const std::string &path, const J &body = nullptr, int timeout = 2) {
        ix::HttpClient client;
        auto args = client.createRequest();
        args->connectTimeout = 1;
        args->transferTimeout = timeout;
        args->followRedirects = false;
        args->compress = false;
        args->extraHeaders["Content-Type"] = "application/json";
        auto response =
            body.is_null() ? client.get(url + path, args) : client.post(url + path, body.dump(), args);
        if (response->statusCode != 200 || response->body.size() > 1024 * 1024) {
            J error = J::parse(response->body, nullptr, false);
            throw std::runtime_error(error.is_object() && error.contains("error") &&
                                             error["error"].is_string()
                                         ? error["error"].get<std::string>()
                                         : "iphone-use HTTP " + std::to_string(response->statusCode));
        }
        J value = J::parse(response->body);
        if (!value.is_object() || value.contains("error"))
            throw std::runtime_error("Invalid/error response from iphone-use");
        return value;
    }
    void run() {
        std::string host, error;
        bool available = false, keyboardAvailable = false, consumerAvailable = false, enabled = false, held = false;
        int width = 375, height = 667;
        J mouse = J::object();
        unsigned long observed = 0;
        auto probeAt = Clock::time_point::min();
        auto release = [&] {
            if (held && !host.empty()) {
                try {
                    J value = call("/mouseUp", {{"device", host}});
                    mouse = value.value("mouse", mouse);
                    held = mouse.value("down", false);
                } catch (...) {
                } // Best effort after timeout/disconnect; retry on the next probe/reset.
            }
        };
        while (true) {
            Job job;
            bool reset = false;
            {
                std::unique_lock<std::mutex> l(mutex);
                wake.wait_for(l, std::chrono::milliseconds(20),
                              [&] { return !running || session != observed || !jobs.empty(); });
                if (!running)
                    break;
                reset = observed != session;
                observed = session;
                if (!jobs.empty()) {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
            }
            if (reset) {
                enabled = false;
                release();
                probeAt = Clock::time_point::min();
            }
            if (Clock::now() >= probeAt) {
                try {
                    J hosts = call("/devices").at("bluetoothHosts");
                    if (!hosts.is_array() || hosts.size() != 1)
                        throw std::runtime_error("Exactly one Bluetooth phone must be connected");
                    std::string found = hosts[0].at("bluetoothHost").get<std::string>();
                    if (found.empty())
                        throw std::runtime_error("Missing Bluetooth host identifier");
                    if (host.empty())
                        host = found;
                    if (host != found)
                        throw std::runtime_error(
                            "Connected Bluetooth phone changed; restart UxPlay to select it");
                    available = hosts[0].value("liveMouse", false);
                    // New helpers expose keyboard readiness explicitly. Older versions
                    // report subscriptions in diagnostics (keyboard report ID 1).
                    keyboardAvailable = hosts[0].value("keyboard", false);
                    consumerAvailable = hosts[0].value("consumerControls", false);
                    if (!hosts[0].contains("keyboard")) {
                        J diagnostics = hosts[0].value("bluetoothDiagnostics", J::object());
                        J reports = diagnostics.value("subscribedReportIDs", J::array());
                        keyboardAvailable = reports.is_array() &&
                                            std::find(reports.begin(), reports.end(), J(1)) != reports.end();
                    }
                    if (!available) {
                        enabled = false;
                        release();
                    }
                    mouse = hosts[0].value("mouse", J::object());
                    if (mouse.value("calibrating", false))
                        throw std::runtime_error("iphone-use mouse calibration is busy");
                    double w = mouse.value("logicalWidth", 375.0), h = mouse.value("logicalHeight", 667.0);
                    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 || w > 100000 ||
                        h > 100000)
                        throw std::runtime_error("Invalid iphone-use logical dimensions");
                    width = static_cast<int>(std::min(w, h));
                    height = static_cast<int>(std::max(w, h));
                    error = available ? "" : "Bluetooth mouse is not ready";
                    if (!enabled)
                        release();
                } catch (const std::exception &e) {
                    error = e.what();
                    available = keyboardAvailable = consumerAvailable = enabled = false;
                    release();
                }
                probeAt = Clock::now() + std::chrono::seconds(1);
            }
            if (!job.request.is_null()) {
                Result result{job.request, nullptr, ""};
                try {
                    {
                        std::lock_guard<std::mutex> l(mutex);
                        if (job.session != session)
                            throw std::runtime_error("Mouse session changed");
                    }
                    const std::string action = job.request.at("action");
                    if (Clock::now() - job.at > std::chrono::seconds(1))
                        throw std::runtime_error("Input command expired; input queue was too slow; nothing "
                                                 "from this call was sent");
                    if (MouseInput::handlesCatalog(action)) {
                        // Discovery is read-only and does not need HID subscriptions.
                        result.value = call("/" + action);
                        result.value["agent"] = "Bluetooth";
                    } else if (action == "disableLiveMouse") {
                        enabled = false;
                        release();
                        if (held)
                            throw std::runtime_error("Could not confirm mouse release");
                    } else if (MouseInput::handlesText(action)) {
                        if (!keyboardAvailable)
                            throw std::runtime_error("Bluetooth keyboard is not ready");
                        release();
                        if (held)
                            throw std::runtime_error("Could not release mouse before typing");
                        const std::string text = job.args.at("text");
                        if (!text.empty()) {
                            // The helper emits 2 or 4 reports per character, spaced 30 ms
                            // apart. A mouse's two-second timeout would truncate the wait
                            // for ordinary paragraphs. Send exactly one request; never retry.
                            int timeout = 5 + static_cast<int>(std::ceil(text.size() * 0.15));
                            try {
                                J value = call("/type", {{"device", host}, {"text", text}}, timeout);
                                mouse = value.value("mouse", mouse);
                            } catch (const std::exception &e) {
                                throw std::runtime_error(std::string("iphone-use text request failed: ") +
                                                         e.what() +
                                                         "; not retried. Delivery may be partial if the "
                                                         "connection failed during typing");
                            }
                        }
                    } else if (handlesNavigation(action)) {
                        bool landscape = job.args.value("orientation", "portrait") != "portrait";
                        int w = landscape ? height : width, h = landscape ? width : height;
                        Navigation plan = navigationPlan(action, job.args, w, h);
                        std::string endpoint;
                        J body;
                        if (!plan.key.empty() && consumerAvailable) {
                            endpoint = "/typeSpecialKey";
                            body = {{"key", plan.key}};
                        } else if (!plan.shortcut.empty() && (keyboardAvailable || consumerAvailable)) {
                            // Resolve the helper's current catalog, including local overrides.
                            // Only fall back before submission, never after a failed POST.
                            J catalog = call("/getKeyboardShortcuts");
                            for (const auto &entry : catalog.at("shortcuts")) {
                                if (entry.value("id", "") != plan.shortcut) continue;
                                if (entry.value("disabled", false))
                                    throw std::runtime_error("Keyboard shortcut is disabled: " + plan.shortcut);
                                if (!entry.value("executable", false)) break;
                                bool ready = true;
                                for (const auto &key : entry.at("keys")) {
                                    bool found = false;
                                    for (const auto &definition : catalog.at("keys")) {
                                        if (definition.at("name") != key) continue;
                                        std::string kind = definition.at("kind");
                                        if (kind == "consumer") ready &= consumerAvailable;
                                        else if (kind == "key" || kind == "modifier") ready &= keyboardAvailable;
                                        else throw std::runtime_error("Unsupported navigation shortcut key kind");
                                        found = true;
                                        break;
                                    }
                                    if (!found) throw std::runtime_error("Unknown key in navigation shortcut catalog");
                                }
                                if (ready && !entry.at("keys").empty()) {
                                    endpoint = "/executeKeyboardShortcut";
                                    body = {{"id", plan.shortcut}, {"revision", catalog.at("revision")}};
                                }
                                break;
                            }
                        }
                        auto checkSession = [&] {
                            std::lock_guard<std::mutex> l(mutex);
                            if (!running || job.session != session)
                                throw std::runtime_error("Navigation cancelled: input session changed");
                        };
                        checkSession();
                        if (endpoint.empty() && (!available || plan.points.empty()))
                            throw std::runtime_error("No ready HID key/shortcut or supported mouse fallback for " + plan.gesture);
                        release();
                        if (held) throw std::runtime_error("Could not release mouse before navigation");
                        checkSession();
                        if (!endpoint.empty()) {
                            body["device"] = host;
                            try {
                                result.value = call(endpoint, body);
                            } catch (const std::exception &e) {
                                throw std::runtime_error(std::string("iphone-use navigation request failed: ") +
                                    e.what() + "; not retried; no mouse fallback after submission");
                            }
                            result.value["inputMethod"] = "bluetoothKeyboard";
                        } else {
                            // A self-contained drag does not require enableLiveMouse.
                            // The worker owns the entire sequence; other jobs cannot
                            // interleave. Waits are cancellable; cleanup releases even
                            // when a timeout leaves the last physical report uncertain.
                            J geometry = {{"device", host}, {"width", w}, {"height", h},
                                          {"orientation", job.args.value("orientation", "portrait")}};
                            auto sendPoint = [&](const char *path, double x, double y) {
                                checkSession();
                                J move = geometry;
                                move["x"] = x; move["y"] = y; move["button"] = 1;
                                if (std::string(path) == "/mouseDown") held = true;
                                J value = call(path, move);
                                mouse = value.value("mouse", mouse);
                                held = mouse.value("down", true);
                            };
                            auto pause = [&](double seconds) {
                                std::unique_lock<std::mutex> l(mutex);
                                if (wake.wait_for(l, std::chrono::duration<double>(seconds),
                                                  [&] { return !running || job.session != session; }))
                                    throw std::runtime_error("Navigation cancelled: input session changed");
                            };
                            const auto &start = plan.points.front();
                            sendPoint("/mouseDown", start.x, start.y);
                            for (size_t i = 1; i < plan.points.size(); ++i) {
                                const auto &a = plan.points[i - 1], &b = plan.points[i];
                                double duration = b.seconds - a.seconds;
                                if (a.x == b.x && a.y == b.y) { pause(duration); continue; }
                                int steps = std::max(1, static_cast<int>(std::ceil(duration / .05)));
                                for (int step = 1; step <= steps; ++step) {
                                    pause(duration / steps);
                                    double t = static_cast<double>(step) / steps;
                                    sendPoint("/mouseMove", a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
                                }
                            }
                            checkSession();
                            release();
                            if (held) throw std::runtime_error("Could not confirm mouse release after navigation");
                            result.value = {{"submitted", true}, {"inputMethod", "bluetoothMouse"},
                                            {"mouse", mouse}, {"bluetoothHost", host}};
                        }
                        result.value["gesture"] = plan.gesture;
                    } else if (MouseInput::handlesKeyboard(action)) {
                        if (!keyboardAvailable && !consumerAvailable)
                            throw std::runtime_error("Bluetooth keyboard and consumer controls are not ready");
                        release();
                        if (held)
                            throw std::runtime_error("Could not release mouse before keyboard input");
                        J body = job.args;
                        body["device"] = host; // Never accept a viewer-supplied target.
                        // iphone-use owns aliases, HID usages, chord validation and
                        // per-report readiness checks. In particular, its modern API
                        // distinguishes Delete (forward) from Backspace; /key does not.
                        // Keep frontend agent hints compatible with the existing text
                        // bridge: this provider has one hardware input backend.
                        try {
                            result.value = call("/" + action, body);
                        } catch (const std::exception &e) {
                            throw std::runtime_error(std::string("iphone-use keyboard request failed: ") +
                                e.what() + "; not retried; submission may be uncertain after a transport failure");
                        }
                    } else {
                        if (!available)
                            throw std::runtime_error(error.empty() ? "Bluetooth mouse unavailable" : error);
                        if (action == "enableLiveMouse") {
                            release();
                            if (held)
                                throw std::runtime_error("Could not confirm mouse release");
                            enabled = true;
                        } else {
                            if (!enabled && action != "mouseUp")
                                throw std::runtime_error("Enable live mouse first");
                            J body = job.args;
                            // Selection and dimensions are provider-owned, never supplied by a viewer.
                            body["device"] = host;
                            bool landscape = body.value("orientation", "portrait") != "portrait";
                            body["width"] = landscape ? height : width;
                            body["height"] = landscape ? width : height;
                            if (action == "mouseDown")
                                held = true; // Even an HTTP timeout may have pressed it.
                            J value = call("/" + action, body);
                            mouse = value.value("mouse", mouse);
                            held = mouse.value("down", action != "mouseUp");
                        }
                    }
                    if (result.value.is_null())
                        result.value = {{"mouse", mouse}, {"enabled", enabled}, {"bluetoothHost", host}};
                    if (MouseInput::handlesText(action)) {
                        result.value["charactersSubmitted"] = job.args.at("text").get<std::string>().size();
                        result.value["inputMethod"] = "bluetoothKeyboard";
                    }
                } catch (const std::exception &e) {
                    result.error = e.what();
                    enabled = false;
                    release();
                }
                std::lock_guard<std::mutex> l(mutex);
                if (job.session == session)
                    results.push_back(std::move(result));
            }
            {
                std::lock_guard<std::mutex> l(mutex);
                if (observed == session)
                    published = {{"available", available},
                                 {"keyboardAvailable", keyboardAvailable},
                                 {"consumerControlsAvailable", consumerAvailable},
                                 {"enabled", enabled},
                                 {"bluetoothHost", host},
                                 {"logicalWidthPortrait", width},
                                 {"logicalHeightPortrait", height},
                                 {"mouse", mouse},
                                 {"error", error}};
            }
        }
        release();
    }
};
MouseInput::MouseInput() : impl(new Impl) {}
MouseInput::~MouseInput() {
    stop();
}
void MouseInput::start(const std::string &url) {
    if (url.empty())
        return;
    stop();
    impl->url = url;
    while (!impl->url.empty() && impl->url.back() == '/')
        impl->url.pop_back();
    impl->running = true;
    impl->worker = std::thread([this] { impl->run(); });
}
void MouseInput::stop() {
    {
        std::lock_guard<std::mutex> l(impl->mutex);
        impl->running = false;
        ++impl->session;
        impl->jobs.clear();
        impl->results.clear();
    }
    impl->wake.notify_all();
    if (impl->worker.joinable())
        impl->worker.join();
    std::lock_guard<std::mutex> l(impl->mutex);
    impl->published["available"] = false;
    impl->published["keyboardAvailable"] = false;
    impl->published["consumerControlsAvailable"] = false;
    impl->published["enabled"] = false;
}
void MouseInput::reset() {
    {
        std::lock_guard<std::mutex> l(impl->mutex);
        ++impl->session;
        impl->jobs.clear();
        impl->results.clear();
        impl->published["enabled"] = false;
    }
    impl->wake.notify_all();
}
MouseInput::Json MouseInput::state() {
    std::lock_guard<std::mutex> l(impl->mutex);
    return impl->published;
}
bool MouseInput::take(Result &result) {
    std::lock_guard<std::mutex> l(impl->mutex);
    if (impl->results.empty())
        return false;
    result = std::move(impl->results.front());
    impl->results.pop_front();
    return true;
}
bool MouseInput::handles(const std::string &action) {
    return handlesNavigation(action) || handlesKeyboard(action) || handlesCatalog(action) || action == "enableLiveMouse" || action == "disableLiveMouse" ||
           action == "mouseDown" || action == "mouseMove" || action == "mouseUp";
}
bool MouseInput::handlesText(const std::string &action) {
    return action == "typeText" || action == "tapText";
}
bool MouseInput::handlesKeyboard(const std::string &action) {
    return handlesText(action) || action == "typeSpecialKey" || action == "pressKeyChord" ||
           action == "executeKeyboardShortcut";
}
bool MouseInput::handlesCatalog(const std::string &action) {
    return action == "getKeyboardKeys" || action == "getKeyboardShortcuts";
}
void MouseInput::submit(const Json &request, const Json &args) {
    if (!args.is_object())
        throw std::runtime_error("Input args must be an object");
    J clean = J::object();
    const std::string action = request.at("action");
    auto name = [](const J &value, const char *field) {
        if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
            value.get_ref<const std::string &>().size() > 128)
            throw std::runtime_error(std::string(field) + " must be a nonempty string of at most 128 bytes");
        return value;
    };
    if (handlesNavigation(action)) {
        for (auto key : {"gesture", "duration", "x", "y", "orientation", "modelName"})
            if (args.contains(key)) clean[key] = args[key];
        // Validate names/held-button requests on the worker with its current
        // geometry; never forward arbitrary CF arguments to the helper.
    } else if (handlesText(action))
        clean["text"] = checkedText(args);
    else if (action == "typeSpecialKey")
        clean["key"] = name(args.value("key", J()), "key");
    else if (action == "pressKeyChord") {
        J keys = args.value("keys", J());
        if (!keys.is_array() || keys.empty() || keys.size() > 15)
            throw std::runtime_error("keys must contain 1–15 key names");
        for (const auto &key : keys) name(key, "key");
        clean["keys"] = keys;
    } else if (action == "executeKeyboardShortcut") {
        clean["id"] = name(args.value("id", J()), "id");
        if (args.contains("revision")) {
            if (!args["revision"].is_string() || args["revision"].get_ref<const std::string &>().size() > 1024)
                throw std::runtime_error("revision must be a string of at most 1024 bytes");
            clean["revision"] = args["revision"];
        }
    } else if (handlesCatalog(action)) {
        // Do not forward targets, keys or other caller fields in a discovery GET.
    } else {
        for (auto key : {"x", "y", "path", "button", "orientation"})
            if (args.contains(key))
                clean[key] = args[key];
        auto point = [](const J &p) {
            for (auto key : {"x", "y"})
                if (!p.is_object() || !p.contains(key) || !p[key].is_number() ||
                    !std::isfinite(p[key].get<double>()) || std::abs(p[key].get<double>()) > 100000)
                    throw std::runtime_error("Mouse x/y must be finite logical coordinates");
        };
        if (clean.contains("x") || clean.contains("y"))
            point(clean);
        if (clean.contains("path")) {
            if (!clean["path"].is_array() || clean["path"].size() > 1000)
                throw std::runtime_error("Mouse path must contain at most 1000 points");
            for (auto &p : clean["path"]) {
                point(p);
                p = {{"x", p["x"]}, {"y", p["y"]}};
            }
        }
        if (clean.contains("button") &&
            (!clean["button"].is_number_integer() || clean["button"] < 1 || clean["button"] > 3))
            throw std::runtime_error("Mouse button must be 1, 2, or 3");
    }
    std::lock_guard<std::mutex> l(impl->mutex);
    if (!impl->running)
        throw std::runtime_error("Start UxPlay with --controlfloor-mouse to enable Bluetooth input");
    if (impl->jobs.size() + impl->results.size() >= 63) {
        ++impl->session;
        impl->jobs.clear();
        impl->results.clear();
        impl->published["enabled"] = false;
        impl->wake.notify_all();
        throw std::runtime_error("Mouse queue full; cancelled input and releasing buttons");
    }
    impl->jobs.push_back({request, clean, impl->session, Clock::now()});
    impl->wake.notify_all();
}
} // namespace controlfloor
