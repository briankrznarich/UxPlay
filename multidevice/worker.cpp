// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker.h"
#include <gio/gio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <atomic>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define fdopen _fdopen
#else
#include <unistd.h>
#endif

namespace multidevice {
static FILE *events = NULL;
static std::mutex event_mutex;
static std::atomic<bool> video_active(false);
static std::mutex command_mutex;
static std::function<void(const std::string &)> command_handler;
void worker_command_handler(std::function<void(const std::string &)> handler) {
    std::lock_guard<std::mutex> lock(command_mutex);
    command_handler = std::move(handler);
}

bool worker_enabled() {
    return g_getenv("UXPLAY_WORKER_PEER") != NULL;
}

static gpointer watch_parent(gpointer) {
    // The supervisor owns this pipe. EOF also handles a killed/crashed parent.
    // Process exit releases the sockets even before GStreamer's loop exists.
    char command[16384];
    while (fgets(command, sizeof(command), stdin)) {
        if (!strcmp(command, "STOP\n")) {
            exit(EXIT_SUCCESS);
        }
        if (!strchr(command, '\n')) exit(EXIT_FAILURE);
        GVariant *record = g_variant_parse(G_VARIANT_TYPE("(s)"), command, NULL, NULL, NULL);
        if (!record) exit(EXIT_FAILURE);
        const char *payload;
        g_variant_get(record, "(&s)", &payload);
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            if (command_handler) command_handler(payload);
        }
        g_variant_unref(record);
    }
    exit(EXIT_SUCCESS);
    return NULL;
}

bool worker_init() {
    if (!worker_enabled()) return true;
    GInetAddress *address = g_inet_address_new_from_string(g_getenv("UXPLAY_WORKER_PEER"));
    if (!address) return false;
    g_object_unref(address);
    // Reserve the original stdout pipe for framed events. Upstream's existing
    // printf logging remains untouched and goes to inherited stderr instead.
    fflush(stdout);
    int fd = dup(fileno(stdout));
    if (fd < 0) return false;
    events = fdopen(fd, "w");
    if (!events || dup2(fileno(stderr), fileno(stdout)) < 0) return false;
    g_thread_unref(g_thread_new("uxplay-parent", watch_parent, NULL));
    return true;
}

bool worker_configure(raop_t *raop) {
    if (!worker_enabled()) return true;
    GInetAddress *address = g_inet_address_new_from_string(g_getenv("UXPLAY_WORKER_PEER"));
    if (!address) return false;
    const char *scope = g_getenv("UXPLAY_WORKER_SCOPE");
    unsigned int zone = scope ? (unsigned int) g_ascii_strtoull(scope, NULL, 10) : 0;
    int result = raop_set_proxy_remote(raop, g_inet_address_to_bytes(address),
                                      (int) g_inet_address_get_native_size(address), zone);
    g_object_unref(address);
    return result == 0;
}

void worker_event(const char *type, const char *value, const char *name, const char *model) {
    if (!events) return;
    // GVariant text escapes tabs/newlines in remote names. One record per line;
    // no log scraping, JSON dependency, or extra publicly reachable IPC port.
    GVariant *record = g_variant_ref_sink(g_variant_new("(ssss)", type,
        value ? value : "", name ? name : "", model ? model : ""));
    gchar *line = g_variant_print(record, TRUE);
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        fprintf(events, "%s\n", line);
        fflush(events);
    }
    g_free(line);
    g_variant_unref(record);
}

void worker_ready(unsigned short port) {
    worker_event("ready", std::to_string(port).c_str());
}

void worker_video(bool active) {
    if (events && video_active.exchange(active) != active) worker_event(active ? "streaming" : "idle");
}
} // namespace multidevice
