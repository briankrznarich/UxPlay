// SPDX-License-Identifier: GPL-3.0-or-later
#include "identity.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <gst/video/video.h>
#include <allheaders.h>
#include <mutex>
#include <map>
#include <cmath>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <tesseract/baseapi.h>
#include <thread>

namespace controlfloor {
using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
J Identity::json() const {
    return {{"keyboardId", keyboard}, {"name", name}, {"modelName", modelName},
            {"modelNumber", modelNumber}, {"serialNumber", serial}, {"osVersion", osVersion}};
}
namespace {
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string safeName(const std::string &s) {
    std::string out;
    for (unsigned char c : s.substr(0, 120))
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-') ? c : '_';
    return out.empty() ? "unknown" : out;
}
// Tesseract can recognize table columns in separate blocks. Recover display
// rows from word coordinates instead of depending on its paragraph order.
std::vector<std::string> rows(const OCRResult &ocr) {
    struct Row { double center; int height; std::vector<OCRWord> words; };
    std::vector<Row> lines;
    auto words = ocr.words;
    std::sort(words.begin(), words.end(), [](const OCRWord &a, const OCRWord &b) {
        return a.top + a.bottom < b.top + b.bottom;
    });
    for (const auto &w : words) {
        double y = (w.top + w.bottom) / 2.0;
        if (lines.empty() || std::abs(lines.back().center - y) >
                            std::max(6.0, std::min(lines.back().height, w.bottom - w.top) * .65))
            lines.push_back({y, w.bottom - w.top, {}});
        lines.back().words.push_back(w);
    }
    std::vector<std::string> result;
    for (auto &line : lines) {
        std::sort(line.words.begin(), line.words.end(), [](const OCRWord &a, const OCRWord &b) {
            return a.left < b.left;
        });
        std::string text;
        for (const auto &word : line.words) text += (text.empty() ? "" : " ") + word.text;
        result.push_back(trim(text));
    }
    return result;
}
} // namespace
struct ScreenReader::Impl {
    tesseract::TessBaseAPI api;
    Impl() {
        if (api.Init(nullptr, "eng"))
            throw std::runtime_error("Cannot initialize Tesseract English data; install tesseract-ocr-eng or check TESSDATA_PREFIX");
        api.SetPageSegMode(tesseract::PSM_SPARSE_TEXT);
    }
    OCRResult read(Pix *source) {
        if (!source) throw std::runtime_error("Cannot read OCR image");
        // Upscale small mirrored screens; cap work on large/4K displays.
        float scale = std::min(3.0f, std::min(1000.0f / pixGetWidth(source),
                                            2400.0f / pixGetHeight(source)));
        Pix *image = pixScale(source, scale, scale);
        if (!image) throw std::runtime_error("Cannot scale OCR image");
        api.SetImage(image);
        api.SetSourceResolution(150);
        if (api.Recognize(nullptr)) {
            api.Clear(); pixDestroy(&image);
            throw std::runtime_error("Tesseract recognition failed");
        }
        OCRResult result;
        char *text = api.GetUTF8Text();
        if (text) { result.text = text; delete[] text; }
        std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
        if (it) do {
            char *word = it->GetUTF8Text(tesseract::RIL_WORD);
            OCRWord w;
            if (word && it->BoundingBox(tesseract::RIL_WORD, &w.left, &w.top, &w.right, &w.bottom)) {
                w.text = word;
                if (!w.text.empty()) result.words.push_back(w);
            }
            delete[] word;
        } while (it->Next(tesseract::RIL_WORD));
        api.Clear();
        pixDestroy(&image);
        return result;
    }
};
ScreenReader::ScreenReader() : impl(new Impl) {}
ScreenReader::~ScreenReader() = default;
OCRResult ScreenReader::readFile(const std::string &path) {
    Pix *image = pixRead(path.c_str());
    if (!image) throw std::runtime_error("Cannot open image: " + path);
    try { auto result = impl->read(image); pixDestroy(&image); return result; }
    catch (...) { pixDestroy(&image); throw; }
}
OCRResult ScreenReader::read(GstSample *sample) {
    GstVideoInfo input, output;
    gst_video_info_init(&input); gst_video_info_init(&output);
    if (!gst_video_info_from_caps(&input, gst_sample_get_caps(sample)))
        throw std::runtime_error("OCR requires decoded video caps");
    gst_video_info_set_format(&output, GST_VIDEO_FORMAT_RGB, input.width, input.height);
    output.fps_n = input.fps_n; output.fps_d = input.fps_d;
    output.par_n = input.par_n; output.par_d = input.par_d;
    GstVideoConverter *converter = gst_video_converter_new(&input, &output, nullptr);
    if (!converter) throw std::runtime_error("Cannot create OCR RGB converter");
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, output.size, nullptr);
    GstVideoFrame inFrame = {}, outFrame = {};
    bool inMapped = gst_video_frame_map(&inFrame, &input, gst_sample_get_buffer(sample), GST_MAP_READ);
    bool outMapped = buffer && gst_video_frame_map(&outFrame, &output, buffer, GST_MAP_WRITE);
    Pix *image = nullptr;
    if (inMapped && outMapped) {
        gst_video_converter_frame(converter, &inFrame, &outFrame);
        image = pixCreate(input.width, input.height, 32);
        if (image) {
            auto *data = static_cast<const unsigned char *>(GST_VIDEO_FRAME_PLANE_DATA(&outFrame, 0));
            int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&outFrame, 0);
            auto *dest = pixGetData(image);
            int words = pixGetWpl(image);
            for (int y = 0; y < input.height; ++y)
                for (int x = 0; x < input.width; ++x) {
                    const auto *p = data + y * stride + x * 3;
                    dest[y * words + x] = (l_uint32(p[0]) << 24) | (l_uint32(p[1]) << 16) | (l_uint32(p[2]) << 8);
                }
        }
    }
    if (outMapped) gst_video_frame_unmap(&outFrame);
    if (inMapped) gst_video_frame_unmap(&inFrame);
    if (buffer) gst_buffer_unref(buffer);
    gst_video_converter_free(converter);
    if (!image) throw std::runtime_error("Cannot map decoded frame for OCR");
    try { auto result = impl->read(image); pixDestroy(&image); return result; }
    catch (...) { pixDestroy(&image); throw; }
}
std::string keyboardID(const OCRResult &ocr) {
    static const std::regex marker("X\\s*=\\s*([A-Za-z0-9][A-Za-z0-9:_-]{0,127})\\s*=\\s*X", std::regex::icase);
    std::string found;
    // Work by row so a wrapped or incomplete marker cannot consume other UI text.
    for (const auto &line : rows(ocr)) {
        for (std::sregex_iterator it(line.begin(), line.end(), marker), end; it != end; ++it) {
            auto id = (*it)[1].str();
            if (!found.empty() && found != id) return ""; // Ambiguous screen.
            found = id;
        }
    }
    return found;
}
Identity aboutIdentity(const OCRResult &ocr) {
    Identity identity;
    static const std::regex field("^\\s*(Serial Number|Model Number|Model Name|iOS Version|Name)\\s+(.+?)\\s*$", std::regex::icase);
    std::map<std::string, std::string> fields;
    for (const auto &line : rows(ocr)) {
        std::smatch match;
        if (!std::regex_match(line, match, field)) continue;
        std::string label = match[1], value = trim(match[2]);
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::tolower(c); });
        // Chevron is an accessory, not part of the value.
        while (!value.empty() && (value.back() == '>' || value.back() == '\xBB')) value.pop_back();
        value = trim(value);
        if (fields.count(label) && fields[label] != value) return Identity();
        fields[label] = value;
    }
    identity.name = fields["name"];
    identity.modelName = fields["model name"];
    identity.modelNumber = fields["model number"];
    identity.serial = fields["serial number"];
    identity.osVersion = fields["ios version"];
    // Do not guess substitutions such as O/0 or I/1 in permanent identifiers.
    static const std::regex serial("[A-Z0-9]{8,16}");
    if (!std::regex_match(identity.serial, serial)) identity.serial.clear();
    return identity;
}
struct IdentityGate::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool running = false, managed = false, allowAbout = false;
    std::string expectedToken;
    J airplay = J::object();
    enum Stage { Idle, Keyboard, About, Ready } stage = Idle;
    unsigned long connection = 0, keyboardFrame = 0;
    std::string directory, path, suffix, candidate;
    double interval = 5;
    Snapshot snapshot;
    FILE *file = nullptr;
    ~Impl() { if (file) fclose(file); }
    void closeLog() { if (file) fclose(file); file = nullptr; }
    Identity identity;
    void log(const std::string &event, const J &detail = nullptr) {
        if (!file) return;
        auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string line = J({{"time", time}, {"event", event}, {"detail", detail}}).dump() + "\n";
        fwrite(line.data(), 1, line.size(), file);
        fflush(file);
    }
    void open() {
        if (file) return;
        gchar *uuid = g_uuid_string_random();
        suffix = std::to_string(g_get_real_time()) + "-" + uuid;
        g_free(uuid);
        path = directory + "/pending-" + suffix + ".jsonl";
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { file = fdopen(fd, "w"); if (!file) close(fd); }
        if (!file) throw std::runtime_error("Cannot create connection log: " + path);
        log("connection-open", {{"peer", g_getenv("UXPLAY_WORKER_PEER") ? g_getenv("UXPLAY_WORKER_PEER") : "unknown"}});
    }
    void run() {
        ScreenReader reader;
        std::unique_lock<std::mutex> lock(mutex);
        while (running) {
            wake.wait(lock, [&] { return !running || (stage == Keyboard && (!managed || !expectedToken.empty())) ||
                (stage == About && (!managed || allowAbout)); });
            if (!running) break;
            auto token = connection;
            auto expected = stage;
            lock.unlock();
            ScreenSample frame = snapshot();
            OCRResult result;
            std::string error;
            auto started = Clock::now();
            if (frame.sample && frame.connection == token) {
                try { result = reader.read(frame.sample); }
                catch (const std::exception &e) { error = e.what(); }
            }
            if (frame.sample) gst_sample_unref(frame.sample);
            lock.lock();
            if (!running) break;
            if (token != connection || expected != stage || (managed && expected == About && !allowAbout)) continue;
            if (!error.empty()) log("ocr-error", error);
            else if (!result.text.empty()) {
                log("ocr", {{"stage", stage == Keyboard ? "keyboard" : "about"}, {"text", result.text},
                     {"milliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()}});
                if (stage == Keyboard) {
                    auto keyboard = keyboardID(result);
                    if (!keyboard.empty() && (!managed || keyboard == expectedToken)) {
                        identity.keyboard = keyboard;
                        keyboardFrame = frame.sequence;
                        stage = About;
                        log("keyboard-identified", keyboard);
                    }
                } else if (frame.sequence > keyboardFrame) {
                    Identity next = aboutIdentity(result);
                    if (next.complete()) {
                        // Require agreement on two scans before using an OCR serial as a database key.
                        if (candidate == next.serial + "\n" + next.name + "\n" + next.modelName) {
                            next.keyboard = identity.keyboard;
                            identity = next;
                            stage = Ready;
                            log("device-identified", identity.json());
                        } else {
                            candidate = next.serial + "\n" + next.name + "\n" + next.modelName;
                            log("identity-candidate", next.json());
                        }
                    } else {
                        candidate.clear();
                        log("about-incomplete", next.json());
                    }
                }
            } else log("waiting-for-frame-or-text");
            auto currentStage = stage;
            wake.wait_for(lock, std::chrono::duration<double>(interval), [&] {
                return !running || token != connection || stage != currentStage;
            });
        }
    }
};
IdentityGate::IdentityGate() : impl(new Impl) {}
IdentityGate::~IdentityGate() { stop(); }
void IdentityGate::start(const std::string &directory, double interval, Snapshot snapshot, bool managed) {
    impl->managed = managed;
    // Fail startup immediately if the language data is unavailable.
    { ScreenReader probe; }
    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0)
        throw std::runtime_error("Cannot create ControlFloor log directory: " + directory);
    impl->directory = directory; impl->interval = interval; impl->snapshot = std::move(snapshot);
    impl->running = true;
    impl->worker = std::thread([this] {
        try { impl->run(); }
        catch (const std::exception &e) { log("ocr-fatal", e.what()); fprintf(stderr, "ControlFloor OCR: %s\n", e.what()); }
    });
}
void IdentityGate::stop() {
    { std::lock_guard<std::mutex> lock(impl->mutex); impl->running = false; }
    impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    end();
}
void IdentityGate::opened() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->open();
}
void IdentityGate::begin(unsigned long connection, const std::string &id,
                         const std::string &name, const std::string &model) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->stage != Impl::Idle) {
        impl->log("connection-replaced"); impl->closeLog();
    }
    impl->open();
    std::string named = impl->directory + "/" + safeName(id) + "-" + impl->suffix + ".jsonl";
    if (std::rename(impl->path.c_str(), named.c_str()) != 0)
        impl->log("log-rename-failed", named);
    else impl->path = named;
    fprintf(stderr, "ControlFloor connection log: %s\n", impl->path.c_str());
    impl->connection = connection; impl->stage = Impl::Keyboard;
    impl->identity = Identity(); impl->candidate.clear(); impl->keyboardFrame = 0;
    impl->expectedToken.clear(); impl->allowAbout = false;
    impl->airplay = {{"id", id}, {"name", name}, {"model", model}};
    impl->log("airplay-client", impl->airplay);
    impl->wake.notify_all();
}
void IdentityGate::end() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->stage = Impl::Idle; ++impl->connection; impl->airplay = J::object();
    impl->log("connection-closed"); impl->closeLog();
    impl->wake.notify_all();
}
bool IdentityGate::ready(unsigned long connection, Identity &identity) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->stage != Impl::Ready || impl->connection != connection) return false;
    identity = impl->identity; return true;
}
void IdentityGate::pairing(unsigned long connection, const std::string &token, bool about) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->managed || impl->connection != connection) return;
    if (impl->expectedToken != token) {
        impl->expectedToken = token;
        impl->identity = Identity(); impl->candidate.clear();
        impl->identity.keyboard = token;
        impl->stage = token.empty() ? Impl::Keyboard : Impl::About;
    }
    if (about && !impl->allowAbout) impl->candidate.clear();
    impl->allowAbout = about;
    impl->wake.notify_all();
}
nlohmann::json IdentityGate::progress(unsigned long connection) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->connection != connection) return J::object();
    return {{"identity", impl->identity.json()}, {"ready", impl->stage == Impl::Ready}, {"airplay",impl->airplay}};
}
void IdentityGate::log(const std::string &event, const J &detail) {
    std::lock_guard<std::mutex> lock(impl->mutex); impl->log(event, detail);
}
} // namespace controlfloor
