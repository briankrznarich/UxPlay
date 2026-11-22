// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UXPLAY_MULTIDEVICE_SUPERVISOR_H
#define UXPLAY_MULTIDEVICE_SUPERVISOR_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace multidevice {

struct Event {
    std::string type;
    std::string session_id;
    std::string peer;
    std::string device_id;
    std::string name;
    std::string model;
};

struct Options {
    std::vector<std::string> arguments;
    unsigned int max_devices = 8;
    unsigned int idle_seconds = 30;
    unsigned short port = 0;
};

// Use the default GLib main context on one thread for all methods/callbacks.
// Callbacks may inspect sessions or disconnect one; defer stop/destruction
// until outside a callback (stop drains pending asynchronous completions).
// The caller owns discovery. Workers never advertise themselves.
class Supervisor {
public:
    Supervisor(const Options &options, std::function<void(const Event &)> on_event);
    ~Supervisor();
    bool start(std::string &error);
    unsigned short port() const;
    std::vector<Event> sessions() const;
    bool disconnect(const std::string &session_id);
    // Stops accepting, terminates workers, and drains asynchronous completions.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace multidevice
#endif
