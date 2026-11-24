// SPDX-License-Identifier: GPL-3.0-or-later
#include "controlfloor.h"
#include "mouse_input.h"
#include "audio.h"
#include "identity.h"
#include "pairing.h"
#include <sys/file.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <json.hpp>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
std::string deviceID = "airplay-test-12345";
std::mutex shutdownMutex;
std::once_flag exitCleanup;
controlfloor::IdentityGate identityGate;
controlfloor::Identity identified;
std::mutex lifecycleMutex;
std::atomic<bool> connected{false};
std::atomic<unsigned long> connectionRevision{0};
unsigned long sampleConnection = 0, sampleSequence = 0;
struct Options {
    bool enabled = false, insecure = false, registration = false, test = false, lan = false;
    controlfloor::PairingOptions pairing;
    std::string server, config, mouseURL, logDirectory = "log";
    double ocrInterval = 5;
} opt;
std::atomic<bool> running{false}, online{false}, active{false}, forwarding{false}, dirty{false},
    screenshotDemand{false};
// CF sends an application-level ping every 20 seconds. A successful upgrade
// alone cannot tell us that its provider command loop is still functioning.
std::atomic<int64_t> lastServerMessage{0};
std::atomic<bool> serverConfirmed{false};
int64_t steadyMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
std::atomic<unsigned long> epoch{1}, dropped{0}, encoded{0}, sent{0};
std::mutex stateMutex, frameMutex, commandMutex;
std::string phoneName = "AirPlay", phoneModel = "iPhone", generation;
std::string rawCaps, encoderError;
std::string airplayModel;
bool prepared = false;
std::atomic<unsigned long> captured{0}, encodeAttempts{0}, encodeFailures{0};
int nativeWidth = 0, nativeHeight = 0;
J settings;
controlfloor::MouseInput mouseInput;
controlfloor::AudioStream audioStream;
std::unique_ptr<controlfloor::PairingCoordinator> coordinator;
void (*pairingTransport)(const char *) = nullptr;
std::mutex pairingMutex;
std::deque<J> pairingMessages;
J binding = J::object(); // Network thread only.
std::string pairingToken; // Protected by pairingMutex; socket callbacks also report it.
J remoteInput = J::object();
void pairingSend(J message) {
    message["connection"] = connectionRevision.load();
    { std::lock_guard<std::mutex> lock(pairingMutex); message["token"] = pairingToken; }
    if (pairingTransport) pairingTransport(message.dump().c_str());
}
void resetInput() {
    mouseInput.reset();
    if (opt.lan) pairingSend({{"type","reset-input"}});
}
J inputState() { return opt.lan ? remoteInput : mouseInput.state(); }

J mouseState = {{"available", false},
                {"enabled", false},
                {"logicalWidthPortrait", 375},
                {"logicalHeightPortrait", 667},
                {"mouse", J::object()}};
int logicalWidth() {
    return opt.mouseURL.empty() ? std::min(nativeWidth, nativeHeight)
                                : mouseState.value("logicalWidthPortrait", 375);
}
int logicalHeight() {
    return opt.mouseURL.empty() ? std::max(nativeWidth, nativeHeight)
                                : mouseState.value("logicalHeightPortrait", 667);
}
GstSample *latest = nullptr;
unsigned long sampleEpoch = 0;
struct Frame {
    std::string bytes, generation;
    int width = 0, height = 0;
    unsigned long epoch = 0;
    int64_t timestamp = 0;
} pending;
std::deque<J> commands;
std::thread encodeThread, networkThread;
ix::WebSocket cfSocket;
J credentials = J::object();
int64_t millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void log(const char *s) {
    fprintf(stderr, "ControlFloor: %s\n", s);
    identityGate.log("provider", s);
}
J defaults() {
    return {{"maxFramesPerSecond", 15.0},
            {"maxBytesPerSecond", 0},
            {"maxDimension", 1280},
            {"maxScale", 1.0},
            {"jpegQuality", 70},
            {"flowControlVersion", 1},
            {"adaptationPolicy", "fixed"},
            {"videoQualityPreset", "custom"},
            {"burstDuration", 0},
            {"burst", J::object()},
            {"assumeAvailableBytesPerSecond", 0},
            {"providerMaxFramesPerSecond", 0},
            {"providerMaxBytesPerSecond", 0},
            {"clientWidthPortrait", 0},
            {"clientHeightPortrait", 0},
            {"clientWidthLandscape", 0},
            {"clientHeightLandscape", 0}};
}
J encoderDiagnostics() {
    std::lock_guard<std::mutex> l(stateMutex);
    return {{"capturedFrames", captured.load()},
            {"encodeAttempts", encodeAttempts.load()},
            {"encodeFailures", encodeFailures.load()},
            {"rawCaps", rawCaps},
            {"lastError", encoderError}};
}
J snapshot() {
    std::lock_guard<std::mutex> l(stateMutex);
    J j = settings;
    auto audio=audioStream.configuration();
    j["audioEncoding"]=audio["encoding"]; j["audioSilence"]=audio["silence"];
    j["audioEnabled"]=audioStream.enabled() ? 1 : 0;
    j["supportedAudioEncodings"]=audio["supportedAudioEncodings"];
    j["nativeAudioEncoding"]=audio["nativeAudioEncoding"];
    j["nativeWidthPortrait"] = std::min(nativeWidth, nativeHeight);
    j["nativeHeightPortrait"] = std::max(nativeWidth, nativeHeight);
    // Mouse geometry is independent of AirPlay/JPEG pixels. Without input, pixels remain provisional.
    j["logicalWidthPortrait"] = logicalWidth();
    j["logicalHeightPortrait"] = logicalHeight();
    return j;
}
J info() {
    std::lock_guard<std::mutex> l(stateMutex);
    return {{"name", phoneName},
            {"deviceType", phoneModel},
            {"udid", deviceID},
            {"osVersion", identified.osVersion},
            {"serialNumber", identified.serial},
            {"hardwareModelName", identified.modelName},
            {"hardwareModelNumber", identified.modelNumber},
            {"keyboardId", identified.keyboard},
            {"logicalWidthPortrait", logicalWidth()},
            {"logicalHeightPortrait", logicalHeight()}};
}
J status() {
    std::lock_guard<std::mutex> l(stateMutex);
    return {{"providerStatus", "Connected"},
            {"providerStatusDetail", active ? "AirPlay provider" : "Waiting for AirPlay"},
            {"providerHostname", g_get_host_name()},
            {"videoSource", active ? "UxPlay" : "None"},
            {"liveMouse", active && mouseState.value("available", false)},
            {"mouse", mouseState.value("mouse", J::object())},
            {"mouseEnabled", mouseState.value("enabled", false)},
            {"keyboardAvailable", active && mouseState.value("keyboardAvailable", false)},
            {"consumerControlsAvailable", active && mouseState.value("consumerControlsAvailable", false)},
            {"mouseError", mouseState.value("error", std::string())},
            {"cfaStatus", "Unavailable"},
            {"wdaStatus", "Unavailable"},
            {"interfaceOrientation", nativeWidth > nativeHeight ? "LandscapeLeft" : "Portrait"}};
}
std::string currentGeneration() {
    std::lock_guard<std::mutex> l(stateMutex);
    return generation;
}
J capabilities() {
    J input = inputState();
    bool keyboard = active && input.value("keyboardAvailable", false);
    bool consumer = active && input.value("consumerControlsAvailable", false);
    // These compatibility flags mean tapText can be accepted immediately via
    // Bluetooth. They do not claim a native CFAgent or on-screen-keyboard calibration.
    return {{"capabilities",
             {{"tapTextCapable", keyboard ? 1 : 0},
              {"tapTextInitialized", keyboard ? 1 : 0},
              {"initializationState", 0},
              {"keyboards", J::array()},
              {"video", true},
              {"audio", bool(audioStream.available)},
              {"input", keyboard || consumer || (active && input.value("available", false))},
              {"textInputMethod", "bluetoothKeyboard"},
              {"nativeIOS", false}}}};
}
J notice(const std::string &type, const J &value) {
    return {{"cf", {{"d", deviceID}}}, {"type", type}, {"status", "notice"}, {"value", value}};
}
J reply(const J &r, const J &v = nullptr, const std::string &error = "") {
    std::string action =
                    r.contains("action") && r["action"].is_string() ? r["action"].get<std::string>() : "",
                type = "ack";
    if (action == "ping")
        type = "pong";
    else if (action.compare(0, 3, "get") == 0)
        type = action.substr(3);
    J out = {{"type", error.empty() ? type : "error"}, {"status", error.empty() ? "ok" : "error"}};
    for (auto key : {"cf", "tag", "events"})
        if (r.contains(key))
            out[key] = r[key];
    if (!v.is_null())
        out["value"] = v;
    if (!error.empty())
        out["error"] = error;
    return out;
}
void send(const J &j) {
    if (active && online && !cfSocket.sendText(j.dump()).success)
        online = false;
}
void clearFrames() {
    std::lock_guard<std::mutex> l(frameMutex);
    if (latest)
        gst_sample_unref(latest);
    latest = nullptr;
    pending = Frame();
}
void invalidate(bool retainDisplay = false) {
    epoch++;
    std::lock_guard<std::mutex> l(frameMutex);
    if (!retainDisplay) {
        if (latest)
            gst_sample_unref(latest);
        latest = nullptr;
    }
    sampleEpoch = epoch;
    pending = Frame();
}
std::string newGeneration() {
    gchar *uuid = g_uuid_string_random();
    std::string s(uuid);
    g_free(uuid);
    return s;
}
ix::SocketTLSOptions tls() {
    ix::SocketTLSOptions t;
    if (opt.insecure) {
        t.caFile = "NONE";
        t.disable_hostname_validation = true;
    }
    return t;
}
std::string escape(const std::string &s) {
    gchar *p = g_uri_escape_string(s.c_str(), nullptr, FALSE);
    std::string r(p);
    g_free(p);
    return r;
}
ix::HttpResponsePtr post(const std::string &path, const J &fields, const std::string &cookie = "") {
    ix::HttpClient client;
    client.setTLSOptions(tls());
    auto a = client.createRequest();
    a->connectTimeout = 5;
    a->transferTimeout = 10;
    a->followRedirects = false;
    a->compress = false;
    a->extraHeaders["Content-Type"] = "application/x-www-form-urlencoded";
    if (!cookie.empty())
        a->extraHeaders["Cookie"] = cookie;
    std::string body;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (!body.empty())
            body += '&';
        body += escape(it.key()) + "=" + escape(it.value().get<std::string>());
    }
    return client.post(opt.server + path, body, a);
}
std::string login(const std::string &user, const std::string &password) {
    auto r = post("/login?noredirect=1", {{"username", user}, {"password", password}});
    if (r->statusCode != 200) {
        std::string reason = "login failed (HTTP " + std::to_string(r->statusCode) + ")";
        if (r->statusCode == 401 || r->statusCode == 403)
            reason += "; credentials rejected; retrying will not repair an invalid provider account";
        throw std::runtime_error(reason);
    }
    std::string cookie;
    for (auto &h : r->headers) {
        std::string key = h.first;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "set-cookie")
            cookie = h.second.substr(0, h.second.find(';'));
    }
    if (cookie.empty())
        throw std::runtime_error("login returned no session cookie");
    return cookie;
}
std::string env(const char *key) {
    const char *v = getenv(key);
    return v ? v : "";
}
void saveCredentials() {
    gchar *parent = g_path_get_dirname(opt.config.c_str());
    if (g_mkdir_with_parents(parent, 0700) != 0) {
        g_free(parent);
        throw std::runtime_error("cannot create config directory");
    }
    g_free(parent);
    std::string temp = opt.config + ".XXXXXX";
    std::vector<char> path(temp.begin(), temp.end());
    path.push_back(0);
    int fd = mkstemp(path.data());
    if (fd < 0)
        throw std::runtime_error("cannot create private config");
    std::string data = credentials.dump(2) + "\n";
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = write(fd, data.data() + offset, data.size() - offset);
        if (n <= 0) {
            close(fd);
            unlink(path.data());
            throw std::runtime_error("cannot write config");
        }
        offset += n;
    }
    fsync(fd);
    close(fd);
    if (rename(path.data(), opt.config.c_str()) != 0) {
        unlink(path.data());
        throw std::runtime_error("cannot save config");
    }
}
bool configure() {
    if (opt.config.empty())
        opt.config = std::string(g_get_user_config_dir()) + "/uxplay/controlfloor.json";
    std::ifstream f(opt.config);
    if (f.good())
        f >> credentials;
    if (opt.server.empty())
        opt.server = credentials.value("server", std::string());
    while (!opt.server.empty() && opt.server.back() == '/')
        opt.server.pop_back();
    if (opt.server.compare(0, 8, "https://") != 0 && opt.server.compare(0, 7, "http://") != 0)
        throw std::runtime_error("specify --controlfloor-server http(s)://host:port");
    if (opt.registration) {
        std::string user = env("CF_ADMIN_USERNAME"), password = env("CF_ADMIN_PASSWORD");
        if (user.empty() || password.empty())
            throw std::runtime_error(
                "registration needs CF_ADMIN_USERNAME and CF_ADMIN_PASSWORD environment variables");
        auto cookie = login(user, password);
        auto response = post("/admin/registerProvider?noredirect=1",
                             {{"username", user},
                              {"password", password},
                              {"providerUsername", env("CF_PROVIDER_USERNAME")},
                              {"providerDescription", std::string("UxPlay on ") + g_get_host_name()}},
                             cookie);
        if (response->statusCode != 200)
            throw std::runtime_error("registration failed (HTTP " + std::to_string(response->statusCode) +
                                     ")");
        J r = J::parse(response->body);
        if (!r.value("Error", std::string()).empty())
            throw std::runtime_error("registration rejected by server");
        credentials = {
            {"server", opt.server}, {"username", r.at("Username")}, {"password", r.at("Password")}};
        if (credentials["username"].get<std::string>().empty() ||
            credentials["password"].get<std::string>().empty())
            throw std::runtime_error("registration returned empty credentials");
        saveCredentials();
        fprintf(stderr, "ControlFloor: registered provider %s; saved %s\n",
                credentials["username"].get<std::string>().c_str(), opt.config.c_str());
        return false;
    }
    if (credentials.value("server", std::string()) != opt.server)
        throw std::runtime_error("config belongs to another server; use a separate --controlfloor-config");
    if (!credentials.contains("username") || !credentials.contains("password"))
        throw std::runtime_error("register first with --controlfloor-register");
    return true;
}
// Validate the entire patch before publishing it. Zero means unchanged; -2 resets.
void mergeSettings(J &target, const J &patch, bool burst = false) {
    if (!patch.is_object())
        throw std::runtime_error("configuration must be an object");
    const J def = defaults();
    const std::map<std::string, double> limits = {{"maxFramesPerSecond", 240},
                                                  {"maxBytesPerSecond", 1e9},
                                                  {"maxDimension", 8192},
                                                  {"maxScale", 1},
                                                  {"jpegQuality", 100},
                                                  {"flowControlVersion", 2},
                                                  {"burstDuration", 60},
                                                  {"assumeAvailableBytesPerSecond", 1e9},
                                                  {"providerMaxFramesPerSecond", 240},
                                                  {"providerMaxBytesPerSecond", 1e9},
                                                  {"clientWidthPortrait", 8192},
                                                  {"clientHeightPortrait", 8192},
                                                  {"clientWidthLandscape", 8192},
                                                  {"clientHeightLandscape", 8192}};
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto &k = it.key();
        const auto &v = it.value();
        if (k == "vidstream") {
            if (!v.is_object())
                throw std::runtime_error("vidstream must be an object");
            continue;
        }
        if (k == "logicalWidthPortrait" || k == "logicalHeightPortrait" || k == "nativeWidthPortrait" ||
            k == "nativeHeightPortrait")
            continue;
        if (k == "burst") {
            if (burst)
                throw std::runtime_error("nested burst unsupported");
            J b = target.value("burst", J::object());
            mergeSettings(b, v, true);
            target[k] = b;
            continue;
        }
        if (k == "adaptationPolicy") {
            if (!v.is_string())
                throw std::runtime_error("invalid adaptationPolicy");
            auto s = v.get<std::string>();
            if (s.empty())
                continue;
            if (s != "fixed" && s != "freshness")
                throw std::runtime_error("supported adaptationPolicy: fixed, freshness");
            target[k] = s;
            continue;
        }
        if (k == "videoQualityPreset") {
            if (!v.is_string())
                throw std::runtime_error("invalid preset");
            auto s = v.get<std::string>();
            if (s.empty())
                continue;
            static const std::map<std::string, std::pair<int, int>> presets = {{"ultra_low", {480, 30}},
                                                                               {"low", {640, 45}},
                                                                               {"standard", {848, 60}},
                                                                               {"high", {1280, 80}},
                                                                               {"max", {0, 95}}};
            if (s != "custom" && !presets.count(s))
                throw std::runtime_error("unknown videoQualityPreset");
            if (s != "custom") {
                target["maxDimension"] = presets.at(s).first;
                target["jpegQuality"] = presets.at(s).second;
            }
            target[k] = s;
            continue;
        }
        if (k == "maxVideoCPUPercentage" || k == "allowUnsafeFrameRate") {
            if (!v.is_number())
                throw std::runtime_error("numeric CPU option required");
            continue;
        }
        if (!limits.count(k))
            throw std::runtime_error("unsupported configuration field: " + k);
        if (!v.is_number())
            throw std::runtime_error("numeric value required: " + k);
        double n = v.get<double>();
        if (!std::isfinite(n) || n < -2 || n > limits.at(k) || (n < 0 && n != -1 && n != -2))
            throw std::runtime_error("out of range: " + k);
        if (n == 0)
            continue;
        if (n == -2)
            target[k] = def.at(k);
        else if (n == -1)
            target[k] = (k == "jpegQuality" ? 100 : k == "maxScale" ? 1 : k == "flowControlVersion" ? 1 : 0);
        else if (k == "maxFramesPerSecond" || k == "maxScale" || k == "burstDuration")
            target[k] = n;
        else {
            if (std::floor(n) != n)
                throw std::runtime_error("integer required: " + k);
            target[k] = static_cast<int64_t>(n);
        }
    }
    if (patch.contains("videoQualityPreset")) {
        for (auto key : {"maxDimension", "jpegQuality"})
            if (patch.contains(key))
                mergeSettings(target, J{{key, patch[key]}}, burst);
    }
    if (!burst && patch.contains("vidstream") && !patch["vidstream"].empty()) {
        if (patch["vidstream"].contains("vidstream"))
            throw std::runtime_error("nested vidstream unsupported");
        mergeSettings(target, patch["vidstream"], false);
    }
}
GstFlowReturn captureAudio(GstAppSink *sink, gpointer) {
    GstSample *sample=gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    if (running && active && online && connected && audioStream.enabled()) {
        GstMapInfo map;
        GstBuffer *buffer=gst_sample_get_buffer(sample);
        if (buffer && gst_buffer_map(buffer,&map,GST_MAP_READ)) {
            audioStream.capture(map.data,map.size);
            gst_buffer_unmap(buffer,&map);
        }
    }
    gst_sample_unref(sample); return GST_FLOW_OK;
}
GstFlowReturn capture(GstAppSink *sink, gpointer) {
    const unsigned long revision = connectionRevision;
    GstSample *s = gst_app_sink_pull_sample(sink);
    if (!s)
        return GST_FLOW_OK;
    // The HTML viewer asks for native dimensions BEFORE it opens a video socket.
    // Observe negotiated raw caps even when forwarding is off. Never encode or
    // wait for network/ACK progress just to discover geometry.
    if (running && connected) {
        captured++;
        GstVideoInfo vi;
        gst_video_info_init(&vi);
        if (gst_video_info_from_caps(&vi, gst_sample_get_caps(s))) {
            int width = GST_VIDEO_INFO_WIDTH(&vi), height = GST_VIDEO_INFO_HEIGHT(&vi);
            std::unique_lock<std::mutex> l(stateMutex, std::try_to_lock);
            if (l.owns_lock() && connected && revision == connectionRevision && (width != nativeWidth || height != nativeHeight)) {
                nativeWidth = width;
                nativeHeight = height;
                dirty = true;
            }
        }
    }
    if (running && connected) {
        std::unique_lock<std::mutex> l(frameMutex, std::try_to_lock);
        if (l.owns_lock() && connected && revision == connectionRevision) {
            if (latest) {
                gst_sample_unref(latest);
                dropped++;
            }
            latest = s;
            sampleEpoch = epoch;
            sampleConnection = revision;
            ++sampleSequence;
            s = nullptr;
        } else
            dropped++;
    }
    if (s)
        gst_sample_unref(s);
    return GST_FLOW_OK;
}
void encoder() {
    GstElement *pipe = nullptr, *src = nullptr, *sink = nullptr;
    std::string previous;
    auto last = Clock::now() - std::chrono::seconds(1);
    auto burstStart = last;
    bool wasForwarding = false;
    while (running) {
        if (!active || !online || (!forwarding && !screenshotDemand)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        bool enabled = forwarding;
        if (enabled && !wasForwarding)
            burstStart = Clock::now();
        wasForwarding = enabled;
        GstSample *sample = nullptr;
        unsigned long e = 0;
        {
            std::lock_guard<std::mutex> l(frameMutex);
            // Retain the displayed raw frame: an unchanged AirPlay screen may
            // supply no new frames between opening the viewer and startVideo.
            sample = latest ? gst_sample_ref(latest) : nullptr;
            e = sampleEpoch;
        }
        if (!sample) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        J conf = snapshot();
        if (std::chrono::duration<double>(Clock::now() - burstStart).count() <
            conf.value("burstDuration", 0.0))
            conf.update(conf["burst"]);
        double fps = conf.value("maxFramesPerSecond", 15.0),
               providerFPS = conf.value("providerMaxFramesPerSecond", 0.0);
        if (providerFPS > 0)
            fps = fps > 0 ? std::min(fps, providerFPS) : providerFPS;
        if (fps > 0 && std::chrono::duration<double>(Clock::now() - last).count() < 1.0 / fps) {
            gst_sample_unref(sample);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        GstVideoInfo vi;
        gst_video_info_init(&vi);
        if (!gst_video_info_from_caps(&vi, gst_sample_get_caps(sample))) {
            gst_sample_unref(sample);
            continue;
        }
        int w = GST_VIDEO_INFO_WIDTH(&vi), h = GST_VIDEO_INFO_HEIGHT(&vi);
        std::string frameGeneration;
        {
            std::lock_guard<std::mutex> l(stateMutex);
            frameGeneration = generation;
        }
        double scale = std::min(1.0, conf.value("maxScale", 1.0));
        if (scale <= 0)
            scale = 1;
        double dim = conf.value("maxDimension", 0.0);
        if (dim > 0)
            scale = std::min(scale, dim / std::max(w, h));
        double cw = conf.value(w > h ? "clientWidthLandscape" : "clientWidthPortrait", 0.0),
               ch = conf.value(w > h ? "clientHeightLandscape" : "clientHeightPortrait", 0.0);
        if (cw > 0)
            scale = std::min(scale, cw / w);
        if (ch > 0)
            scale = std::min(scale, ch / h);
        int ow = std::max(2, static_cast<int>(w * scale) / 2 * 2),
            oh = std::max(2, static_cast<int>(h * scale) / 2 * 2);
        int quality = conf.value("jpegQuality", 70);
        gchar *caps = gst_caps_to_string(gst_sample_get_caps(sample));
        {
            std::lock_guard<std::mutex> l(stateMutex);
            rawCaps = caps;
        }
        encodeAttempts++;
        std::string key = std::string(caps) + ":" + std::to_string(ow) + ":" + std::to_string(oh) + ":" +
                          std::to_string(quality);
        g_free(caps);
        if (key != previous) {
            if (pipe) {
                gst_element_set_state(pipe, GST_STATE_NULL);
                gst_object_unref(src);
                gst_object_unref(sink);
                gst_object_unref(pipe);
                pipe = nullptr;
            }
            std::string desc = "appsrc name=src is-live=true format=time block=false max-bytes=0 ! "
                               "videoconvert ! videoscale ! video/x-raw,format=I420,width=" +
                               std::to_string(ow) + ",height=" + std::to_string(oh) +
                               " ! jpegenc quality=" + std::to_string(quality) +
                               " ! appsink name=sink sync=false max-buffers=1 drop=true";
            GError *error = nullptr;
            pipe = gst_parse_launch(desc.c_str(), &error);
            if (error) {
                log(error->message);
                g_error_free(error);
                if (pipe)
                    gst_object_unref(pipe);
                pipe = nullptr;
                gst_sample_unref(sample);
                continue;
            }
            src = gst_bin_get_by_name(GST_BIN(pipe), "src");
            sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
            // The JPEG pipeline consumes CPU memory, even when the desktop
            // renderer negotiated GPU textures. Preserve video geometry/color
            // information, but do not propagate the memory:GLMemory feature.
            GstCaps *cpuCaps = gst_video_info_to_caps(&vi);
            gst_app_src_set_caps(GST_APP_SRC(src), cpuCaps);
            gst_caps_unref(cpuCaps);
            gst_element_set_state(pipe, GST_STATE_PLAYING);
            previous = key;
        }
        // GST_MAP_READ asks GLMemory to download pixels through its own GL
        // context. Do this only on the encoder worker. Copy planes with their
        // actual strides into a plain CPU buffer; copying the GstBuffer header
        // alone leaves the underlying memory GPU-backed.
        GstVideoFrame input = {}, output = {};
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, vi.size, nullptr);
        bool inputMapped = gst_video_frame_map(&input, &vi, gst_sample_get_buffer(sample), GST_MAP_READ);
        bool outputMapped = buffer && gst_video_frame_map(&output, &vi, buffer, GST_MAP_WRITE);
        bool copied = inputMapped && outputMapped && gst_video_frame_copy(&output, &input);
        if (inputMapped)
            gst_video_frame_unmap(&input);
        if (outputMapped)
            gst_video_frame_unmap(&output);
        if (!copied) {
            if (buffer)
                gst_buffer_unref(buffer);
            gst_sample_unref(sample);
            encodeFailures++;
            {
                std::lock_guard<std::mutex> l(stateMutex);
                encoderError = "Could not map/copy decoded frame into CPU memory";
            }
            last = Clock::now();
            continue;
        }
        GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
        gst_sample_unref(sample);
        GstSample *jpg = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 500 * GST_MSECOND);
        if (!jpg) {
            encodeFailures++;
            std::string reason = "JPEG encoder timed out waiting for output";
            GstBus *bus = gst_element_get_bus(pipe);
            GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (message) {
                GError *failure = nullptr;
                gchar *detail = nullptr;
                gst_message_parse_error(message, &failure, &detail);
                reason = failure ? failure->message : "GStreamer encoder error";
                if (detail)
                    reason += std::string(": ") + detail;
                g_clear_error(&failure);
                g_free(detail);
                gst_message_unref(message);
            }
            gst_object_unref(bus);
            {
                std::lock_guard<std::mutex> l(stateMutex);
                if (encoderError != reason)
                    log(reason.c_str());
                encoderError = reason;
            }
            previous.clear();
            continue;
        }
        GstMapInfo map;
        if (gst_buffer_map(gst_sample_get_buffer(jpg), &map, GST_MAP_READ)) {
            if (map.size <= 16 * 1024 * 1024) {
                Frame f;
                f.bytes.assign(reinterpret_cast<char *>(map.data), map.size);
                f.width = ow;
                f.height = oh;
                f.epoch = e;
                f.generation = frameGeneration;
                f.timestamp = millis();
                std::lock_guard<std::mutex> l(frameMutex);
                if (e == epoch && (forwarding || screenshotDemand)) {
                    if (!pending.bytes.empty())
                        dropped++;
                    pending = std::move(f);
                    encoded++;
                }
            }
            gst_buffer_unmap(gst_sample_get_buffer(jpg), &map);
        }
        gst_sample_unref(jpg);
        last = Clock::now();
    }
    if (pipe) {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(src);
        gst_object_unref(sink);
        gst_object_unref(pipe);
    }
}
// File locks prevent two local workers from registering the same OCR serial or
// driving the sole input helper concurrently. They are released on crash/exit.
class Lease {
    int fd = -1;
  public:
    ~Lease() { release(); }
    void release() { if (fd >= 0) { close(fd); fd = -1; } }
    bool acquire(const std::string &key) {
        release();
        gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key.c_str(), -1);
        std::string path = opt.logDirectory + "/.lease-" + hash;
        g_free(hash);
        fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) { release(); return false; }
        return true;
    }
};
void network() {
    unsigned long observedConnection = 0;
    bool leaseWarning = false;
    auto claimAt = Clock::now();
    Lease deviceLease, inputLease;
    auto retry = Clock::now();
    unsigned long attempt = 0;
    unsigned long observedEpoch = 0;
    int frameID = 0, lastAck = 0;
    std::map<int, Clock::time_point> flights;
    auto pairingStatusAt = Clock::time_point::min();
    auto lastProbe = Clock::now(), budgetAt = Clock::now(), blockedAt = Clock::now();
    double budget = 0;
    bool wasBlocked = false;
    J screenshot;
    auto screenshotDeadline = Clock::now();
    cfSocket.setTLSOptions(tls());
    cfSocket.disableAutomaticReconnection();
    cfSocket.disablePerMessageDeflate();
    cfSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr &m) {
        if (m->type == ix::WebSocketMessageType::Open) {
            lastServerMessage = steadyMillis();
            serverConfirmed = false;
            online = true;
            dirty = true;
            log("provider WebSocket transport open; awaiting ControlFloor commands");
        } else if (m->type == ix::WebSocketMessageType::Close || m->type == ix::WebSocketMessageType::Error) {
            online = false;
            audioStream.stop();
            serverConfirmed = false;
            resetInput();
            if (m->type == ix::WebSocketMessageType::Close) {
                std::string message = "provider WebSocket closed (code " + std::to_string(m->closeInfo.code) +
                    (m->closeInfo.remote ? ", remote)" : ", local)");
                log(message.c_str());
                identityGate.log("provider-websocket-close", {{"code", m->closeInfo.code},
                    {"remote", m->closeInfo.remote}, {"retryEnabled", bool(running && active)}});
            } else {
                std::string message = "provider WebSocket error (HTTP " + std::to_string(m->errorInfo.http_status) + ")";
                log(message.c_str());
                identityGate.log("provider-websocket-error", {{"httpStatus", m->errorInfo.http_status},
                    {"retryEnabled", bool(running && active)}});
            }
        } else if (m->type == ix::WebSocketMessageType::Message && !m->binary) {
            if (m->str.size() > 1024 * 1024) {
                online = false;
                return;
            }
            J j = J::parse(m->str, nullptr, false);
            if (!j.is_object())
                return;
            if (j.contains("action") && j["action"].is_string() && !j["action"].get<std::string>().empty()) {
                lastServerMessage = steadyMillis();
                if (!serverConfirmed.exchange(true))
                    log("ControlFloor protocol active; received server command");
            }
            std::lock_guard<std::mutex> l(commandMutex);
            if (commands.size() < 128)
                commands.push_back(std::move(j));
            else
                online = false;
        }
    });
    auto revokeProvider = [&] {
        active=false;
        audioStream.stop();
        cfSocket.stop(); // Join old callbacks before changing the binding token.
        online=false; serverConfirmed=false;
        resetInput();
        deviceLease.release(); inputLease.release();
        forwarding=false; screenshotDemand=false; screenshot=nullptr; flights.clear();
        invalidate(true); // Preserve local AirPlay playback and the latest decoded frame.
        { std::lock_guard<std::mutex> lock(commandMutex); commands.clear(); }
        binding=J::object(); remoteInput=J::object();
        { std::lock_guard<std::mutex> lock(pairingMutex); pairingToken.clear(); }
        identityGate.pairing(connectionRevision.load(),"",false);
        attempt=0; leaseWarning=false; claimAt=Clock::now(); retry=claimAt; dirty=true;
    };
    while (running) {
        auto now = Clock::now();
        unsigned long revision = connectionRevision;
        if (revision != observedConnection) {
            // Socket stop/join runs only here, never in the AirPlay callbacks.
            active = false;
            audioStream.stop();
            cfSocket.stop();
            online = false;
            mouseInput.stop();
            resetInput();
            deviceLease.release(); inputLease.release();
            forwarding = false; screenshotDemand = false; screenshot = nullptr;
            flights.clear();
            { std::lock_guard<std::mutex> l(commandMutex); commands.clear(); }
            if (opt.lan) {
                binding=J::object(); remoteInput=J::object();
                std::lock_guard<std::mutex> lock(pairingMutex); pairingToken.clear();
            }
            observedConnection = revision;
            attempt = 0;
            leaseWarning = false; claimAt = now;
            retry = now;
        }
        if (opt.lan) {
            std::deque<J> messages;
            { std::lock_guard<std::mutex> l(pairingMutex); messages.swap(pairingMessages); }
            for (const auto &message : messages) {
                if (message.value("connection",0UL) != revision) continue;
                std::string kind = message.value("type", "");
                if (kind == "binding") {
                    std::string nextToken=message.value("token","");
                    if (nextToken!=binding.value("token","")) revokeProvider();
                    binding = message;
                    { std::lock_guard<std::mutex> lock(pairingMutex); pairingToken=nextToken; }
                    remoteInput = message.value("input",J::object());
                    identityGate.pairing(revision,message.value("token", ""),message.value("about",false));
                } else if (kind == "input-result" && active && online && message.value("token","")==binding.value("token","")) {
                    send(reply(message.at("request"),message.value("value",J()),message.value("error", "")));
                } else if (kind == "shortcut-progress" && active && online && message.value("token","")==binding.value("token","")) {
                    J progress = notice("ShortcutProgress",message.at("value"));
                    for (auto key : {"cf","tag","events"})
                        if (message.at("request").contains(key)) progress[key] = message.at("request")[key];
                    send(progress);
                } else if (kind == "revoke" && message.value("token","")==binding.value("token","")) {
                    revokeProvider();
                }
            }
            if (now >= pairingStatusAt) {
                J status = identityGate.progress(revision);
                status["type"] = "status"; status["connected"] = connected.load();
                status["providerConnected"] = active && online && serverConfirmed;
                status["providerTransportOpen"] = active && online;
                pairingSend(status); pairingStatusAt = now + std::chrono::milliseconds(250);
            }
        }
        if (connected && !active && revision == observedConnection && now >= claimAt) {
            controlfloor::Identity identity;
            bool ready = identityGate.ready(revision, identity);
            // An explicit synthetic source has no phone/Bonjour listener. Keep the
            // old protocol tests independent of OCR; real AirPlay never bypasses it.
            if (opt.test) {
                identity.serial = "test-12345"; identity.name = "synthetic-airplay-id";
                identity.modelName = "Synthetic video"; ready = true;
            }
            if (opt.lan) {
                ready = ready && binding.value("connection",0UL) == revision && binding.value("admit",false);
                if (ready) identity.keyboard = binding.value("keyboard", "");
            }
            if (ready) {
                std::lock_guard<std::mutex> session(lifecycleMutex);
                if (revision == connectionRevision && connected) {
                    std::string id = "airplay-" + identity.serial;
                    if (!deviceLease.acquire("device:" + id)) {
                        // A duplicate serial must never evict another provider.
                        if (!leaseWarning) log("Duplicate device identity is already registered by another local worker; waiting");
                        leaseWarning = true; claimAt = now + std::chrono::seconds(1);
                        continue;
                    }
                    { std::lock_guard<std::mutex> l(stateMutex);
                      deviceID = id; identified = identity; phoneName = identity.name; phoneModel = airplayModel; }
                    if (!opt.lan && !opt.mouseURL.empty()) {
                        if (inputLease.acquire("input:" + opt.mouseURL)) mouseInput.start(opt.mouseURL);
                        else log("Input helper already assigned to another worker; this device is video-only");
                    }
                    active = true; dirty = true;
                    identityGate.log("controlfloor-admitted", {{"udid", id}, {"identity", identity.json()}});
                }
            }
        }
        {
            J next = inputState();
            std::lock_guard<std::mutex> l(stateMutex);
            for (auto key : {"available", "keyboardAvailable", "consumerControlsAvailable", "enabled", "logicalWidthPortrait",
                             "logicalHeightPortrait", "error"})
                if (next.value(key, J()) != mouseState.value(key, J()))
                    dirty = true;
            mouseState = std::move(next);
        }
        if (active && online && steadyMillis() - lastServerMessage.load() >= 60000) {
            log("No ControlFloor command/heartbeat for 60 seconds; reconnecting provider");
            identityGate.log("provider-heartbeat-timeout", {{"timeoutSeconds", 60}});
            online = false;
            cfSocket.stop();
            resetInput();
            retry = now;
        }
        if (active && !online && now >= retry) {
            cfSocket.stop();
            forwarding = false;
            screenshotDemand = false;
            screenshot = nullptr;
            invalidate(true);
            {
                std::lock_guard<std::mutex> l(commandMutex);
                commands.clear();
            }
            ++attempt;
            identityGate.log("provider-connect-attempt", {{"attempt", attempt}, {"udid", deviceID},
                {"retryIntervalSeconds", 10}});
            log(("ControlFloor connection attempt " + std::to_string(attempt) +
                 "; failures retry after 10 seconds (OCR identity retained)").c_str());
            try {
                std::string cookie = login(credentials.at("username"), credentials.at("password"));
                if (!running)
                    break;
                if (!active || revision != connectionRevision) continue;
                std::string base = opt.server;
                base.replace(0, 4, "ws");
                cfSocket.setUrl(base + "/provider/ws?udid=" + deviceID);
                cfSocket.setExtraHeaders({{"Cookie", cookie}});
                cfSocket.setHandshakeTimeout(5);
                cfSocket.start();
            } catch (const std::exception &e) {
                log(e.what());
                identityGate.log("provider-connect-failed", {{"attempt", attempt}, {"error", e.what()},
                    {"retryAfterSeconds", 10}});
            }
            retry = Clock::now() + std::chrono::seconds(10);
        }
        if (!online || !active || revision != connectionRevision) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (observedEpoch != epoch) {
            observedEpoch = epoch;
            flights.clear();
            lastAck = frameID;
            lastProbe = Clock::now();
        }
        // Bound the WebSocket library's own output buffer too. Control replies and media
        // have one writer, so nothing can interleave a JPEG header and its binary payload.
        bool blocked = cfSocket.bufferedAmount() > 0;
        if (blocked && !wasBlocked)
            blockedAt = now;
        wasBlocked = blocked;
        if (cfSocket.bufferedAmount() > 18 * 1024 * 1024 ||
            (blocked && now - blockedAt > std::chrono::seconds(5))) {
            online = false;
            cfSocket.stop();
            retry = now + std::chrono::seconds(1);
            continue;
        }
        if (blocked) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (dirty.exchange(false)) {
            send(notice("DeviceInformation", info()));
            send(notice("DeviceStatus", status()));
            J inputNotice = notice("CFAgentCapabilities", capabilities());
            inputNotice["tag"] = {{"silent", 1}};
            send(inputNotice);
            send(notice("InterfaceOrientation", status()["interfaceOrientation"]));
        }
        controlfloor::MouseInput::Result mouseResult;
        while (mouseInput.take(mouseResult))
            send(reply(mouseResult.request, mouseResult.value, mouseResult.error));
        J request;
        {
            std::lock_guard<std::mutex> l(commandMutex);
            if (!commands.empty()) {
                request = std::move(commands.front());
                commands.pop_front();
            }
        }
        if (!request.is_null()) {
            try {
                std::string action = request.value("action", std::string());
                J args = request.value("args", J::object());
                if (args.is_null())
                    args = J::object();
                if (controlfloor::MouseInput::handles(action) || (opt.lan &&
                    controlfloor::isShortcutRequest(request))) {
                    std::lock_guard<std::mutex> session(lifecycleMutex);
                    if (!active || revision != connectionRevision)
                        throw std::runtime_error("No identified AirPlay device connected");
                    if (!args.is_object())
                        throw std::runtime_error("Input args must be an object");
                    {
                        std::lock_guard<std::mutex> l(stateMutex);
                        args["orientation"] = nativeWidth > nativeHeight ? "landscapeLeft" : "portrait";
                        args["modelName"] = identified.modelName;
                    }
                    if (opt.lan) {
                        J message={{"type","input"},{"request",request},{"args",args}};
                        if (message.dump().size()>3500) throw std::runtime_error("Input request exceeds the LAN worker message limit");
                        pairingSend(message);
                    }
                    else mouseInput.submit(request, args);
                } else if (action == "getScreenshot") {
                    if (!active)
                        throw std::runtime_error("No AirPlay video connected");
                    if (!screenshot.is_null())
                        throw std::runtime_error("A screenshot request is already pending");
                    if (!args.empty())
                        throw std::runtime_error(
                            "Set screenshot sizing/quality with setVideoStreamConfiguration first");
                    screenshot = request;
                    screenshotDemand = true;
                    screenshotDeadline = now + std::chrono::seconds(3);
                } else if (action == "ping")
                    send(reply(request, "pong"));
                else if (action == "getDeviceInformation")
                    send(reply(request, info()));
                else if (action == "getDeviceStatus" || action == "getStatus")
                    send(reply(request, status()));
                else if (action == "getInterfaceOrientation" || action == "getDeviceOrientation")
                    send(reply(request, status()["interfaceOrientation"]));
                else if (action == "getVideoStreamConfiguration")
                    send(reply(request, snapshot()));
                else if (action == "setVideoStreamConfiguration" || action == "startVideo") {
                    {
                        std::lock_guard<std::mutex> l(stateMutex);
                        J next = settings;
                        mergeSettings(next, args);
                        settings = std::move(next);
                    }
                    invalidate(true);
                    flights.clear();
                    budget = 0;
                    budgetAt = now;
                    if (action == "startVideo")
                        forwarding = true;
                    if (action == "setVideoStreamConfiguration") {
                        J r = reply(request, snapshot());
                        r["type"] = "VideoStreamConfiguration";
                        send(r);
                    } else
                        send(reply(request));
                    dirty = true;
                } else if (action == "resetVideoStreamConfiguration") {
                    {
                        std::lock_guard<std::mutex> l(stateMutex);
                        settings = defaults();
                    }
                    invalidate(true);
                    flights.clear();
                    J r = reply(request, snapshot());
                    r["type"] = "VideoStreamConfiguration";
                    send(r);
                } else if (action == "stopVideo") {
                    forwarding = false;
                    invalidate(true);
                    flights.clear();
                    send(reply(request));
                } else if (action == "startAudio" || action == "setAudioStreamConfiguration") {
                    audioStream.configure(args,action=="startAudio");
                    send(reply(request));
                } else if (action == "stopAudio") {
                    audioStream.stop(); send(reply(request));
                } else if (action == "getAudioStreamConfiguration")
                    send(reply(request,audioStream.configuration()));
                else if (action == "getVideoStreamStatus")
                    send(reply(request, {{"paused", 0},
                                         {"audioEnabled", audioStream.enabled() ? 1 : 0},
                                         {"enabled", forwarding ? 1 : 0},
                                         {"streaming", bool(forwarding && active)},
                                         {"videoEnabled", bool(forwarding)},
                                         {"source", "UxPlay"},
                                         {"connected", bool(active)}}));
                else if (action == "getVideoTransportDebug")
                    send(reply(request, {{"source", "UxPlay"},
                                         {"sourceGeneration", currentGeneration()},
                                         {"framesEncoded", encoded.load()},
                                         {"encoder", encoderDiagnostics()},
                                         {"audio", audioStream.diagnostics()},
                                         {"framesSent", sent.load()},
                                         {"framesDropped", dropped.load()},
                                         {"inFlightFrames", flights.size()},
                                         {"lastAckedFrameId", lastAck},
                                         {"queuedWebSocketBytes", cfSocket.bufferedAmount()},
                                         {"algorithm", "bounded-window-with-probe"},
                                         {"configuration", snapshot()}}));
                else if (action == "resetVideoTransport") {
                    invalidate(true);
                    flights.clear();
                    lastAck = frameID;
                    send(reply(request));
                } else if (action == "ackVideoFrame") {
                    if (!args.is_object())
                        throw std::runtime_error("ACK args must be an object");
                    std::string source = args.value("source", std::string()),
                                gen = args.value("sourceGeneration", std::string());
                    J summary = args.value("ackSummary", J::object());
                    int id = args.value("frameId", 0);
                    if (!id && summary.is_object())
                        id = summary.value("lastFrameId", 0);
                    if ((source.empty() || source == "UxPlay") &&
                        (gen.empty() || gen == currentGeneration()) && id > lastAck && id <= frameID) {
                        lastAck = id;
                        for (auto it = flights.begin(); it != flights.end() && it->first <= id;)
                            it = flights.erase(it);
                    }
                    if (request.contains("tag") || request.value("ack", J(0)) == 1)
                        send(reply(request));
                } else if (action == "ack")
                    send(reply(request));
                else if (action == "getCFAgentCapabilities")
                    send(reply(request, capabilities()));
                else
                    send(reply(request, nullptr, "Unsupported by UxPlay provider: " + action));
            } catch (const std::exception &e) {
                if (request.contains("action") && request["action"].is_string() &&
                    controlfloor::MouseInput::handles(request["action"]) &&
                    !controlfloor::MouseInput::handlesKeyboard(request["action"]) &&
                    !controlfloor::MouseInput::handlesCatalog(request["action"]))
                    resetInput();
                send(reply(request, nullptr, e.what()));
            }
        }
        if (!screenshot.is_null() && now > screenshotDeadline) {
            send(reply(screenshot, nullptr, "Timed out waiting for a decoded AirPlay frame"));
            screenshot = nullptr;
            screenshotDemand = false;
        }
        // Audio uses the same sole writer as JPEGs: header/payload pairs cannot
        // interleave. It is independent of JPEG demand and the video ACK window.
        controlfloor::AudioStream::Packet audio;
        if (active && online && audioStream.take(audio)) {
            J value={{"encoding",audio.encoding}};
            J out=notice(audio.silenceBytes ? "AudioSilence" : "AudioFrame",value);
            out["status"]="ok"; out["binaryLength"]=audio.bytes.size();
            if (audio.silenceBytes) {
                out["value"]["bytes"]=audio.silenceBytes;
                out["value"]["durationMs"]=audio.durationMs;
            }
            send(out);
            if (online && !audio.bytes.empty() && !cfSocket.sendBinary(audio.bytes).success) online=false;
            if (cfSocket.bufferedAmount()>0) continue;
        }
        if (!active || (!forwarding && !screenshotDemand) || !online) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // ACK loss/clients without ACK support get only one recovery probe per second.
        if (!screenshotDemand && flights.size() >= 2 && now - lastProbe < std::chrono::seconds(1)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        J conf = snapshot();
        double bps = conf.value("maxBytesPerSecond", 0.0);
        for (auto key : {"providerMaxBytesPerSecond", "assumeAvailableBytesPerSecond"}) {
            double v = conf.value(key, 0.0);
            if (v > 0)
                bps = bps > 0 ? std::min(bps, v) : v;
        }
        size_t nextSize = 0;
        {
            std::lock_guard<std::mutex> l(frameMutex);
            nextSize = pending.bytes.size();
        }
        budget = std::min(budget + std::chrono::duration<double>(now - budgetAt).count() * bps,
                          std::max(static_cast<double>(nextSize), bps));
        budgetAt = now;
        Frame frame;
        {
            std::lock_guard<std::mutex> l(frameMutex);
            if (!pending.bytes.empty() && (screenshotDemand || bps <= 0 || budget >= pending.bytes.size())) {
                frame = std::move(pending);
                pending = Frame();
            }
        }
        if (frame.bytes.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (frame.epoch != epoch || millis() - frame.timestamp > 1000) {
            dropped++;
            continue;
        }
        if (!screenshot.is_null()) {
            J r = reply(screenshot, {{"size", frame.bytes.size()},
                                     {"timestamp", frame.timestamp},
                                     {"width", frame.width},
                                     {"height", frame.height},
                                     {"source", "UxPlay"},
                                     {"sourceGeneration", currentGeneration()}});
            r["type"] =
                screenshot.value("action", std::string()) == "getScreenshot" ? "Screenshot" : "VideoFrame";
            r["binaryLength"] = frame.bytes.size();
            send(r);
            if (online && !cfSocket.sendBinary(frame.bytes).success)
                online = false;
            screenshot = nullptr;
            screenshotDemand = false;
            continue;
        }
        if (flights.size() >= 2)
            flights.clear();
        lastProbe = now;
        frameID++;
        flights[frameID] = now;
        if (bps > 0)
            budget -= frame.bytes.size();
        J value = {{"frameId", frameID},
                   {"timestamp", frame.timestamp},
                   {"size", frame.bytes.size()},
                   {"source", "UxPlay"},
                   {"sourceGeneration", frame.generation},
                   {"width", frame.width},
                   {"height", frame.height},
                   {"flowControlVersion", conf["flowControlVersion"]},
                   {"flowControlAlgorithm", "bounded-window-with-probe"},
                   {"adaptationPolicy", conf["adaptationPolicy"]},
                   {"jpegQuality", conf["jpegQuality"]}};
        J out = notice("VideoFrame", value);
        out["binaryLength"] = frame.bytes.size();
        out["events"] = J::array({{{"node", "UxPlay"}, {"event", "SENDING"}, {"timestamp", millis()}}});
        send(out);
        if (online && !cfSocket.sendBinary(frame.bytes).success)
            online = false;
        sent++;
    }
    online = false;
    cfSocket.stop();
    mouseInput.stop();
}
} // namespace
extern "C" int cf_enabled() {
    return opt.enabled;
}
extern "C" int cf_test_source() {
    return opt.test;
}
extern "C" void cf_help() {
    printf(
        "--controlfloor-lan            Automatic IPv4 keyboard/shortcut pairing (requires -multi)\n"
        "--controlfloor-keyboard-db F   Persistent keyboard registry (default: log/keyboards.sqlite3)\n"
        "--controlfloor-shortcut-port N LAN HTTP port (default: 9876)\n"
        "--controlfloor-shortcut-file F Serve signed ControlFloorLAN.shortcut\n"
        "--controlfloor-provider       OCR-identify AirPlay clients, then expose them to ControlFloor\n"
        "--controlfloor-log-dir DIR    Per-connection JSONL logs (default: log)\n"
        "--controlfloor-ocr-interval N  Seconds between OCR scans (default: 5)\n"
        "--controlfloor-register       Register provider, save credentials, then exit\n"
        "--controlfloor-server URL     ControlFloor http(s)://host:port\n"
        "--controlfloor-config FILE    Private provider credentials JSON\n"
        "--controlfloor-insecure       Disable server TLS verification (test only)\n"
        "--controlfloor-test-source    Use generated video instead of a phone\n"
        "--controlfloor-mouse          Use sole iphone-use Bluetooth phone for mouse/text (localhost:7390)\n"
        "--controlfloor-mouse-url URL  Override iphone-use HTTP endpoint and enable mouse\n");
}
extern "C" int cf_option(int argc, char **argv, int *index) {
    std::string a = argv[*index];
    if (a.compare(0, 15, "--controlfloor-") != 0)
        return 0;
    if (a == "--controlfloor-lan") {
        opt.enabled = opt.lan = true;
        if (opt.mouseURL.empty()) opt.mouseURL = "http://127.0.0.1:7390";
    } else if (a == "--controlfloor-provider")
        opt.enabled = true;
    else if (a == "--controlfloor-mouse")
        opt.mouseURL = "http://127.0.0.1:7390";
    else if (a == "--controlfloor-register") {
        opt.enabled = true;
        opt.registration = true;
    } else if (a == "--controlfloor-insecure")
        opt.insecure = true;
    else if (a == "--controlfloor-test-source") {
        opt.enabled = true;
        opt.test = true;
    } else if (a == "--controlfloor-server" || a == "--controlfloor-config" ||
               a == "--controlfloor-mouse-url" || a == "--controlfloor-log-dir" ||
               a == "--controlfloor-ocr-interval" || a == "--controlfloor-shortcut-port" ||
               a == "--controlfloor-shortcut-file" || a == "--controlfloor-keyboard-db") {
        if (*index + 1 >= argc) {
            log("missing ControlFloor option value");
            exit(1);
        }
        std::string value = argv[++*index];
        if (a == "--controlfloor-keyboard-db") opt.pairing.database = value;
        else if (a == "--controlfloor-shortcut-file") opt.pairing.artifact = value;
        else if (a == "--controlfloor-shortcut-port") {
            char *end = nullptr; long port = strtol(value.c_str(), &end, 10);
            if (!end || *end || port < 1 || port > 65535) { log("Invalid LAN shortcut port"); exit(1); }
            opt.pairing.port = static_cast<unsigned short>(port);
        } else if (a == "--controlfloor-server")
            opt.server = value;
        else if (a == "--controlfloor-mouse-url")
            opt.mouseURL = value;
        else if (a == "--controlfloor-log-dir")
            opt.logDirectory = value;
        else if (a == "--controlfloor-ocr-interval") {
            char *end = nullptr;
            double interval = strtod(value.c_str(), &end);
            if (!end || *end || !std::isfinite(interval) || interval < .2 || interval > 60) {
                log("OCR interval must be a number from 0.2 to 60 seconds"); exit(1);
            }
            opt.ocrInterval = interval;
        } else
            opt.config = value;
    } else {
        log("unknown ControlFloor option");
        exit(1);
    }
    return 1;
}
extern "C" int cf_prepare() {
    if (!opt.enabled) return 0;
    try {
        ix::initNetSystem();
        if (!configure()) return 1;
        if (!opt.test) { controlfloor::ScreenReader probe; }
        prepared = true;
        return 0;
    } catch (const std::exception &e) { log(e.what()); return -1; }
}
extern "C" int cf_start() {
    if (!opt.enabled)
        return 0;
    std::lock_guard<std::mutex> shutdown(shutdownMutex);
    // The receiver worker exits directly on parent-pipe EOF/STOP. Register
    // before creating threads so normal process exit joins them before static
    // std::thread destructors run. Serialize concurrent cleanup/startup too.
    std::call_once(exitCleanup, [] { std::atexit(cf_stop); });
    try {
        ix::initNetSystem();
        if (!prepared && !configure())
            return 1;
        settings = defaults();
        generation = newGeneration();
        identityGate.start(opt.logDirectory, opt.ocrInterval, [] {
            controlfloor::ScreenSample frame;
            std::lock_guard<std::mutex> l(frameMutex);
            if (latest) frame.sample = gst_sample_ref(latest);
            frame.connection = sampleConnection; frame.sequence = sampleSequence;
            return frame;
        }, opt.lan);
        running = true;
        encodeThread = std::thread([] {
            try {
                encoder();
            } catch (const std::exception &e) {
                log(e.what());
                running = false;
            }
        });
        networkThread = std::thread([] {
            try {
                network();
            } catch (const std::exception &e) {
                log(e.what());
                online = false;
                cfSocket.stop();
                running = false;
            }
        });
        return 0;
    } catch (const std::exception &e) {
        log(e.what());
        return -1;
    }
}
extern "C" void cf_stop() {
    std::lock_guard<std::mutex> shutdown(shutdownMutex);
    running = false;
    audioStream.stop();
    identityGate.stop();
    if (encodeThread.joinable())
        encodeThread.join();
    if (networkThread.joinable())
        networkThread.join();
    mouseInput.stop();
    clearFrames();
}
extern "C" void cf_connection_open() {
    if (!running) return;
    try { identityGate.opened(); }
    catch (const std::exception &e) { log(e.what()); }
}
extern "C" void cf_client(const char *id, const char *name, const char *model) {
    if (!running) return;
    std::lock_guard<std::mutex> session(lifecycleMutex);
    active = false; connected = false;
    audioStream.stop();
    ++connectionRevision;
    resetInput();
    invalidate();
    {
        std::lock_guard<std::mutex> l(stateMutex);
        airplayModel = (model && *model) ? model : "iPhone";
        nativeWidth = nativeHeight = 0;
        generation = newGeneration();
    }
    try {
        identityGate.begin(connectionRevision, id ? id : "unknown", name ? name : "", model ? model : "");
        connected = true;
        dirty = true;
    } catch (const std::exception &e) { log(e.what()); }
}
extern "C" void cf_disconnected() {
    if (!running) return;
    std::lock_guard<std::mutex> session(lifecycleMutex);
    connected = false; active = false; ++connectionRevision;
    audioStream.stop();
    resetInput(); invalidate(); identityGate.end(); dirty = true;
}
extern "C" void cf_attach(GstElement *pipeline) {
    if (!opt.enabled)
        return;
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "cf_capture");
    if (!sink)
        return;
    GstAppSinkCallbacks cb = {};
    cb.new_sample = capture;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, nullptr, nullptr);
    gst_object_unref(sink);
}

extern "C" void cf_audio_flush() { audioStream.flush(); }
extern "C" void cf_attach_audio(GstElement *pipeline) {
    if (!opt.enabled) return;
    GstElement *sink=gst_bin_get_by_name(GST_BIN(pipeline),"cf_audio_capture");
    if (!sink) return;
    GstAppSinkCallbacks callbacks={}; callbacks.new_sample=captureAudio;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink),&callbacks,nullptr,nullptr);
    gst_object_unref(sink); audioStream.available=true; dirty=true;
}

bool cf_lan_enabled() { return opt.lan; }
bool cf_lan_start() {
    if (!opt.lan) return true;
    try {
        opt.pairing.helper = opt.mouseURL; opt.pairing.directory = opt.logDirectory;
        coordinator.reset(new controlfloor::PairingCoordinator(opt.pairing));
        coordinator->start(); return true;
    } catch (const std::exception &e) { log(e.what()); coordinator.reset(); return false; }
}
void cf_lan_stop() { if (coordinator) coordinator->stop(); coordinator.reset(); }
controlfloor::PairingCoordinator *cf_lan_coordinator() { return coordinator.get(); }
void cf_lan_worker_transport(void (*send)(const char *)) { pairingTransport = send; }
void cf_lan_worker_message(const std::string &text) {
    if (!opt.lan || text.size() > 4096) return;
    J message = J::parse(text,nullptr,false);
    if (!message.is_object()) return;
    std::lock_guard<std::mutex> lock(pairingMutex);
    if (pairingMessages.size() < 64) pairingMessages.push_back(std::move(message));
}
