// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include <algorithm>
#include <stdexcept>

namespace controlfloor {
using J = nlohmann::json;
namespace {
constexpr size_t inputBytesPerSecond = 44100 * 2 * 2;
constexpr size_t queueLimit = inputBytesPerSecond / 5; // 200 ms
const auto maximumAge = std::chrono::milliseconds(250);
J encodings() {
    J values=J::array();
    for (int bits : {16,8}) for (int rate : {44100,22050,11025}) for (int channels : {1,2})
        values.push_back("PCM"+std::to_string(bits)+"-"+std::to_string(rate)+"-"+std::to_string(channels)+"ch");
    return values;
}
int sample(const unsigned char *p) {
    unsigned value=p[0] | (unsigned(p[1])<<8);
    return value>=32768 ? int(value)-65536 : int(value);
}
}
void AudioStream::clear() {
    queue.clear(); queued=0; carry=sumLeft=sumRight=0;
    zeroFrames=silenceBytes=0; held=Packet();
}
void AudioStream::flush() { std::lock_guard<std::mutex> lock(mutex); clear(); }
void AudioStream::stop() { std::lock_guard<std::mutex> lock(mutex); on=false; clear(); }
void AudioStream::configure(const J &args, bool start) {
    if (!args.is_object()) throw std::runtime_error("Audio args must be an object");
    std::lock_guard<std::mutex> lock(mutex);
    std::string nextEncoding=encoding, nextSilence=silence;
    for (auto key : {"encoding","silence"}) if (args.contains(key) && !args[key].is_string())
        throw std::runtime_error(std::string(key)+" must be a string");
    if (args.contains("encoding") && args["encoding"]!="") nextEncoding=args["encoding"];
    if (args.contains("silence") && args["silence"]!="") nextSilence=args["silence"];
    auto supported=encodings();
    if (std::find(supported.begin(),supported.end(),nextEncoding)==supported.end())
        throw std::runtime_error("Unsupported audio encoding: "+nextEncoding);
    if (nextSilence!="drop" && nextSilence!="include" && nextSilence!="report")
        throw std::runtime_error("silence must be drop, include or report");
    if (start && !available) throw std::runtime_error("AirPlay audio renderer unavailable; restart without -a or -as 0");
    // Parse only after membership/field validation. Failed changes are atomic.
    if (nextEncoding!=encoding || nextSilence!=silence || start) clear();
    encoding=nextEncoding; silence=nextSilence;
    bits=encoding.find("PCM16-")==0 ? 16 : 8;
    rate=encoding.find("-44100-")!=std::string::npos ? 44100 : encoding.find("-22050-")!=std::string::npos ? 22050 : 11025;
    channels=encoding.find("-2ch")!=std::string::npos ? 2 : 1; ratio=44100/rate;
    if (start) on=true;
}
J AudioStream::configuration() const {
    std::lock_guard<std::mutex> lock(mutex);
    return {{"encoding",encoding},{"silence",silence},{"enabled",bool(on)},
        {"supportedAudioEncodings",encodings()},{"nativeAudioEncoding","PCM-s16le-44100-2ch"}};
}
J AudioStream::diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex);
    return {{"available",bool(available)},{"enabled",bool(on)},{"capturedPackets",captured.load()},
        {"droppedPackets",dropped.load()},{"queuedBytes",queued},{"queueLimitBytes",queueLimit},
        {"maximumPacketAgeMs",250},{"encoding",encoding},{"silence",silence}};
}
void AudioStream::capture(const void *data, size_t size) {
    if (!on || !data || !size) return;
    if (size%4 || size>queueLimit) { ++dropped; return; }
    std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
    if (!lock.owns_lock()) { ++dropped; return; }
    if (!on) return;
    auto now=Clock::now();
    while (!queue.empty() && (queued+size>queueLimit || queue.size()>=32 || now-queue.front().at>maximumAge)) {
        queued-=queue.front().bytes.size(); queue.pop_front(); ++dropped;
        carry=sumLeft=sumRight=0; silenceBytes=zeroFrames=0;
    }
    queue.push_back({std::string(static_cast<const char *>(data),size),now}); queued+=size; ++captured;
}
bool AudioStream::take(Packet &out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!on) return false;
    const auto now=Clock::now();
    if (!held.bytes.empty()) {
        if (now-heldAt<=maximumAge) { out=std::move(held); held=Packet(); return true; }
        held=Packet(); ++dropped;
    }
    while (!queue.empty()) {
        Chunk chunk=std::move(queue.front()); queue.pop_front(); queued-=chunk.bytes.size();
        if (now-chunk.at>maximumAge) { ++dropped; carry=sumLeft=sumRight=0; silenceBytes=zeroFrames=0; continue; }
        Packet packet; packet.encoding=encoding;
        packet.bytes.reserve(chunk.bytes.size()/ratio*channels/2*bits/16);
        bool allZero=true;
        for (size_t i=0;i<chunk.bytes.size();i+=4) {
            const auto *p=reinterpret_cast<const unsigned char *>(chunk.bytes.data()+i);
            int l=sample(p), r=sample(p+2); allZero &= l==0 && r==0;
            sumLeft+=l; sumRight+=r;
            if (++carry<ratio) continue;
            auto append=[&](int value) {
                if (bits==8) packet.bytes.push_back(static_cast<char>(value<0 ? -((-value+255)/256) : value/256));
                else { unsigned v=static_cast<unsigned>(value)&65535; packet.bytes.push_back(char(v&255)); packet.bytes.push_back(char(v>>8)); }
            };
            if (channels==1) append((sumLeft+sumRight)/(ratio*2));
            else { append(sumLeft/ratio); append(sumRight/ratio); }
            carry=sumLeft=sumRight=0;
        }
        zeroFrames=allZero ? zeroFrames+chunk.bytes.size()/4 : 0;
        if (allZero && zeroFrames>44100 && silence!="include") {
            if (silence=="report") silenceBytes+=packet.bytes.size();
            else silenceBytes=0;
            if (silenceBytes<size_t(rate*channels*(bits/8)*2)) continue;
            packet.bytes.clear(); packet.silenceBytes=silenceBytes; silenceBytes=0;
        } else if (silenceBytes && !packet.bytes.empty()) {
            held=std::move(packet); held.durationMs=1000.0*held.bytes.size()/(rate*channels*(bits/8)); heldAt=chunk.at;
            packet=Packet(); packet.encoding=encoding; packet.silenceBytes=silenceBytes; silenceBytes=0;
        }
        if (packet.bytes.empty() && !packet.silenceBytes) continue;
        packet.durationMs=1000.0*(packet.bytes.size()+packet.silenceBytes)/(rate*channels*(bits/8));
        out=std::move(packet); return true;
    }
    return false;
}
}
