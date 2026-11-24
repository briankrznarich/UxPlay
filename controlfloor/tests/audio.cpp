// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using controlfloor::AudioStream;
using J=nlohmann::json;
#define check(value) do { if (!(value)) throw std::runtime_error("audio assertion failed at line " + std::to_string(__LINE__)); } while (false)
std::string pcm(const std::vector<int> &values) {
    std::string s;
    for (int v:values) { s+=char(v&255); s+=char((unsigned(v)>>8)&255); }
    return s;
}
int main() {
    AudioStream audio; audio.available=true; AudioStream::Packet p;
    auto feed=[&](const std::string &s) { audio.capture(s.data(),s.size()); };
    auto start=[&](const std::string &encoding,const std::string &silence="include") { audio.configure({{"encoding",encoding},{"silence",silence}},true); };
    start("PCM16-44100-2ch"); auto stereo=pcm({32767,-32768,256,-256});feed(stereo);
    check(audio.take(p) && p.bytes==stereo);
    start("PCM8-44100-2ch");feed(stereo);check(audio.take(p) && p.bytes==std::string("\x7f\x80\x01\xff",4));
    start("PCM16-22050-1ch");feed(pcm({1000,3000}));check(!audio.take(p));
    feed(pcm({3000,5000}));check(audio.take(p) && p.bytes==pcm({3000}));
    for(int bits:{8,16})for(int rate:{11025,22050,44100})for(int channels:{1,2}) {
        start("PCM"+std::to_string(bits)+"-"+std::to_string(rate)+"-"+std::to_string(channels)+"ch");
        feed(pcm(std::vector<int>(352*2,1024)));check(audio.take(p));
        check(p.bytes.size()==size_t(352/(44100/rate)*channels*bits/8));
    }
    auto old=audio.configuration();
    bool rejected=false;
    try { audio.configure({{"encoding","AAC"},{"silence","drop"}},true); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && audio.configuration()==old);
    rejected=false;
    try { audio.configure({{"encoding","PCM16-44100-1ch"},{"silence","bogus"}},true); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && audio.configuration()==old);
    audio.available=false; rejected=false;
    try { audio.configure(J::object(),true); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && audio.configuration()==old); audio.available=true;
    std::cout<<"PASS little-endian signed PCM, mono/downsampling carry, all encodings, atomic configuration\n";
    start("PCM16-44100-2ch");feed(stereo);audio.stop();check(!audio.take(p));feed(stereo);check(!audio.take(p));
    start("PCM16-44100-2ch");check(!audio.take(p));feed(stereo);audio.flush();check(!audio.take(p));
    feed(stereo);std::this_thread::sleep_for(std::chrono::milliseconds(270));check(!audio.take(p));
    std::string block=pcm(std::vector<int>(441*2,1234));
    for(int i=0;i<100;i++)feed(block);
    check(audio.diagnostics()["queuedBytes"]<=audio.diagnostics()["queueLimitBytes"]);
    check(audio.diagnostics()["droppedPackets"]>=80);
    int packets=0;while(audio.take(p))packets++;check(packets<=20);
    std::cout<<"PASS stop/restart/flush freshness, stale expiry, bounded slow-consumer queue\n";
    std::string zero(441*4,0);start("PCM16-22050-1ch","drop");
    int audible=0;for(int i=0;i<301;i++){feed(zero);while(audio.take(p))audible++;}
    check(audible==100);
    start("PCM8-11025-2ch","report");int reports=0;size_t silent=0;
    for(int i=0;i<350;i++){feed(zero);while(audio.take(p))if(p.silenceBytes){reports++;silent+=p.silenceBytes;check(p.bytes.empty());}}
    check(reports==1 && silent==11025*2*2);
    feed(block);check(audio.take(p)&&p.silenceBytes>0);check(audio.take(p)&&!p.bytes.empty());
    start("PCM16-44100-1ch","include");for(int i=0;i<200;i++){feed(zero);check(audio.take(p)&&p.silenceBytes==0&&!p.bytes.empty());}
    std::cout<<"PASS silence grace, include/drop/report and report flush on resumed sound\n";
}
