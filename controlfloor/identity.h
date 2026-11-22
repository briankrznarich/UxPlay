// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gst/gst.h>
#include <json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace controlfloor {
struct Identity {
    std::string keyboard, name, modelName, modelNumber, serial, osVersion;
    bool complete() const { return !name.empty() && !modelName.empty() && !serial.empty(); }
    nlohmann::json json() const;
};
struct OCRWord { std::string text; int left, top, right, bottom; };
struct OCRResult { std::string text; std::vector<OCRWord> words; };
// No networking or provider state: usable from an image test harness too.
class ScreenReader {
  public:
    ScreenReader();
    ~ScreenReader();
    OCRResult read(GstSample *sample);
    OCRResult readFile(const std::string &path);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::string keyboardID(const OCRResult &ocr);
Identity aboutIdentity(const OCRResult &ocr);

struct ScreenSample {
    GstSample *sample = nullptr; // Owned reference; caller releases.
    unsigned long connection = 0, sequence = 0;
};
// Each receiver worker owns one gate. OCR runs on a separate thread and holds
// only one sample reference. Results from an old connection are discarded.
class IdentityGate {
  public:
    using Snapshot = std::function<ScreenSample()>;
    IdentityGate();
    ~IdentityGate();
    void start(const std::string &directory, double interval, Snapshot snapshot);
    void stop();
    void opened();
    void begin(unsigned long connection, const std::string &id,
               const std::string &name, const std::string &model);
    void end();
    bool ready(unsigned long connection, Identity &identity);
    void log(const std::string &event, const nlohmann::json &detail = nullptr);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace controlfloor
