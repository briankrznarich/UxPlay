// SPDX-License-Identifier: GPL-3.0-or-later
#include "../supervisor.h"
#include <gio/gio.h>
#include <cstdio>
#include <cstring>

struct Control {
    GMainLoop *loop;
    multidevice::Supervisor *supervisor;
};
struct Command {
    Control *control;
    std::string line;
};
static gboolean command(gpointer data) {
    std::unique_ptr<Command> request(static_cast<Command *>(data));
    if (request->line.compare(0, 5, "DROP ") == 0) {
        request->control->supervisor->disconnect(request->line.substr(5));
    } else {
        g_main_loop_quit(request->control->loop);
    }
    return G_SOURCE_REMOVE;
}
static gpointer input(gpointer data) {
    Control *control = static_cast<Control *>(data);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        g_main_context_invoke(NULL, command, new Command{control, line});
        if (!strcmp(line, "STOP")) return NULL;
    }
    g_main_context_invoke(NULL, command, new Command{control, "STOP"});
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    multidevice::Options options;
    const char *capacity = g_getenv("UXPLAY_TEST_CAPACITY");
    options.max_devices = capacity ? (unsigned int) g_ascii_strtoull(capacity, NULL, 10) : 2;
    options.idle_seconds = 2;
    for (int i = 1; i < argc; i++) options.arguments.push_back(argv[i]);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    multidevice::Supervisor supervisor(options, [](const multidevice::Event &event) {
        printf("EVENT %s %s %s\n", event.type.c_str(), event.session_id.c_str(), event.peer.c_str());
        fflush(stdout);
    });
    std::string error;
    if (!supervisor.start(error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    printf("READY %u\n", supervisor.port());
    fflush(stdout);
    Control control{loop, &supervisor};
    GThread *reader = g_thread_new("test-control", input, &control);
    g_main_loop_run(loop);
    supervisor.stop();
    g_thread_join(reader);
    g_main_loop_unref(loop);
    return 0;
}
