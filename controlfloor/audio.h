// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <json.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace controlfloor {
// Canonical input is decoded 44.1 kHz, stereo, signed 16-bit little-endian PCM.
// The renderer callback only copies into a bounded queue; the sole CF socket
// writer converts/takes packets. It never waits for a viewer or video ACK.
class AudioStream {
public:
    struct Packet {
        std::string bytes, encoding;
        size_t silenceBytes = 0;
        double durationMs = 0;
    };
    std::atomic<bool> available{false};
    void configure(const nlohmann::json &args, bool start);
    void stop();
    void flush();
    bool enabled() const { return on.load(); }
    nlohmann::json configuration() const;
    nlohmann::json diagnostics() const;
    void capture(const void *data, size_t size);
    bool take(Packet &packet);
private:
    using Clock = std::chrono::steady_clock;
    struct Chunk { std::string bytes; Clock::time_point at; };
    mutable std::mutex mutex;
    std::atomic<bool> on{false};
    std::atomic<unsigned long> captured{0}, dropped{0};
    std::deque<Chunk> queue;
    size_t queued = 0;
    std::string encoding = "PCM16-22050-1ch", silence = "drop";
    int rate = 22050, channels = 1, bits = 16, ratio = 2;
    int carry = 0, sumLeft = 0, sumRight = 0;
    size_t zeroFrames = 0, silenceBytes = 0;
    Packet held;
    Clock::time_point heldAt{};
    void clear(); // mutex held
};
}
