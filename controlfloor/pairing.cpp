// SPDX-License-Identifier: GPL-3.0-or-later
#include "pairing.h"
#include <cmath>
#include "mouse_input.h"
#include "keyboard_store.h"
#include "dashboard_asset.h"
#include "calibration_asset.h"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXHttpServer.h>
#include <glib.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace controlfloor {
using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
std::string uuid() { gchar *s = g_uuid_string_random(); std::string v(s); g_free(s); return v; }
std::string token() {
    // Four uniformly random 00..99 groups. Reject the incomplete byte range
    // before reducing modulo 100, so no group is more likely than another.
    std::string value;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char byte;
        do {
            if (RAND_bytes(&byte, 1) != 1) throw std::runtime_error("Cannot generate pairing code");
        } while (byte >= 200);
        unsigned group = byte % 100;
        if (i) value += '-';
        value += char('0' + group / 10);
        value += char('0' + group % 10);
    }
    return value;
}
std::string enteredToken(const J &report) {
    // Preserve leading zeroes and separators: the shortcut must return text.
    if (!report.value("pairingId", J()).is_string()) return "";
    std::string value = report["pairingId"];
    if (value.size() != 11) return "";
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 2 || i == 5 || i == 8) { if (value[i] != '-') return ""; }
        else if (value[i] < '0' || value[i] > '9') return "";
    }
    return value;
}
bool ipv4(const std::string &peer) { in_addr a; return inet_pton(AF_INET, peer.c_str(), &a) == 1; }
J http(const std::string &url) {
    ix::HttpClient client; auto request = client.createRequest();
    request->connectTimeout = 1; request->transferTimeout = 2; request->followRedirects = false;
    auto response = client.get(url, request);
    if (response->statusCode != 200 || response->body.size() > 1024*1024)
        throw std::runtime_error("iphone-use inventory HTTP " + std::to_string(response->statusCode));
    J value = J::parse(response->body);
    if (!value.is_object() || value.contains("error")) throw std::runtime_error("Invalid helper inventory");
    return value;
}
ix::HttpResponsePtr response(const J &value, int status = 200) {
    return std::make_shared<ix::HttpResponse>(status, status == 200 ? "OK" : "Error", ix::HttpErrorCode::Ok,
        ix::WebSocketHttpHeaders{{"Content-Type","application/json"},{"Cache-Control","no-store"},{"Connection","close"}}, value.dump());
}
std::string safe(std::string value) {
    for (char &c : value) if (!g_ascii_isalnum(c) && c != '-' && c != '_') c = '_';
    return value.substr(0,128);
}
FILE *appendLog(const std::string &path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    FILE *file = fd >= 0 ? fdopen(fd, "a") : nullptr;
    if (!file && fd >= 0) close(fd);
    return file;
}
void appendEvent(FILE *file, const J &event) {
    if (!file) return;
    std::string line = event.dump(-1, ' ', false, J::error_handler_t::replace) + "\n";
    // Also serialize appenders in separate coordinators using this directory.
    if (flock(fileno(file), LOCK_EX)) {
        fprintf(stderr, "ControlFloor: cannot lock diagnostic log\n"); return;
    }
    bool written = fwrite(line.data(), 1, line.size(), file) == line.size();
    if (fflush(file) || !written) fprintf(stderr, "ControlFloor: cannot write diagnostic log\n");
    flock(fileno(file), LOCK_UN);
}
void logBody(J &record, const std::string &body) {
    record["bodyBytes"] = body.size();
    if (g_utf8_validate(body.data(), body.size(), nullptr)) {
        record["body"] = body; record["bodyEncoding"] = "utf-8";
    } else {
        gchar *encoded = g_base64_encode(reinterpret_cast<const guchar *>(body.data()), body.size());
        record["body"] = encoded; record["bodyEncoding"] = "base64";
        g_free(encoded);
    }
}
const std::map<std::string,std::string> aliases = {
    {"ping","ping"}, {"getShortcutScriptVersion","version"}, {"launchApplication","launch-app"},
    {"openURL","open-url"}, {"getClipboardText","clipboard-get"}, {"setClipboardText","clipboard-set"},
    {"getDeviceName","device-info"}, {"getDeviceInformation","device-info"},
    {"enableWiFi","wifi-on"}, {"disableWiFi","wifi-off"}, {"enableBluetooth","bluetooth-on"},
    {"disableBluetooth","bluetooth-off"}, {"enableCellularData","cellular-data-on"},
    {"disableCellularData","cellular-data-off"}, {"enableAirplaneMode","airplane-mode-on"},
    {"disableAirplaneMode","airplane-mode-off"}, {"getBatteryStatus","battery-state"},
    {"getNetworkState","network-state"}, {"getIOSMajorVersion","ios-major-version"},
    {"prefsGeneralAbout","prefs-general-about"}, {"setBrightness","brightness-set"},
    {"setVolume","volume-set"}, {"restartNetwork","restart-network"},
    {"startMediaDownload","download-media"},
    {"enableFlashlight","flashlight-on"}, {"disableFlashlight","flashlight-off"}
};
J shortcutInput(const J &request) {
    J input = request.value("args", J::object());
    if (!input.is_object()) throw std::runtime_error("Shortcut args must be an object");
    std::string action = request.at("action"), command;
    if (action == "runShortcut") command = input.value("command", "");
    else { auto entry = aliases.find(action); command = entry == aliases.end() ? action : entry->second; }
    if (command == "hello") command = "ping";
    if (action == "setBrightness" || action == "setVolume") {
        J percent = input.value("percent",J());
        if (!percent.is_number() || percent < 0 || percent > 100)
            throw std::runtime_error("percent must be a number from 0 to 100");
        input["value"] = percent; input.erase("percent");
    }
    if (action == "startMediaDownload") {
        std::string destination = input.value("destination","CAMERA_ROLL");
        if (destination == "SHARE") command = "download-share";
        else if (destination != "CAMERA_ROLL") throw std::runtime_error("Unsupported shortcut download destination");
        input.erase("destination");
    }
    if (command == "restart-network" && !input.contains("waitSeconds")) input["waitSeconds"] = 20;
    if (command == "initiate-pairing" || command == "display-pairing-id" || command.empty())
        throw std::runtime_error("Pairing commands are reserved for the coordinator");
    // The installed shortcut validates its own action/parameters and reports errors.
    // Preserve action-specific arguments, but generate all routing fields ourselves.
    input.erase("requestId"); input.erase("queuedAt"); input.erase("sessionId"); input["command"] = command;
    return input;
}
}
bool isShortcutRequest(const J &request) {
    static const std::set<std::string> dedicated = {"runShortcut","getShortcutScriptVersion","setBrightness","setVolume",
        "getBatteryStatus","getNetworkState","restartNetwork","prefsGeneralAbout","getIOSMajorVersion"};
    return dedicated.count(request.value("action", "")) || request.value("agent", "") == "Shortcuts";
}
struct PairingCoordinator::Impl {
    struct Work { std::string operation; J request; };
    struct Keyboard {
        std::string host, token, ip, worker, phase = "idle", requestId, command, error;
        unsigned long connection = 0;
        bool autoConnect = true, manualHold = false, continueAfterPair = true, adapterStarted = false;
        std::string lastIP, label, pendingManual, operation, expectedSerial;
        J lastIdentity = J::object(), history = J::array();
        J calibration = J::object();
        std::string calibrationReturnPhase;
        double calibrationContact = 0;
        int64_t lastSeen = 0;
        double savedAt = 0;
        bool replayRequested = false, calibrationForAdmission = false;
        bool recoveryNext = false;
        bool present = false, initialReport = false, fetched = false, reported = false, replayDone = false;
        double due = 0, deadline = 0, started = 0, armDeadline = 0;
        unsigned long replayProbe = 0;
        J metadata, identity, commandInput, remoteShortcut, remoteReport;
        std::map<std::string,Work> work;
        MouseInput input;
        int lease = -1;
        FILE *log = nullptr;
        ~Keyboard() { input.stop(); if (lease >= 0) close(lease); if (log) fclose(log); }
    };
    struct Worker { std::string peer; unsigned long connection = 0; bool connected = false; int64_t connectedAt = 0; J progress; };
    struct Bootstrap { std::string requestId; double at = 0; bool fetched = true; };
    PairingOptions opt;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> running{false};
    std::thread thread;
    std::unique_ptr<ix::HttpServer> server;
    std::map<std::string,std::unique_ptr<Keyboard>> keyboards;
    std::map<std::string,Worker> workers;
    std::map<std::string,Bootstrap> bootstrap;
    std::map<std::string,std::string> shortcutAddresses; // peer -> accepted local destination
    std::map<std::string,std::pair<std::string,double>> receipts; // requestId -> peer/expiry
    std::deque<PairingOutput> outputs;
    std::set<std::string> retiredWorkers;
    std::string artifact, mutationToken = uuid(), inventoryError;
    int64_t inventoryAt = 0;
    // Coordinator-wide, monotonic and ephemeral. Only serving an actual pairing
    // prompt to an unbound peer arms typing; inventory/UI/paired requests do not.
    double lastPairingPromptAt = -1;
    KeyboardStore store;
    std::string source() const { return "iphone-use:" + opt.helper; }
    void save(Keyboard &k) {
        store.save(source(), {{"host",k.host},{"autoConnect",k.autoConnect},{"label",k.label},
            {"lastIP",k.lastIP},{"identity",k.lastIdentity},{"lastSeen",k.lastSeen},
            {"model",k.metadata.value("model","")},{"iosVersion",k.metadata.value("iosVersion","")}});
        k.savedAt=now();
    }
    std::string freshToken() const {
        for (;;) {
            auto candidate = token();
            bool used = false;
            for (const auto &entry : keyboards)
                if (entry.second->token == candidate) { used = true; break; }
            if (!used) return candidate;
        }
    }
    int helperLease = -1;
    FILE *httpLog = nullptr;
    std::mutex httpLogMutex;
    ~Impl() { if (helperLease >= 0) close(helperLease); if (httpLog) fclose(httpLog); }
    void logHttp(J record) {
        record["time"] = g_get_real_time()/1000;
        std::lock_guard<std::mutex> lock(httpLogMutex);
        appendEvent(httpLog, record);
    }
    std::string keyboardLogPath(const Keyboard &k) const {
        return opt.directory+"/keyboard-"+safe(k.host)+".jsonl";
    }
    explicit Impl(const PairingOptions &o) : opt(o) {}
    void log(Keyboard &k, const std::string &event, const J &detail = nullptr) {
        k.history.push_back({{"time",g_get_real_time()/1000},{"event",event},{"detail",detail}});
        if (k.history.size()>20) k.history.erase(k.history.begin());
        if (!k.log) {
            k.log = appendLog(keyboardLogPath(k));
            if (!k.log) fprintf(stderr, "ControlFloor: cannot open keyboard log for %s\n", k.host.c_str());
        }
        appendEvent(k.log, {{"time",g_get_real_time()/1000}, {"event",event}, {"keyboardId",k.host},
            {"pairingId",k.token}, {"ip",k.ip}, {"phase",k.phase}, {"detail",detail}});
        if (event != "shortcut-progress") fprintf(stderr,"ControlFloor pairing [%s] %s: %s\n",k.host.c_str(),k.phase.c_str(),event.c_str());
    }
    void output(const std::string &worker, J message, bool disconnect = false) {
        if (worker.empty()) return;
        if (disconnect) retiredWorkers.insert(worker);
        // Never let snapshot coalescing move a new binding ahead of its revoke.
        if (disconnect || message.value("type","")=="revoke")
            outputs.erase(std::remove_if(outputs.begin(),outputs.end(),[&](const PairingOutput &item) {
                return item.worker==worker && item.message.value("type","")=="binding";
            }),outputs.end());
        // Replace unsent state snapshots. Input replies are ordered and bounded.
        if (message.value("type", "") == "binding")
            for (auto &item : outputs) if (item.worker == worker && item.message.value("type", "") == "binding") {
                item.message = std::move(message); return;
            }
        if (outputs.size() >= 4096) throw std::runtime_error("Pairing output queue is full");
        PairingOutput item; item.worker=worker; item.message=std::move(message); item.disconnect=disconnect;
        outputs.push_back(std::move(item));
    }
    void state(Keyboard &k) {
        if (k.worker.empty()) return;
        output(k.worker, {{"type","binding"}, {"connection",k.connection}, {"token",k.token},
            {"keyboard",k.host}, {"about",k.phase=="reading-about" || k.phase=="going-home" || k.phase=="live" || k.phase=="shortcut" || k.phase=="calibration-complete" || (k.phase=="calibration" && k.calibrationForAdmission)}, {"admit",k.phase == "live" || k.phase == "shortcut"},
            {"input",k.input.state()}});
    }
    Keyboard *byIP(const std::string &ip) {
        for (auto &entry : keyboards) if (entry.second->ip == ip && entry.second->phase != "failed") return entry.second.get();
        return nullptr;
    }
    Keyboard *byWorker(const std::string &id) {
        for (auto &entry : keyboards) if (entry.second->worker == id && entry.second->phase != "failed") return entry.second.get();
        return nullptr;
    }
    void result(Keyboard &k, const J &request, const J &value, const std::string &error = "") {
        J route=J::object();
        for (auto key : {"action","cf","tag","events"}) if (request.contains(key)) route[key]=request[key];
        J message={{"type","input-result"}, {"connection",k.connection}, {"token",k.token}, {"request",route}, {"value",value}, {"error",error}};
        if (message.dump().size()>4096) { message["value"]=nullptr; message["error"]="Shortcut/input result exceeds the 4 KiB worker message limit"; }
        output(k.worker,std::move(message));
    }
    void fail(Keyboard &k, const std::string &error, bool disconnect = false) {
        if (k.phase == "failed") return;
        log(k,"pairing-failed",error); k.error = error;
        if (!k.remoteShortcut.is_null()) result(k,k.remoteShortcut,nullptr,error);
        for (const auto &entry : k.work)
            if (entry.second.operation == "remote") result(k,entry.second.request,nullptr,error);
        k.input.reset(); k.work.clear();
        if (k.calibration.value("active",false)) {
            k.calibration["active"]=false; k.calibration["error"]=error;
        }
        if (k.replayRequested) {
            try { task(k,"cancel-replay","typeSpecialKey",{{"key","endCapture"}}); }
            catch (const std::exception &e) { log(k,"replay-cancel-failed",e.what()); }
        }
        output(k.worker,{{"type","revoke"},{"connection",k.connection},{"token",k.token}},disconnect);
        k.operation = "Failed: " + error;
        k.pendingManual.clear();
        k.phase = "failed"; k.due = now() + opt.retryDelay;
        k.ip.clear(); k.worker.clear(); k.requestId.clear(); k.remoteShortcut = nullptr;
        wake.notify_all();
    }
    void task(Keyboard &k, const std::string &op, const std::string &action, const J &args = J::object(), const J &original = nullptr) {
        std::string id = uuid();
        k.input.submit({{"action",action},{"_pairingTask",id}},args);
        k.work[id] = {op,original};
    }
    void command(Keyboard &k, const J &input) {
        k.commandInput = input; k.command = input.at("command"); k.requestId = uuid();
        k.commandInput["requestId"] = k.requestId;
        k.fetched = false; k.reported = false; k.remoteReport = nullptr; k.deadline = now() + opt.timeout;
        log(k,"shortcut-queued",{{"command",k.command},{"requestId",k.requestId}});
    }
    bool acquire(Keyboard &k) {
        if (k.lease < 0) {
            gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, ("input:"+opt.helper+":"+k.host).c_str(),-1);
            std::string path = opt.directory+"/.lease-"+hash; g_free(hash);
            k.lease = open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
            if (k.lease < 0 || flock(k.lease,LOCK_EX|LOCK_NB)) {
                if (k.lease >= 0) close(k.lease);
                k.lease = -1; k.due = now()+opt.retryDelay; return false;
            }
        }
        return true;
    }
    void begin(Keyboard &k) {
        if (!acquire(k)) return;
        k.token = freshToken(); k.started = now(); k.expectedSerial.clear(); k.calibrationForAdmission=false;
        k.operation = "Pairing keyboard";
        if (!k.lastIP.empty()) {
            Bootstrap request; request.requestId=uuid(); request.at=k.started; request.fetched=false;
            bootstrap[k.lastIP]=request;
        }
        bool recovery=k.recoveryNext; k.recoveryNext=!recovery;
        k.phase = recovery ? "going-home-for-pairing" : "starting";
        k.initialReport = k.fetched = k.reported = k.replayDone = false;
        k.ip.clear(); k.worker.clear(); k.error.clear(); k.identity = J::object();
        k.deadline = now()+opt.retryDelay;
        log(k,"startup",{{"log",keyboardLogPath(k)}, {"sequence",recovery ? "home-home-enter-m8" : "m8"}});
        if (recovery) {
            k.operation="Recovery: sending first Home key";
            task(k,"startup-home-first","simulateHomeButton");
        } else {
            k.operation="Opening shortcut with Mouse 8 (no Home keys)";
            task(k,"startup-click","clickMouseButton",{{"button",8}});
        }
    }
    void completed(Keyboard &k, MouseInput::Result &r) {
        std::string id = r.request.value("_pairingTask", "");
        auto it = k.work.find(id); if (it == k.work.end()) return;
        Work work = it->second; k.work.erase(it);
        if (work.operation == "calibration") {
            k.calibration["busy"]=false;
            k.calibration["result"]=r.value;
            k.calibration["error"]=r.error;
            const std::string operation=work.request.at("operation");
            log(k,"mouse-calibration-result",{{"operation",operation},{"result",r.value},{"error",r.error}});
            if (!r.error.empty() || operation=="cancel") {
                stopCalibration(k,r.error);
            } else if (operation=="commit") {
                k.calibration["active"]=false; k.requestId.clear();
                k.error.clear(); k.operation="Mouse calibrated: "+r.value.value("name","");
                if (k.calibrationForAdmission) {
                    // Give the page time to receive its committed result before
                    // Home backgrounds Safari. Admission stays closed until Home completes.
                    k.phase="calibration-complete"; k.due=now()+1;
                    k.deadline=now()+opt.timeout;
                    log(k,"mouse-calibration-admission-ready");
                } else k.phase=k.calibrationReturnPhase;
                state(k);
            }
            return;
        }
        if (work.operation == "calibration-launch") {
            if (!r.error.empty()) stopCalibration(k,r.error);
            return;
        }
        if (work.operation == "cancel-replay") {
            if (!r.error.empty()) log(k,"replay-cancel-failed",r.error);
            else { k.replayRequested=false; log(k,"replay-cancelled"); }
            return;
        }
        if (work.operation == "manual-click") {
            k.operation = r.error.empty() ? "Mouse 8 clicked" : "Mouse 8 failed";
            k.error = r.error; log(k,"manual-mouse-8",r.error.empty() ? r.value : J(r.error));
            return;
        }
        if (work.operation == "remote") { result(k,work.request,r.value,r.error); return; }
        if (!r.error.empty()) { fail(k,r.error); return; }
        if (work.operation == "startup-home-first") {
            k.phase="home-key-delay"; k.due=now()+opt.homeDelay;
            k.operation="Waiting before second Home key"; log(k,"startup-home",{{"press",1}});
        } else if (work.operation == "startup-home-second") {
            k.phase="recovery-dismiss-delay"; k.due=now()+opt.homeDelay;
            k.operation="Waiting before dismissing old shortcut prompt"; log(k,"startup-home",{{"press",2}});
        } else if (work.operation == "startup-dismiss") {
            k.phase="shortcut-launch-delay"; k.due=now()+opt.homeDelay;
            k.operation="Waiting before Mouse 8"; log(k,"startup-dismissed-prompt");
        } else if (work.operation == "startup-click") {
            k.phase = "prompt-delay"; k.due = now()+opt.promptDelay;
            k.armDeadline=now()+opt.retryDelay;
            k.deadline=k.armDeadline+opt.promptDelay+1;
            k.operation="Waiting for shortcut prompt";
        }
        else if (work.operation == "startup-type") task(k,"startup-enter","typeSpecialKey",{{"key","Enter"}});
        else if (work.operation == "startup-enter") k.phase = "awaiting-initial-report";
        else if (work.operation == "replay") {
            k.phase = "waiting-for-airplay";
            k.replayProbe = k.input.state().value("probeSequence",0UL);
            log(k,"replay-started",r.value);
        } else if (work.operation == "home") {
            // Calibration can take long enough for another keyboard to finish
            // identifying the same serial. Recheck at the admission boundary.
            for (const auto &entry : keyboards) {
                const auto &other=*entry.second;
                if (&other!=&k && controlfloorActive(other) &&
                    other.identity.value("serialNumber","")==k.identity.value("serialNumber","")) {
                    fail(k,"Serial number already belongs to another live keyboard"); return;
                }
            }
            k.phase = "live"; k.operation="Waiting for ControlFloor";
            k.lastIdentity=k.identity; save(k); log(k,"live",k.identity); state(k);
        } else if (work.operation == "shortcut-click") log(k,"shortcut-triggered");
        else if (work.operation == "about-click") log(k,"about-shortcut-triggered");
    }
    bool quiescent(const Keyboard &k) const {
        return k.phase=="idle" || k.phase=="failed" || k.phase=="remembered" || k.phase=="paired" || k.phase=="calibration-required";
    }
    bool bindExisting(Keyboard &k) {
        if (k.ip.empty() || !k.initialReport) return false;
        auto selected=workers.end();
        for (auto it=workers.begin(); it!=workers.end(); ++it) {
            const auto &worker=it->second;
            if (!worker.connected || worker.peer!=k.ip || retiredWorkers.count(it->first)) continue;
            auto owner=byWorker(it->first);
            if (owner && owner!=&k) continue;
            if (selected==workers.end() || worker.connectedAt>selected->second.connectedAt) selected=it;
        }
        if (selected==workers.end()) return false;
        if (k.worker!=selected->first || k.connection!=selected->second.connection) {
            k.worker=selected->first; k.connection=selected->second.connection;
            log(k,"airplay-ip-bound",{{"existing",true}});
        }
        state(k); return true;
    }
    void startMirroring(Keyboard &k) {
        if (k.ip.empty()) {
            if (k.lastIP.empty()) throw std::runtime_error("Pair the keyboard to learn its IP first");
            if (auto other=byIP(k.lastIP)) if (other!=&k) throw std::runtime_error("Saved IP belongs to another active keyboard");
            k.ip=k.lastIP; k.token=freshToken(); k.initialReport=true;
            k.expectedSerial=k.lastIdentity.value("serialNumber","");
            log(k,"saved-ip-binding-resumed",k.ip);
        }
        command(k,{{"command","prefs-general-about"}});
        if (bindExisting(k)) {
            k.replayRequested=false; k.replayDone=true;
            k.phase="waiting-for-about"; k.operation="Reusing AirPlay; requesting About screen";
            log(k,"airplay-reused",{{"worker",k.worker},{"connection",k.connection}});
            state(k);
            task(k,"about-click","clickMouseButton",{{"button",8}});
            return;
        }
        k.phase="starting-replay"; k.replayRequested=true; k.replayDone=false; k.replayProbe=0;
        k.operation="Starting screen mirroring";
        task(k,"replay","typeSpecialKey",{{"key","replayCapture"}});
    }
    std::string calibrationURL(const Keyboard &k) {
        auto address=shortcutAddresses.find(k.ip);
        if (address==shortcutAddresses.end() || !ipv4(address->second))
            throw std::runtime_error("No local destination recorded for this phone; invoke its LAN shortcut again");
        return "http://"+address->second+":"+std::to_string(opt.port)+"/mouse-calibration#"+k.calibration.at("session").get<std::string>();
    }
    void stopCalibration(Keyboard &k, const std::string &error) {
        k.input.reset(); k.work.clear(); // Adapter cancels its own raw-HID session.
        k.calibration["active"]=false; k.calibration["busy"]=false; k.calibration["error"]=error;
        k.phase=k.calibrationReturnPhase; k.requestId.clear();
        k.error=error; k.operation=error.empty() ? "Mouse calibration cancelled" : "Mouse calibration failed";
        log(k,"mouse-calibration-stopped",error); state(k);
    }
    bool controlfloorActive(const Keyboard &k) const {
        // Include admission/reconnect and in-flight shortcuts, not just the last
        // asynchronous connected flag, so a socket cannot open mid-calibration.
        if (k.phase=="live" || k.phase=="shortcut") return true;
        auto worker=workers.find(k.worker);
        return worker!=workers.end() && worker->second.progress.value("token","")==k.token &&
            (worker->second.progress.value("providerConnected",false) || worker->second.progress.value("providerTransportOpen",false));
    }
    void startCalibration(Keyboard &k, bool force, bool admitAfter = false) {
        if (controlfloorActive(k)) throw std::runtime_error("Disconnect ControlFloor before calibrating the mouse");
        k.calibrationForAdmission=admitAfter || k.phase=="calibration-required";
        k.calibrationReturnPhase=k.calibrationForAdmission ? "calibration-required" : k.phase;
        k.calibration={{"session",uuid()},{"active",true},{"busy",false},{"job",0},
            {"force",force},{"requiredForAdmission",k.calibrationForAdmission},{"result",nullptr},{"error",""},{"status","Opening Safari"}};
        k.calibrationContact=now();
        k.calibration["url"]=calibrationURL(k);
        command(k,{{"command","open-url"},{"url",k.calibration["url"]}});
        k.phase="calibration"; k.operation="Mouse calibration: opening Safari";
        log(k,"mouse-calibration-started",{{"force",force},{"session",k.calibration["session"]}});
        task(k,"calibration-launch","clickMouseButton",{{"button",8}});
    }
    void reset(Keyboard &k, bool forget) {
        std::string oldIP=k.ip;
        if (k.phase!="failed") fail(k,"Stopped from keyboard dashboard");
        if (!oldIP.empty()) bootstrap.erase(oldIP);
        if (!k.lastIP.empty()) bootstrap.erase(k.lastIP);
        k.phase="idle"; k.autoConnect=false; k.manualHold=true; k.pendingManual.clear();
        k.recoveryNext=false;
        k.token.clear(); k.error.clear(); k.initialReport=false; k.identity=J::object();
        k.operation=forget ? "Binding forgotten; auto-connect paused" : "Auto-connect paused";
        if (forget) { k.lastIP.clear(); k.lastIdentity=J::object(); k.metadata=J::object(); }
        save(k); log(k,forget ? "binding-forgotten" : "auto-connect-disabled");
    }
    void tick(Keyboard &k) {
        MouseInput::Result r;
        while (k.input.take(r)) completed(k,r);
        auto input = k.input.state();
        bool ready = k.present && input.value("keyboardAvailable",false) && input.value("available",false);
        std::string captureState = input.value("mouse",J::object()).value("capture",J::object()).value("state","");
        bool free = ready && k.work.empty() && k.input.idle() && captureState != "replaying" && captureState != "recording";
        if (!k.pendingManual.empty() && free) {
            std::string action=k.pendingManual; k.pendingManual.clear();
            if (action=="pair") { k.continueAfterPair=false; begin(k); }
            else if (action=="mirror") startMirroring(k);
            else if (action=="calibrate" || action=="calibrate-new") startCalibration(k,action=="calibrate-new");
            else if (action=="mouse8") task(k,"manual-click","clickMouseButton",{{"button",8}});
            return;
        }
        if (k.phase=="calibration-complete" && now()>=k.due) {
            k.phase="going-home"; k.operation="Finishing setup after mouse calibration";
            task(k,"home","simulateHomeButton");
        }
        if (k.phase=="calibration") {
            if (!k.present) stopCalibration(k,"Keyboard disappeared during calibration");
            else if (now()-k.calibrationContact>60) stopCalibration(k,"Calibration page timed out");
            return;
        }
        if ((k.phase == "idle" || k.phase == "failed" || k.phase == "remembered") &&
            k.autoConnect && !k.manualHold && free && now() >= k.due) {
            k.continueAfterPair=true; begin(k); return;
        }
        if ((k.phase=="paired" || k.phase=="calibration-required") && !ready) { fail(k,"Paired keyboard disappeared; rediscovery required"); return; }
        if (quiescent(k)) return;
        if (!ready) { fail(k,"Keyboard/mouse disappeared or HID reports are unavailable"); return; }
        if (k.phase != "live" && now() > k.deadline) { fail(k,"Timed out waiting in "+k.phase); return; }
        if (k.phase == "shortcut" && k.reported && k.work.empty()) {
            J value=k.remoteReport, resultValue=value;
            std::string action=k.remoteShortcut.value("action", ""), error;
            if (value.value("status", "") == "error") error=value.value("error","Shortcut failed");
            else if (action=="getClipboardText") {
                if (!value.value("text",J()).is_string()) error="Shortcut clipboard report is missing text";
                else resultValue={{"text",value["text"]},{"hasText",value["text"]!=""}};
            } else if (action=="getDeviceName") resultValue=value.value("deviceName",J());
            result(k,k.remoteShortcut,resultValue,error);
            k.remoteShortcut=nullptr; k.remoteReport=nullptr; k.phase="live"; k.requestId.clear();
        }
        if (k.phase=="home-key-delay" && now()>=k.due) {
            k.phase="going-home-for-pairing"; k.operation="Sending second Home key";
            task(k,"startup-home-second","simulateHomeButton");
        }
        if (k.phase=="recovery-dismiss-delay" && now()>=k.due) {
            k.phase="dismissing-shortcut-prompt"; k.operation="Recovery: Enter to dismiss old shortcut prompt";
            task(k,"startup-dismiss","typeSpecialKey",{{"key","Enter"}});
        }
        if (k.phase=="waiting-for-pairing-arm") {
            if (lastPairingPromptAt>=0 && lastPairingPromptAt<=k.armDeadline && now()-lastPairingPromptAt<=opt.pairingArmWindow) {
                k.phase="prompt-delay";
                k.due=std::max(now(),lastPairingPromptAt+opt.promptDelay);
                k.operation="Pairing armed; waiting for shortcut prompt";
                return;
            }
            if (now()>=k.armDeadline) {
                log(k,"pairing-code-skipped",{{"reason","No recent unpaired shortcut request"},
                    {"armingWindowSeconds",opt.pairingArmWindow},{"waitSeconds",opt.retryDelay}});
                fail(k,"Pairing not armed: no recent unpaired shortcut request; code not typed");
                k.due=now(); // The retry wait has already elapsed since Mouse 8.
            } else k.operation="Waiting for unpaired shortcut request; next attempt in "+
                std::to_string(static_cast<int>(std::ceil(k.armDeadline-now())))+" seconds";
            return;
        }
        if (k.phase=="shortcut-launch-delay" && now()>=k.due) {
            k.phase="starting"; k.operation="Opening shortcut with Mouse 8";
            task(k,"startup-click","clickMouseButton",{{"button",8}});
        }
        if (k.phase == "prompt-delay" && now() >= k.due) {
            if (lastPairingPromptAt < 0 || now()-lastPairingPromptAt > opt.pairingArmWindow) {
                k.phase="waiting-for-pairing-arm";
                k.operation="Waiting for unpaired shortcut request; code not typed";
                log(k,"pairing-awaiting-arm");
                return;
            }
            k.phase = "typing-token";
            task(k,"startup-type","typeText",{{"text",k.token}});
        }
        if (k.phase == "awaiting-initial-report" && k.initialReport) {
            if (k.continueAfterPair) startMirroring(k);
            else { k.phase="paired"; k.operation="IP bound; ready to start screen mirroring"; log(k,"manual-pairing-complete"); }
        }
        if (k.phase == "waiting-for-airplay") {
            J capture = input.value("mouse",J::object()).value("capture",J::object());
            if (input.value("probeSequence",0UL) > k.replayProbe && capture.value("state","") == "idle") {
                if (capture.contains("error") && !capture["error"].is_null() && capture["error"] != "") {
                    fail(k,"Mirroring replay failed: "+capture["error"].dump()); return;
                }
                if (!k.replayDone) { k.replayDone = true; k.replayRequested=false; log(k,"replay-completed"); }
            }
            if (k.replayDone && !k.worker.empty() && k.reported) {
                k.phase = "reading-about"; k.deadline = now()+opt.timeout;
                log(k,"about-ocr-enabled");
            }
        }
        if (k.phase=="waiting-for-about" && k.reported && k.work.empty() && k.input.idle()) {
            k.phase="reading-about"; k.deadline=now()+opt.timeout;
            log(k,"about-ocr-enabled"); state(k);
        }
        if (k.phase == "reading-about") {
            auto it = workers.find(k.worker);
            if (it != workers.end() && it->second.progress.value("token","")==k.token && it->second.progress.value("ready",false)) {
                k.identity = it->second.progress.value("identity",J::object());
                std::string serial = k.identity.value("serialNumber", "");
                if (serial.empty() || k.identity.value("keyboardId", "") != k.token) return;
                if (!k.expectedSerial.empty() && serial!=k.expectedSerial) {
                    fail(k,"Saved IP now identifies a different phone; pair the keyboard again"); return;
                }
                for (const auto &other : keyboards)
                    if (other.second.get() != &k && (other.second->phase == "live" || other.second->phase == "shortcut") &&
                        other.second->identity.value("serialNumber", "") == serial) {
                        fail(k,"Serial number already belongs to another live keyboard"); return;
                    }
                k.expectedSerial.clear();
                k.lastIdentity=k.identity; save(k);
                log(k,"device-identified",k.identity);
                startCalibration(k,false,true); state(k);
            }
        }
    }
    void inventory() {
        J list = http(opt.helper+"/devices").at("bluetoothHosts");
        if (!list.is_array() || list.size()>256) throw std::runtime_error("Invalid or oversized keyboard inventory");
        std::lock_guard<std::mutex> lock(mutex);
        inventoryAt=g_get_real_time()/1000; inventoryError.clear();
        for (auto &entry : keyboards) entry.second->present = false;
        for (const auto &entry : list) {
            std::string host = entry.value("bluetoothHost", "");
            if (host.empty() || host.size()>128) continue;
            if (!keyboards.count(host)) {
                if (keyboards.size()>=256) continue;
                auto k = std::unique_ptr<Keyboard>(new Keyboard); k->host=host;
                k->metadata=J::object(); keyboards[host]=std::move(k);
            }
            auto &k=*keyboards[host]; k.present=true; k.lastSeen=g_get_real_time()/1000;
            if (!k.adapterStarted) { k.input.start(opt.helper,host); k.adapterStarted=true; }
            if (!k.savedAt || now()-k.savedAt>30) save(k);
        }
    }
    void run() {
        double probe=0, publish=0;
        while (running) {
            if (now() >= probe) {
                try { inventory(); }
                catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    inventoryError=e.what();
                    for (auto &entry : keyboards) entry.second->present=false;
                    fprintf(stderr,"ControlFloor pairing inventory: %s\n",e.what());
                }
                probe=now()+1;
            }
            std::unique_lock<std::mutex> lock(mutex);
            for (auto &entry : keyboards) {
                try { tick(*entry.second); }
                catch (const std::exception &e) { fail(*entry.second,e.what()); }
            }
            if (now()>=publish) {
                for (auto &entry : keyboards) if (entry.second->phase != "failed") state(*entry.second);
                publish=now()+.25;
                for (auto it=bootstrap.begin(); it!=bootstrap.end();) {
                    if (now()-it->second.at > opt.retryDelay) it=bootstrap.erase(it); else ++it;
                }
                for (auto it=receipts.begin(); it!=receipts.end();) {
                    if (now()>it->second.second) it=receipts.erase(it); else ++it;
                }
            }
            wake.wait_for(lock,std::chrono::milliseconds(20));
        }
    }
    ix::HttpResponsePtr next(const std::string &ip, bool head) {
        std::unique_lock<std::mutex> lock(mutex);
        if (head) return response(J::object());
        Keyboard *k = byIP(ip);
        if (k) {
            // Replay's final Mouse 8 may race the AirPlay event pipe. Never hand
            // out About until an actual receiver worker confirms this IPv4 peer.
            if (!k->requestId.empty() && !k->fetched && k->worker.empty() && k->command=="prefs-general-about") {
                wake.wait_for(lock,std::chrono::seconds(10),[&] { return !running || k->phase=="failed" || !k->worker.empty(); });
                if (!running || k->phase=="failed") return response({{"error","Pairing expired"}},409);
                if (k->worker.empty()) { fail(*k,"Shortcut requested About before AirPlay arrived"); return response({{"error","AirPlay connection not confirmed"}},409); }
            }
            if (!k->requestId.empty() && !k->fetched) {
                k->fetched=true; log(*k,"shortcut-fetched",{{"command",k->command},{"requestId",k->requestId}});
                return response(k->commandInput);
            }
            return response({{"command","ping"},{"requestId",""}});
        }
        for (const auto &entry : keyboards) {
            const auto &known=*entry.second;
            if (known.lastIP==ip && !known.autoConnect && quiescent(known) && known.pendingManual!="pair")
                return response({{"command","ping"},{"requestId",""}});
        }
        auto found=bootstrap.find(ip);
        if (found!=bootstrap.end() && now()-found->second.at <= opt.retryDelay) {
            if (!found->second.fetched) {
                found->second.fetched=true;
                lastPairingPromptAt=now();
                return response({{"command","initiate-pairing"},{"requestId",found->second.requestId}});
            }
            return response({{"command","ping"},{"requestId",""}});
        }
        if (bootstrap.size()>=512) return response({{"error","Too many unpaired callers"}},503);
        Bootstrap b; b.requestId=uuid(); b.at=now(); bootstrap[ip]=b;
        lastPairingPromptAt=now();
        return response({{"command","initiate-pairing"},{"requestId",b.requestId}});
    }
    ix::HttpResponsePtr report(const std::string &ip, const J &value) {
        if (!value.is_object() || !value.value("requestId",J()).is_string() || !value.value("command",J()).is_string() ||
            !value.value("status",J()).is_string() || value.value("protocolVersion",J()) != 2 ||
            !value.value("shortcutVersion",J()).is_string()) return response({{"error","Invalid shortcut report envelope"}},400);
        std::lock_guard<std::mutex> lock(mutex);
        std::string id=value["requestId"], cmd=value["command"], status=value["status"];
        if (id.empty() && (cmd=="ping" || cmd=="version")) return response({{"ok",true}});
        auto receipt=receipts.find(id);
        if (receipt!=receipts.end() && receipt->second.first==ip && now()<receipt->second.second)
            return response({{"ok",true},{"duplicate",true}});
        if (cmd=="initiate-pairing") {
            auto initial=bootstrap.find(ip);
            if (initial==bootstrap.end() || initial->second.requestId!=id || now()-initial->second.at>opt.retryDelay)
                return response({{"error","Expired or unknown pairing request"}},409);
            std::string tokenValue=enteredToken(value);
            Keyboard *k=nullptr;
            for (auto &entry : keyboards) if (entry.second->token==tokenValue && entry.second->present &&
                (entry.second->phase=="typing-token" || entry.second->phase=="awaiting-initial-report") &&
                initial->second.at>=entry.second->started) k=entry.second.get();
            if (!k || status!="ok") return response({{"error","Unknown or expired keyboard pairing token"}},409);
            if (auto old=byIP(ip)) if (old!=k) fail(*old,"IPv4 address claimed by a new pairing attempt");
            k->recoveryNext=false;
            k->ip=ip; k->lastIP=ip; k->initialReport=true; k->deadline=now()+opt.timeout; k->metadata=value;
            save(*k); log(*k,"keyboard-ip-bound",value); bootstrap.erase(initial); bindExisting(*k);
        } else {
            Keyboard *k=byIP(ip);
            if (!k || k->requestId!=id || k->command!=cmd || !k->fetched || now()>k->deadline)
                return response({{"error","Unknown, stale or wrong-peer shortcut report"}},409);
            if (status=="progress") {
                log(*k,"shortcut-progress",value);
                if (!k->remoteShortcut.is_null()) output(k->worker,{{"type","shortcut-progress"},{"connection",k->connection},{"token",k->token},
                    {"request",k->remoteShortcut},{"value",value}});
                return response({{"ok",true}});
            }
            if (status!="ok" && status!="error") return response({{"error","Invalid shortcut status"}},400);
            log(*k,"shortcut-report",value); k->reported=true;
            if (k->phase=="shortcut") {
                // A fast callback can arrive during Mouse 8's HTTP request.
                // Complete only after that trigger has released its button.
                k->remoteReport=value;
            } else if (status=="error") {
                if (k->phase=="calibration") stopCalibration(*k,value.value("error","Shortcut failed"));
                else fail(*k,value.value("error","Shortcut failed"));
            }
            state(*k);
        }
        if (receipts.size()<2048) receipts[id]={ip,now()+opt.timeout};
        wake.notify_all(); return response({{"ok",true}});
    }
    J dashboard() {
        std::lock_guard<std::mutex> lock(mutex);
        J rows=J::array();
        for (const auto &entry : keyboards) {
            auto &k=*entry.second;
            J input=k.input.state();
            bool ready=k.present && input.value("keyboardAvailable",false) && input.value("available",false);
            std::string capture=input.value("mouse",J::object()).value("capture",J::object()).value("state","");
            bool free=k.work.empty() && k.input.idle() && k.pendingManual.empty() && capture!="recording" && capture!="replaying";
            bool controllable=ready && free && (quiescent(k) || k.phase=="live");
            bool cf=false, transport=false;
            auto worker=workers.find(k.worker);
            if (worker!=workers.end() && worker->second.progress.value("token","")==k.token) {
                cf=worker->second.progress.value("providerConnected",false);
                transport=worker->second.progress.value("providerTransportOpen",false);
            }
            std::string display=k.phase;
            if (k.phase=="live") display=cf ? "connected-to-controlfloor" : "connecting-to-controlfloor";
            if (!k.present) display="offline";
            rows.push_back({{"id",k.host},{"source","iphone-use"},{"label",k.label},{"present",k.present},
                {"autoConnect",k.autoConnect},{"manualHold",k.manualHold},{"phase",k.phase},{"displayPhase",display},
                {"pairingCode",k.token},{"ip",k.ip},{"savedIP",k.lastIP},{"bindingVerified",!k.ip.empty() && k.initialReport && k.expectedSerial.empty()},
                {"identity",k.lastIdentity},{"metadata",k.metadata},{"lastSeen",k.lastSeen},{"error",k.error},
                {"calibration",k.calibration},{"operation",k.operation},{"pendingAction",k.pendingManual},{"inputReady",ready},{"captureState",capture},
                {"airplayConnected",!k.worker.empty()},{"providerConnected",cf},{"providerTransportOpen",transport},
                {"retryIn",k.autoConnect && !k.manualHold && k.phase=="failed" ? std::max(0.0,k.due-now()) : 0},
                {"actions",{{"mouse8",controllable},{"pair",controllable},
                    {"calibrate",controllable && !controlfloorActive(k) && !k.ip.empty() && k.initialReport},
                    {"calibrate-new",controllable && !controlfloorActive(k) && !k.ip.empty() && k.initialReport},
                    {"cancel-calibration",k.phase=="calibration"},
                    {"mirror",controllable && quiescent(k) && (!k.ip.empty() || !k.lastIP.empty())},{"reset",true},{"resume",true}}},
                {"history",k.history}});
        }
        J unpaired = J::array();
        for (const auto &entry : workers) {
            const auto &worker = entry.second;
            if (!worker.connected || retiredWorkers.count(entry.first) || byIP(worker.peer)) continue;
            J client = worker.progress.value("airplay",J::object());
            unpaired.push_back({{"worker",entry.first},{"ip",worker.peer},{"connection",worker.connection},
                {"connectedAt",worker.connectedAt},{"clientId",client.value("id","")},
                {"clientName",client.value("name","")},{"model",client.value("model","")}});
        }
        return {{"keyboards",rows},{"unpairedAirplay",unpaired},{"mutationToken",mutationToken},{"helper",opt.helper},{"inventoryError",inventoryError},
            {"inventoryAt",inventoryAt},{"time",g_get_real_time()/1000},{"database",opt.database}};
    }
    ix::HttpResponsePtr control(const J &body) {
        if (!body.is_object() || !body.value("id",J()).is_string()) return response({{"error","Keyboard id is required"}},400);
        std::lock_guard<std::mutex> lock(mutex);
        auto it=keyboards.find(body["id"].get<std::string>());
        if (it==keyboards.end()) return response({{"error","Unknown keyboard"}},404);
        Keyboard &k=*it->second;
        std::string action=body.value("action","");
        if (action=="settings") {
            if (body.contains("autoConnect") && !body["autoConnect"].is_boolean()) return response({{"error","autoConnect must be boolean"}},400);
            if (body.contains("label") && (!body["label"].is_string() || body["label"].get_ref<const std::string &>().size()>80))
                return response({{"error","Label must be at most 80 bytes"}},400);
            if (body.contains("autoConnect")) {
                if (!body["autoConnect"].get<bool>()) reset(k,false);
                else { k.autoConnect=true; k.manualHold=false; k.due=0;
                    if (k.phase=="paired") k.pendingManual="mirror";
                    else if (k.phase=="calibration-required") k.pendingManual="calibrate";
                    log(k,"auto-connect-enabled"); }
            }
            if (body.contains("label")) k.label=body["label"].get<std::string>();
            save(k);
        } else if (action=="cancel-calibration") {
            if (k.phase!="calibration") return response({{"error","No active calibration"}},409);
            stopCalibration(k,"");
        } else if (action=="reset") reset(k,true);
        else if (action=="resume") {
            k.autoConnect=true; k.manualHold=false; k.due=0;
            if (k.phase=="paired") k.pendingManual="mirror";
            else if (k.phase=="calibration-required") k.pendingManual="calibrate";
            save(k); log(k,"auto-connect-enabled");
        } else {
            if (action!="mouse8" && action!="pair" && action!="mirror" && action!="calibrate" && action!="calibrate-new") return response({{"error","Unknown keyboard action"}},400);
            auto input=k.input.state();
            std::string capture=input.value("mouse",J::object()).value("capture",J::object()).value("state","");
            if (!k.present || !input.value("keyboardAvailable",false) || !input.value("available",false))
                return response({{"error","Keyboard/mouse is not ready"}},409);
            if (!k.work.empty() || !k.input.idle() || !k.pendingManual.empty() || capture=="recording" || capture=="replaying" ||
                (!quiescent(k) && k.phase!="live")) return response({{"error","Keyboard is busy; turn auto-connect off to stop it first"}},409);
            if ((action=="calibrate" || action=="calibrate-new") && controlfloorActive(k))
                return response({{"error","Disconnect ControlFloor before calibrating the mouse"}},409);
            if ((action=="calibrate" || action=="calibrate-new") && (k.ip.empty() || !k.initialReport))
                return response({{"error","Pair Keyboard first to learn the phone IP"}},409);
            if (action=="mirror" && (k.phase=="live" || (k.ip.empty() && k.lastIP.empty())))
                return response({{"error","Pair Keyboard first, or pause an existing connection before restarting mirroring"}},409);
            if (!acquire(k)) return response({{"error","Keyboard is owned by another UxPlay process"}},409);
            if (action=="pair") {
                bool automatic=k.autoConnect; reset(k,false); k.autoConnect=automatic; save(k);
            }
            k.manualHold=true; k.pendingManual=action; k.operation="Queued: "+action; k.error.clear();
            log(k,"dashboard-action",action);
        }
        wake.notify_all(); return response({{"ok",true},{"action",action}},202);
    }
    ix::HttpResponsePtr calibrationRequest(const std::string &ip, const J &body) {
        std::lock_guard<std::mutex> lock(mutex);
        Keyboard *k=byIP(ip);
        if (!k || !body.is_object() || !body.contains("session") ||
            k->calibration.value("session","").empty() || body["session"]!=k->calibration["session"])
            return response({{"error","Unknown, stale or wrong-phone calibration session"}},409);
        std::string operation=body.value("operation","");
        k->calibrationContact=now();
        if (operation=="status") { J value=k->calibration; value["inputIdle"]=k->work.empty() && k->input.idle(); return response(value); }
        if (controlfloorActive(*k)) return response({{"error","Mouse calibration is unavailable while ControlFloor is connected or connecting"}},409);
        if (!k->calibration.value("active",false)) return response({{"error","Calibration has ended"}},409);
        if (operation=="abort") {
            stopCalibration(*k,body.value("error","Calibration page cancelled")); return response(k->calibration);
        }
        if (k->calibration.value("busy",false) || !k->work.empty() || !k->input.idle())
            return response({{"error","Previous calibration operation is still running"}},409);
        static const std::set<std::string> operations={"begin","profiles","trial","select","prepare","commit","cancel"};
        if (!operations.count(operation)) return response({{"error","Unknown calibration operation"}},400);
        J args=body;
        // Routing and force policy belong to the coordinator, never to page input.
        args.erase("device"); args.erase("mac"); args.erase("error"); args.erase("label");
        args["testVersion"]=1; args["force"]=k->calibration.at("force");
        args["deviceID"]="airplay-"+k->lastIdentity.value("serialNumber",k->host);
        std::string label=body.value("label",operation).substr(0,120);
        k->calibration["status"]=label; k->operation="Mouse calibration: "+label;
        k->calibration["job"]=k->calibration.value("job",0)+1;
        k->calibration["busy"]=true; k->calibration["error"]=""; k->calibration["result"]=nullptr;
        task(*k,"calibration","_mouseCalibration",args,{{"operation",operation}});
        wake.notify_all(); return response(k->calibration,202);
    }
    ix::HttpResponsePtr disconnectAirplay(const J &body) {
        if (!body.is_object() || !body.value("worker",J()).is_string() ||
            !body.value("connection",J()).is_number_unsigned()) return response({{"error","Worker and connection revision are required"}},400);
        std::lock_guard<std::mutex> lock(mutex);
        std::string id=body["worker"];
        auto it=workers.find(id);
        if (it==workers.end()) return response({{"error","AirPlay client no longer exists"}},404);
        if (!it->second.connected || retiredWorkers.count(id) || body["connection"]!=it->second.connection)
            return response({{"error","AirPlay connection changed; refresh and try again"}},409);
        if (byIP(it->second.peer)) return response({{"error","Client is now paired; reset the keyboard binding before disconnecting"}},409);
        output(id,J::object(),true);
        logHttp({{"event","airplay-disconnect-requested"},{"worker",id},{"peer",it->second.peer},{"connection",it->second.connection}});
        return response({{"ok",true}},202);
    }
    ix::HttpResponsePtr loggedHandle(ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState> connection) {
        double started = now();
        J common = {{"httpId",uuid()}, {"localIP",connection->getLocalIp()}, {"peer",connection->getRemoteIp()}, {"port",connection->getRemotePort()},
            {"method",request->method}, {"uri",request->uri}};
        J arrival = common;
        arrival["event"] = "http-request"; arrival["headers"] = request->headers;
        logBody(arrival, request->body);
        // Flush arrival before routing: even rejected or waiting requests remain visible.
        logHttp(std::move(arrival));
        auto reply = handle(request, connection);
        // IXHttpServer does not suppress HEAD bodies itself.
        if (request->method == "HEAD") reply->body.clear();
        J completion = common;
        completion["event"] = "http-response";
        completion["status"] = reply->statusCode;
        completion["durationMs"] = (now()-started)*1000;
        completion["bodyBytes"] = reply->body.size();
        std::string path = request->uri.substr(0,request->uri.find('?'));
        // Preserve command/error responses; avoid duplicating every dashboard snapshot,
        // HTML page and downloadable shortcut. All incoming bodies are kept in full.
        if (path!="/" && path!="/api/keyboards" && path!="/shortcuts/ControlFloorLAN.shortcut" && path!="/mouse-calibration" &&
            path!="/mouse-calibration.js" && path!="/mouse-calibration-page.js")
            logBody(completion, reply->body);
        else completion["bodyOmitted"] = true;
        logHttp(std::move(completion));
        return reply;
    }
    ix::HttpResponsePtr handle(ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState> connection) {
        try {
            std::string ip=connection->getRemoteIp(), path=request->uri.substr(0,request->uri.find('?'));
            if ((path=="/shortcuts/next" || path=="/shortcuts/report") && ipv4(connection->getLocalIp())) {
                std::lock_guard<std::mutex> lock(mutex);
                if (shortcutAddresses.size()<2048 || shortcutAddresses.count(ip)) shortcutAddresses[ip]=connection->getLocalIp();
            }
            if (!ipv4(ip)) return response({{"error","IPv4 LAN required"}},400);
            if (request->method=="GET" && (path=="/mouse-calibration" || path=="/mouse-calibration.js" || path=="/mouse-calibration-page.js"))
                return std::make_shared<ix::HttpResponse>(200,"OK",ix::HttpErrorCode::Ok,
                    ix::WebSocketHttpHeaders{{"Content-Type",path=="/mouse-calibration" ? "text/html; charset=utf-8" : "text/javascript; charset=utf-8"},
                        {"Cache-Control","no-store"},{"Connection","close"},{"X-Frame-Options","DENY"},{"X-Content-Type-Options","nosniff"}},
                    path=="/mouse-calibration" ? mouseCalibrationHTML : path=="/mouse-calibration.js" ? mouseCalibrationJS : mouseCalibrationPageJS);
            if (request->method=="POST" && path=="/api/calibration") {
                auto origin=request->headers.find("Origin"), host=request->headers.find("Host");
                if (origin!=request->headers.end() && (host==request->headers.end() || origin->second!="http://"+host->second))
                    return response({{"error","Cross-origin calibration is not allowed"}},403);
                if (request->body.size()>65536) return response({{"error","Calibration request too large"}},413);
                return calibrationRequest(ip,J::parse(request->body));
            }
            if (request->method=="GET" && path=="/api/keyboards") return response(dashboard());
            if (request->method=="POST" && (path=="/api/keyboards/control" || path=="/api/airplay/disconnect")) {
                auto key=request->headers.find("X-UxPlay-Token");
                if (key==request->headers.end() || key->second!=mutationToken) return response({{"error","Reload the dashboard before sending commands"}},403);
                auto origin=request->headers.find("Origin"), host=request->headers.find("Host");
                if (origin!=request->headers.end() && (host==request->headers.end() || origin->second!="http://"+host->second))
                    return response({{"error","Cross-origin controls are not allowed"}},403);
                if (request->body.size()>4096) return response({{"error","Control request too large"}},413);
                return path=="/api/airplay/disconnect" ? disconnectAirplay(J::parse(request->body)) : control(J::parse(request->body));
            }
            if (request->method=="GET" && path=="/")
                return std::make_shared<ix::HttpResponse>(200,"OK",ix::HttpErrorCode::Ok,
                    ix::WebSocketHttpHeaders{{"Content-Type","text/html; charset=utf-8"},{"Cache-Control","no-store"},
                        {"Connection","close"},{"X-Frame-Options","DENY"},{"X-Content-Type-Options","nosniff"}},keyboardDashboard);

            if ((request->method=="GET" || request->method=="HEAD") && path=="/shortcuts/next") return next(ip,request->method=="HEAD");
            if (request->method=="POST" && path=="/shortcuts/report") {
                if (request->body.size()>65536) return response({{"error","Report too large"}},413);
                return report(ip,J::parse(request->body));
            }
            if ((request->method=="GET" || request->method=="HEAD") && path=="/shortcuts/ControlFloorLAN.shortcut") {
                if (artifact.empty()) return response({{"error","No LAN shortcut artifact configured"}},404);
                return std::make_shared<ix::HttpResponse>(200,"OK",ix::HttpErrorCode::Ok,
                    ix::WebSocketHttpHeaders{{"Connection","close"},{"Content-Type","application/octet-stream"},{"Content-Disposition","attachment; filename=ControlFloorLAN.shortcut"}},
                    request->method=="HEAD" ? "" : artifact);
            }
            if (request->method=="GET" && path=="/health") return response({{"service","UxPlay ControlFloor LAN"},{"protocolVersion",2}});
            return response({{"error","Unknown shortcut route or method"}},404);
        } catch (const std::exception &e) { return response({{"error",e.what()}},400); }
    }
};
PairingCoordinator::PairingCoordinator(const PairingOptions &opt) : impl(new Impl(opt)) {}
PairingCoordinator::~PairingCoordinator() { stop(); }
void PairingCoordinator::start() {
    if (!impl->opt.port) throw std::runtime_error("A nonzero LAN port is required");
    if (g_mkdir_with_parents(impl->opt.directory.c_str(),0700)) throw std::runtime_error("Cannot create pairing log directory");
    // Shared whole-helper lease excludes the legacy sole-phone adapter, while
    // per-keyboard exclusive leases permit multiple LAN workers/controllers.
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256,("input:"+impl->opt.helper).c_str(),-1);
    std::string leasePath = impl->opt.directory+"/.lease-"+hash; g_free(hash);
    impl->helperLease = open(leasePath.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if (impl->helperLease < 0 || flock(impl->helperLease,LOCK_SH|LOCK_NB))
        throw std::runtime_error("iphone-use is owned by a legacy single-phone provider; stop it before LAN pairing");
    if (impl->opt.database.empty()) impl->opt.database=impl->opt.directory+"/keyboards.sqlite3";
    impl->store.open(impl->opt.database);
    for (const auto &record : impl->store.load(impl->source())) {
        auto k=std::unique_ptr<Impl::Keyboard>(new Impl::Keyboard);
        k->host=record.at("host"); k->autoConnect=record.value("autoConnect",true);
        k->label=record.value("label", ""); k->lastIP=record.value("lastIP", "");
        k->lastIdentity=record.value("identity",J::object()); k->lastSeen=record.value("lastSeen",int64_t(0));
        k->metadata={{"model",record.value("model", "")},{"iosVersion",record.value("iosVersion", "")}};
        k->phase=k->lastIP.empty() ? "idle" : "remembered";
        impl->keyboards[k->host]=std::move(k);
    }
    if (!impl->opt.artifact.empty()) {
        std::ifstream file(impl->opt.artifact,std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read LAN shortcut artifact");
        impl->artifact.assign(std::istreambuf_iterator<char>(file),{});
        if (impl->artifact.size()>16*1024*1024) throw std::runtime_error("LAN shortcut artifact too large");
    }
    impl->httpLog = appendLog(impl->opt.directory+"/http.jsonl");
    if (!impl->httpLog) throw std::runtime_error("Cannot open LAN HTTP request log");
    impl->logHttp({{"event","http-server-starting"},{"listen",impl->opt.listenAddress},{"port",impl->opt.port}});
    impl->server.reset(new ix::HttpServer(impl->opt.port,impl->opt.listenAddress,128,128,AF_INET,15,5));
    impl->server->setOnConnectionCallback([this](ix::HttpRequestPtr r,std::shared_ptr<ix::ConnectionState> c) { return impl->loggedHandle(r,c); });
    auto bound=impl->server->listen(); if (!bound.first) throw std::runtime_error(bound.second);
    impl->running=true; impl->server->start(); impl->thread=std::thread([this] { impl->run(); });
    fprintf(stderr,"ControlFloor LAN shortcut server listening on %s:%u\n",impl->opt.listenAddress.c_str(),port());
}
void PairingCoordinator::stop() {
    if (!impl->running.exchange(false)) return;
    impl->wake.notify_all();
    if (impl->thread.joinable()) impl->thread.join();
    impl->server->stop();
    bool cancelling=false;
    for (auto &entry : impl->keyboards) {
        auto &k=*entry.second;
        if (k.replayRequested) {
            k.input.reset();
            try { impl->task(k,"cancel-replay","typeSpecialKey",{{"key","endCapture"}}); cancelling=true; }
            catch (...) {}
        }
    }
    double deadline=now()+3;
    while (cancelling && now()<deadline) {
        cancelling=false;
        for (auto &entry : impl->keyboards) {
            auto &k=*entry.second; MouseInput::Result result;
            while (k.input.take(result)) {
                if (result.request.value("action", "")=="typeSpecialKey") k.replayRequested=false;
            }
            cancelling |= k.replayRequested;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto &entry : impl->keyboards) entry.second->input.stop();
}
unsigned short PairingCoordinator::port() const { return impl->server ? impl->server->getPort() : 0; }
bool PairingCoordinator::take(PairingOutput &output) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->outputs.empty()) return false;
    output=std::move(impl->outputs.front()); impl->outputs.pop_front(); return true;
}
void PairingCoordinator::event(const std::string &type,const std::string &id,const std::string &peer,const std::string &payload) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!ipv4(peer)) { impl->output(id,J::object(),true); return; }
    if (type=="starting") { impl->workers[id].peer=peer; return; }
    if (type=="stopped") {
        if (auto k=impl->byWorker(id)) impl->fail(*k,"AirPlay worker stopped; rediscovery required");
        impl->workers.erase(id); impl->retiredWorkers.erase(id); return;
    }
    if (type!="message" || !impl->workers.count(id) || impl->retiredWorkers.count(id)) return;
    try {
        J message=J::parse(payload); auto &worker=impl->workers.at(id);
        std::string kind=message.value("type", "");
        if (kind=="status") {
            unsigned long revision=message.at("connection"); bool connected=message.value("connected",false);
            if (auto old=impl->byWorker(id)) if (!connected || (worker.connection && revision!=worker.connection))
                impl->fail(*old,"AirPlay session changed; rediscovery required");
            if (connected && (!worker.connected || revision!=worker.connection)) worker.connectedAt=g_get_real_time()/1000;
            if (!connected) worker.connectedAt=0;
            worker.connection=revision; worker.connected=connected; worker.progress=message;
            if (connected) {
                auto k=impl->byIP(peer);
                if (k && k->initialReport && k->phase!="failed") {
                    if (!k->worker.empty() && k->worker!=id) impl->fail(*k,"AirPlay worker replaced; rediscovery required");
                    else {
                        if (k->worker.empty()) { k->worker=id; k->connection=revision; impl->log(*k,"airplay-ip-bound"); }
                        impl->state(*k);
                    }
                }
            }
            impl->wake.notify_all(); return;
        }
        auto k=impl->byWorker(id);
        if (!k || message.value("connection",0UL)!=k->connection || message.value("token","")!=k->token) return;
        if (kind=="reset-input") {
            if (k->phase=="live" || k->phase=="shortcut") {
                k->input.reset(); k->work.clear(); k->remoteShortcut=nullptr;
                k->requestId.clear(); k->phase="live";
            }
            return;
        }
        if (kind=="input") {
            J request=message.at("request");
            if (k->phase!="live") { impl->result(*k,request,nullptr,"Pairing or shortcut execution is in progress"); return; }
            std::string action=request.at("action");
            if (isShortcutRequest(request)) {
                if (!k->work.empty()) { impl->result(*k,request,nullptr,"Input is busy; try the shortcut again"); return; }
                try {
                    J input=shortcutInput(request); impl->command(*k,input); k->remoteShortcut=request; k->phase="shortcut";
                    impl->task(*k,"shortcut-click","clickMouseButton",{{"button",8}});
                } catch (const std::exception &e) {
                    k->remoteShortcut=nullptr; k->requestId.clear(); k->phase="live";
                    impl->result(*k,request,nullptr,e.what());
                }
            } else if (MouseInput::handles(action)) {
                try { impl->task(*k,"remote",action,message.at("args"),request); }
                catch (const std::exception &e) { impl->result(*k,request,nullptr,e.what()); }
            } else impl->result(*k,request,nullptr,"Unsupported input action");
        }
    } catch (const std::exception &e) {
        if (auto k=impl->byWorker(id)) impl->fail(*k,std::string("Invalid worker message: ")+e.what(),true);
    }
}
} // namespace controlfloor
