// SPDX-License-Identifier: GPL-3.0-or-later
#include "supervisor.h"
#include <gio/gio.h>
#include <algorithm>
#include <map>
#include <deque>
#include <cstring>
#include <utility>

namespace multidevice {
struct Supervisor::Impl {
    struct Worker {
        Impl *owner;
        Event event;
        GSubprocess *process = NULL;
        GCancellable *cancel = g_cancellable_new();
        std::vector<GSocketConnection *> waiting;
        std::string input;
        std::deque<std::string> output;
        unsigned short port = 0;
        unsigned int connections = 0;
        gint64 created = g_get_monotonic_time();
        gint64 idle_since = created;
        bool stopping = false;
        bool exited = false;
        bool admitted = false;
        bool streaming = false;

        explicit Worker(Impl *owner) : owner(owner) {}
        ~Worker() {
            for (auto connection : waiting) g_object_unref(connection);
            if (process) g_object_unref(process);
            g_object_unref(cancel);
        }
    };
    typedef std::shared_ptr<Worker> WorkerPtr;
    struct Relay {
        WorkerPtr worker;
        GSocketConnection *client;
        GSocketConnection *backend = NULL;
        GSocketClient *connector = NULL;
        explicit Relay(WorkerPtr worker, GSocketConnection *client)
            : worker(worker), client(client) {}
        ~Relay() {
            g_io_stream_close(G_IO_STREAM(client), NULL, NULL);
            g_object_unref(client);
            if (backend) {
                g_io_stream_close(G_IO_STREAM(backend), NULL, NULL);
                g_object_unref(backend);
            }
            if (connector) g_object_unref(connector);
            worker->connections--;
            worker->idle_since = g_get_monotonic_time();
        }
    };

    Options options;
    std::function<void(const Event &)> on_event;
    GSocketService *service = NULL;
    std::map<std::string, WorkerPtr> workers;
    guint timer = 0;
    unsigned short listen_port = 0;
    unsigned int pending = 0;
    bool stopping = false;

    Impl(const Options &options, std::function<void(const Event &)> on_event)
        : options(options), on_event(std::move(on_event)) {}

    void emit(WorkerPtr worker, const char *type) {
        worker->event.type = type;
        if (on_event) on_event(worker->event);
    }

    void stop_worker(WorkerPtr worker) {
        if (worker->stopping) return;
        worker->stopping = true;
        g_cancellable_cancel(worker->cancel);
        for (auto connection : worker->waiting) {
            g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
            g_object_unref(connection);
        }
        worker->connections -= worker->waiting.size();
        worker->waiting.clear();
        // Workers have no shared writable output files in multi-device mode.
        // Termination also cancels media sockets that bypass the control proxy.
        if (worker->process && !worker->exited) g_subprocess_force_exit(worker->process);
    }

    static void process_exited(GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<WorkerPtr> reference(static_cast<WorkerPtr *>(data));
        WorkerPtr worker = *reference;
        Impl *self = worker->owner;
        g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
        self->pending--;
        worker->exited = true;
        self->stop_worker(worker);
        auto entry = self->workers.find(worker->event.peer);
        if (entry != self->workers.end() && entry->second == worker) self->workers.erase(entry);
        self->emit(worker, "stopped");
    }

    static void spliced(GObject *, GAsyncResult *result, gpointer data) {
        std::unique_ptr<Relay> relay(static_cast<Relay *>(data));
        g_io_stream_splice_finish(result, NULL);
        relay->worker->owner->pending--;
    }

    static void connected(GObject *source, GAsyncResult *result, gpointer data) {
        Relay *relay = static_cast<Relay *>(data);
        Impl *self = relay->worker->owner;
        self->pending--;
        relay->backend = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, NULL);
        if (!relay->backend || relay->worker->stopping) {
            delete relay;
            return;
        }
        g_socket_set_timeout(g_socket_connection_get_socket(relay->backend), 0);
        self->pending++;
        // GIO uses bounded streaming buffers, not an accumulating message queue.
        // EOF/error in either direction closes both ends and cancels the other.
        g_io_stream_splice_async(G_IO_STREAM(relay->client), G_IO_STREAM(relay->backend),
            (GIOStreamSpliceFlags) (G_IO_STREAM_SPLICE_CLOSE_STREAM1 | G_IO_STREAM_SPLICE_CLOSE_STREAM2),
            G_PRIORITY_DEFAULT, relay->worker->cancel, spliced, relay);
    }

    void relay(WorkerPtr worker, GSocketConnection *connection) {
        Relay *pipe = new Relay(worker, connection); // takes the waiting/accepted reference
        pipe->connector = g_socket_client_new();
        g_socket_client_set_enable_proxy(pipe->connector, FALSE);
        g_socket_client_set_timeout(pipe->connector, 5);
        GInetAddress *address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
        GSocketAddress *endpoint = g_inet_socket_address_new(address, worker->port);
        pending++;
        g_socket_client_connect_async(pipe->connector, G_SOCKET_CONNECTABLE(endpoint),
                                     worker->cancel, connected, pipe);
        g_object_unref(endpoint);
        g_object_unref(address);
    }

    void record(WorkerPtr worker, const std::string &line) {
        GVariant *value = g_variant_parse(G_VARIANT_TYPE("(ssss)"), line.c_str(), NULL, NULL, NULL);
        if (!value) {
            stop_worker(worker);
            return;
        }
        const char *type, *id, *name, *model;
        g_variant_get(value, "(&s&s&s&s)", &type, &id, &name, &model);
        if (!strcmp(type, "ready")) {
            char *end = NULL;
            guint64 port = g_ascii_strtoull(id, &end, 10);
            if (worker->port || !*id || *end || !port || port > 65535) {
                stop_worker(worker);
            } else {
                worker->port = (unsigned short) port;
                emit(worker, "ready");
                std::vector<GSocketConnection *> waiting;
                waiting.swap(worker->waiting);
                for (auto connection : waiting) relay(worker, connection);
            }
        } else if (!strcmp(type, "client")) {
            worker->admitted = true;
            worker->event.device_id = id;
            worker->event.name = name;
            worker->event.model = model;
            emit(worker, "client");
        } else if (!strcmp(type, "message")) {
            worker->event.payload = id;
            emit(worker, "message");
            worker->event.payload.clear();
        } else if (!strcmp(type, "streaming")) {
            worker->streaming = true;
            emit(worker, "streaming");
        } else if (!strcmp(type, "idle")) {
            worker->streaming = false;
            worker->idle_since = g_get_monotonic_time();
            emit(worker, "idle");
        }
        g_variant_unref(value);
    }

    static void read_events(GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<WorkerPtr> reference(static_cast<WorkerPtr *>(data));
        WorkerPtr worker = *reference;
        Impl *self = worker->owner;
        self->pending--;
        GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, NULL);
        gsize size = 0;
        const char *text = bytes ? static_cast<const char *>(g_bytes_get_data(bytes, &size)) : NULL;
        if (size && !worker->stopping) {
            worker->input.append(text, size);
            size_t end;
            while (!worker->stopping && (end = worker->input.find('\n')) != std::string::npos) {
                std::string line = worker->input.substr(0, end);
                worker->input.erase(0, end + 1);
                self->record(worker, line);
            }
            if (worker->input.size() > 16384) self->stop_worker(worker);
            if (!worker->stopping) self->read(worker);
        } else {
            self->stop_worker(worker);
        }
        if (bytes) g_bytes_unref(bytes);
    }

    void read(WorkerPtr worker) {
        pending++;
        g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(worker->process), 4096,
            G_PRIORITY_DEFAULT, worker->cancel, read_events, new WorkerPtr(worker));
    }

    WorkerPtr spawn(const std::string &peer, const std::string &address, guint32 scope) {
        WorkerPtr worker = std::make_shared<Worker>(this);
        gchar *id = g_uuid_string_random();
        worker->event.session_id = id;
        g_free(id);
        worker->event.peer = peer;
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE));
        g_subprocess_launcher_setenv(launcher, "UXPLAY_WORKER_PEER", address.c_str(), TRUE);
        g_subprocess_launcher_setenv(launcher, "UXPLAY_WORKER_SCOPE", std::to_string(scope).c_str(), TRUE);
        std::vector<const char *> arguments;
        for (const auto &argument : options.arguments) arguments.push_back(argument.c_str());
        arguments.push_back(NULL);
        GError *error = NULL;
        worker->process = g_subprocess_launcher_spawnv(launcher, arguments.data(), &error);
        g_object_unref(launcher);
        if (!worker->process) {
            g_warning("Unable to start UxPlay worker: %s", error->message);
            g_clear_error(&error);
            return WorkerPtr();
        }
        workers[peer] = worker;
        pending++;
        g_subprocess_wait_async(worker->process, NULL, process_exited, new WorkerPtr(worker));
        read(worker);
        emit(worker, "starting");
        return worker;
    }

    static void written(GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<WorkerPtr> reference(static_cast<WorkerPtr *>(data));
        WorkerPtr worker = *reference;
        Impl *self = worker->owner;
        self->pending--;
        bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);
        worker->output.pop_front();
        if (!ok) self->stop_worker(worker);
        if (!worker->stopping && !worker->output.empty()) self->write(worker);
    }
    void write(WorkerPtr worker) {
        pending++;
        const auto &line = worker->output.front();
        g_output_stream_write_all_async(g_subprocess_get_stdin_pipe(worker->process),
            line.data(), line.size(), G_PRIORITY_DEFAULT, worker->cancel, written, new WorkerPtr(worker));
    }

    static gboolean incoming(GSocketService *, GSocketConnection *connection, GObject *, gpointer data) {
        Impl *self = static_cast<Impl *>(data);
        if (self->stopping) return FALSE;
        GSocketAddress *remote = g_socket_connection_get_remote_address(connection, NULL);
        if (!remote || !G_IS_INET_SOCKET_ADDRESS(remote)) {
            if (remote) g_object_unref(remote);
            return FALSE;
        }
        GInetAddress *address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));
        guint32 scope = g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6
            ? g_inet_socket_address_get_scope_id(G_INET_SOCKET_ADDRESS(remote)) : 0;
        gchar *text = g_inet_address_to_string(address);
        std::string host = text;
        g_free(text);
        // A dual-stack listener may represent an IPv4 peer as ::ffff:a.b.c.d.
        const guint8 *bytes = g_inet_address_to_bytes(address);
        const guint8 mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6 && !memcmp(bytes, mapped, 12)) {
            GInetAddress *ipv4 = g_inet_address_new_from_bytes(bytes + 12, G_SOCKET_FAMILY_IPV4);
            text = g_inet_address_to_string(ipv4);
            host = text;
            g_free(text);
            g_object_unref(ipv4);
            scope = 0;
        }
        std::string peer = host + (scope ? "%" + std::to_string(scope) : "");
        g_object_unref(remote);
        WorkerPtr worker;
        auto existing = self->workers.find(peer);
        if (existing != self->workers.end()) worker = existing->second;
        if (!worker && self->workers.size() < self->options.max_devices) worker = self->spawn(peer, host, scope);
        if (!worker || worker->stopping || worker->connections >= 8) return FALSE;
        worker->connections++;
        g_object_ref(connection);
        if (worker->port) self->relay(worker, connection);
        else worker->waiting.push_back(connection);
        return TRUE;
    }

    static gboolean tick(gpointer data) {
        Impl *self = static_cast<Impl *>(data);
        gint64 now = g_get_monotonic_time();
        for (const auto &entry : self->workers) {
            WorkerPtr worker = entry.second;
            if ((!worker->port && now - worker->created > 15 * G_USEC_PER_SEC) ||
                (!worker->admitted && now - worker->created > 60 * G_USEC_PER_SEC) ||
                (!worker->connections && !worker->streaming &&
                 now - worker->idle_since > (gint64) self->options.idle_seconds * G_USEC_PER_SEC)) {
                self->stop_worker(worker);
            }
        }
        return G_SOURCE_CONTINUE;
    }
};

Supervisor::Supervisor(const Options &options, std::function<void(const Event &)> on_event)
    : impl(new Impl(options, std::move(on_event))) {}

Supervisor::~Supervisor() { stop(); }

bool Supervisor::start(std::string &message) {
    if (impl->service || impl->stopping || impl->options.arguments.empty() || !impl->options.max_devices) {
        message = "Invalid or already started supervisor";
        return false;
    }
    impl->service = g_socket_service_new();
    GError *error = NULL;
    if (impl->options.ipv4_only) {
        GInetAddress *address = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
        GSocketAddress *endpoint = g_inet_socket_address_new(address, impl->options.port), *bound = NULL;
        if (g_socket_listener_add_address(G_SOCKET_LISTENER(impl->service), endpoint,
                G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error)) {
            impl->listen_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
            g_object_unref(bound);
        }
        g_object_unref(endpoint); g_object_unref(address);
    } else if (impl->options.port) {
        if (g_socket_listener_add_inet_port(G_SOCKET_LISTENER(impl->service), impl->options.port, NULL, &error)) {
            impl->listen_port = impl->options.port;
        }
    } else {
        impl->listen_port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(impl->service), NULL, &error);
    }
    if (!impl->listen_port) {
        message = error ? error->message : "Unable to bind receiver listener";
        g_clear_error(&error);
        return false;
    }
    g_signal_connect(impl->service, "incoming", G_CALLBACK(Impl::incoming), impl.get());
    impl->timer = g_timeout_add_seconds(1, Impl::tick, impl.get());
    g_socket_service_start(impl->service);
    return true;
}

unsigned short Supervisor::port() const { return impl->listen_port; }

std::vector<Event> Supervisor::sessions() const {
    std::vector<Event> result;
    for (const auto &entry : impl->workers) result.push_back(entry.second->event);
    return result;
}

bool Supervisor::disconnect(const std::string &session_id) {
    for (const auto &entry : impl->workers) {
        if (entry.second->event.session_id == session_id) {
            impl->stop_worker(entry.second);
            return true;
        }
    }
    return false;
}

bool Supervisor::send(const std::string &session_id, const std::string &payload) {
    if (payload.size() > 4096) return false;
    for (const auto &entry : impl->workers) {
        auto worker = entry.second;
        if (worker->event.session_id != session_id || worker->stopping) continue;
        if (worker->output.size() >= 16) return false;
        GVariant *record = g_variant_ref_sink(g_variant_new("(s)", payload.c_str()));
        gchar *line = g_variant_print(record, TRUE);
        worker->output.push_back(std::string(line) + "\n");
        g_free(line); g_variant_unref(record);
        if (worker->output.size() == 1) impl->write(worker);
        return true;
    }
    return false;
}

void Supervisor::stop() {
    if (impl->stopping) return;
    impl->stopping = true;
    if (impl->timer) g_source_remove(impl->timer);
    if (impl->service) {
        g_socket_service_stop(impl->service);
        g_socket_listener_close(G_SOCKET_LISTENER(impl->service));
    }
    for (const auto &entry : impl->workers) impl->stop_worker(entry.second);
    while (impl->pending) g_main_context_iteration(NULL, TRUE);
    if (impl->service) {
        g_object_unref(impl->service);
        impl->service = NULL;
    }
    impl->workers.clear();
}
} // namespace multidevice
