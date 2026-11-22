// SPDX-License-Identifier: GPL-3.0-or-later
#include "mouse_input.h"
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
        bool available = false, keyboardAvailable = false, enabled = false, held = false;
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
                    available = keyboardAvailable = enabled = false;
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
                    if (action == "disableLiveMouse") {
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
    return handlesText(action) || action == "enableLiveMouse" || action == "disableLiveMouse" ||
           action == "mouseDown" || action == "mouseMove" || action == "mouseUp";
}
bool MouseInput::handlesText(const std::string &action) {
    return action == "typeText" || action == "tapText";
}
void MouseInput::submit(const Json &request, const Json &args) {
    if (!args.is_object())
        throw std::runtime_error("Input args must be an object");
    J clean = J::object();
    if (handlesText(request.at("action")))
        clean["text"] = checkedText(args);
    else {
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
