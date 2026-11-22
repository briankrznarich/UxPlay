# Experimental multi-device receiver

This fork adds `-multi N`: one advertised receiver name, with an isolated UxPlay
worker process for each connecting peer. It is an initial implementation for
**simultaneous screen mirroring**, not a claim of tested 100-phone capacity.
Without `-multi`, UxPlay follows its normal single-device path.

## Build and run

Use the normal UxPlay dependencies plus GIO >= 2.52 (part of GLib, commonly
already installed with GStreamer). No additional downloaded C++ dependency is
needed.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j 6
./build/uxplay -multi 4 -n "Multi-device AirPlay" -nh -vsync no
```

Select **Multi-device AirPlay** from Screen Mirroring on each phone. All phones
select the same destination. Each active worker uses the normal UxPlay video
renderer, so the initial UI is a separate video window per phone. `-vsync no`
is explicitly shown because it is needed for smooth playback on the Mac used
for this project; the feature does not change upstream's synchronization default.

`N` is a concurrent worker limit, from 1 to 128, not a performance guarantee.
Workers start lazily when a peer opens a control connection. Pending negotiations
and discovery probes also count until cleaned up. Excess peers are disconnected;
existing streams are not evicted. Start with two real phones before raising it.

Stop any other receiver using the same receiver MAC identity before testing.
To run beside another UxPlay instance, use a distinct locally administered
receiver identity, for example `-m 02:00:00:00:00:42`, as well as a distinct name.
Changing only the name is insufficient for the RAOP discovery record.

To build without the new module:

```sh
cmake -S . -B build/single -DENABLE_MULTIDEVICE=OFF
cmake --build build/single -j 6
```

## Architecture and orchestration boundary

```text
                        one Bonjour receiver identity
                                     |
Phones -- control TCP --> Supervisor (one advertised port)
                            |             |
                       loopback TCP  loopback TCP
                            |             |
                         worker A      worker B
                            ^             ^
Phone A -- negotiated media/timing ports  |
Phone B ------------ negotiated media/timing ports
```

The parent owns discovery, connection routing, capacity and worker lifetimes.
Each worker owns upstream's protocol state, decryption, clocks, GStreamer
pipeline and rendering globals. Workers do not advertise Bonjour services.
Their receiver name, MAC identity and pairing key match the parent.

Only control TCP passes through the parent. RTSP SETUP already negotiates
per-session media ports; workers allocate those dynamically and phones send
media directly to them. No compressed or decoded video is copied through the
supervisor. The small RAOP addition supplies the original phone address to the
worker so its NTP/audio connections target the phone, not the loopback proxy.
Proxied workers reject non-loopback connections to their control listener.

`multidevice/supervisor.h` is the central integration point:

- An event callback reports `starting`, `ready`, `client`, `streaming`, `idle`
  and `stopped`, with a worker session ID, peer address and available client
  ID/name/model.
- `sessions()` returns the current worker records.
- `disconnect(session_id)` terminates only that worker and its connections.
- `stop()` closes the listener and drains worker/relay cleanup.

Use the default GLib main context on one owning thread. Marshal events from
other threads into that context. Event callbacks may inspect sessions or request
an individual disconnect; defer stopping/destroying the supervisor until outside
a callback. The current application callback logs events. No ControlFloor,
keyboard orchestration, OCR or shortcut server is bundled in this fork.

A later coordinator can consume these events and keep keyboard inventory,
connection challenges and ControlFloor registration centrally, while adding a
small worker command/snapshot interface. It does not need to take over AirPlay
or pass all decoded frames through the parent. The process boundary is intended
to contain the existing global renderer state, not to make sessions autonomous.

**Identity:** routing currently uses the source IP address, plus IPv6 scope where
applicable. IPv4-mapped IPv6 addresses are normalized. This assumes a controlled
LAN with a distinct address per phone. NAT/shared addresses and associating a
phone's different IPv4/IPv6 addresses are not implemented. IP is a routing key,
not durable hardware identity. A worker session ID lasts for that worker's
lifetime, including a short reconnect grace period. `client` means that the
upstream admission callback saw a client, not that its identity is verified or
video has arrived. Revalidate keyboard/phone binding on every `client` event;
never reuse a binding solely because its worker or IP still exists. `streaming`
means encoded video reached the video callback, not that rendering succeeded.

## Internal IPC and lifecycle

Workers are launched with the original argument vector and explicit overrides
for the resolved receiver name and MAC. They reread the same configuration file;
avoid changing that file or a pairing key file during a running receiver.
`UXPLAY_WORKER_PEER` and `UXPLAY_WORKER_SCOPE` are internal launcher environment
variables, not user configuration options.

The original worker stdout pipe carries newline-delimited GVariant text tuples
`(type, value, name, model)`, four strings. `ready` supplies a listening port;
`client` supplies the AirPlay ID, name and model. `streaming` and `idle` signal
video transitions. Ordinary worker stdout is redirected to inherited stderr
before any startup output, so logs cannot corrupt the event channel. Strings
are escaped, not assembled into executable commands. Parent reads are bounded
and malformed/oversized records terminate that worker.

The parent owns the child's stdin pipe. EOF causes the worker to exit even if
the parent crashes. A `STOP` line also exits it. The supervisor currently uses
process termination for explicit disconnect/shutdown, and waits/reaps workers;
file recording is excluded so workers do not share buffered output files.
Control relays use GIO's bounded asynchronous splicing and close both sides
when either side disconnects. They do not build a queue of whole video frames.

Defaults are eight control sockets per worker, 15 seconds to become ready,
60 seconds to report client admission, and 30 seconds idle with no control
connections or active video. Startup/client deadlines are measured from worker
creation. A disconnected idle worker can be reused during the grace period.
A dead worker is removed; a subsequent phone connection launches a new worker.
No automatic reconnection of an iPhone's Screen Mirroring is attempted.

## Current scope and limitations

- Audio is enabled by default in each worker, using upstream's audio decoder,
  timing and per-client volume controls. Concurrent phones play together through
  the host's audio output; there is no selected-phone audio policy yet. Use
  `-as 0` (or `-a`) to mute all workers, or `-as <sink>` to choose their audio sink.
  Concurrent playback requires an output backend that supports sharing; exclusive
  hardware sinks may reject additional workers.
- HLS URL playback (`-hls`), recording/dumps, cover-art/metadata/DACP file output,
  BLE advertisement files and the writable PIN registration list (`-reg`) are
  rejected in this mode. Ordinary single-device behavior is unaffected.
- Worker control and media ports are dynamic. A configured `-p` selects the
  parent's control port using UxPlay's existing TCP port ordering; worker port
  settings are reset to zero to avoid collisions. Fixed media port ranges and
  NAT traversal are not implemented. The LAN firewall must permit negotiated
  TCP/UDP media traffic, not only the advertised listener.
- This does not reduce decoding work. Every active stream has its own decoder
  and process memory. Hardware acceleration follows the selected GStreamer
  decoder. Window composition, decoder limits and aggregate memory/CPU must be
  measured on the intended host before increasing concurrency.
- The implementation uses cross-platform GIO subprocess/socket APIs. The local
  build and automated tests were run on macOS, and two-phone simultaneous
  mirroring was reported working. Concurrent audio playback and Windows/Linux
  still need verification. PIN/password flows are not covered by synthetic tests.

## Automated checks

Transport tests need Python 3, IPv4/IPv6 loopback and permission to bind local
sockets. They launch a synthetic worker and do not advertise a receiver.

```sh
cmake -S . -B build -DBUILD_MULTIDEVICE_TESTS=ON
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

These cover same-peer reuse, different-peer isolation, concurrent binary
round trips, long-lived sockets, worker and per-worker connection limits,
backend EOF, explicit disconnect, idle cleanup, crash/reconnect, malformed IPC,
and worker cleanup after the parent dies. GLib warnings are fatal in these tests.

The optional real-receiver smoke test briefly advertises a unique name and
starts real UxPlay workers with `fakesink`. It needs working Bonjour/Avahi and
GStreamer. It checks RTSP `/info`, shared receiver keys/identity, independent
connections, reconnect, shutdown and the actual worker's parent-pipe EOF behavior.
It does not perform FairPlay pairing or
send real encrypted video.

```sh
python3 multidevice/tests/test_receiver.py build/uxplay
python3 multidevice/tests/test_receiver.py build/uxplay --single
python3 multidevice/tests/test_receiver.py build/single/uxplay --single
```

For memory-safety checks of the new supervisor:

```sh
cmake -S . -B build/asan -DBUILD_MULTIDEVICE_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/asan --target multidevice_test_harness -j 6
ctest --test-dir build/asan --output-on-failure
```

## Real-phone acceptance checklist

1. Mirror two phones to the same advertised destination; both must remain live
   in separate windows while each shows continuous motion.
2. Play different audio on both phones. Verify both are audible and each phone's
   volume/pause affects only its own stream. Stop/restart mirroring on one phone
   and rotate it; the other must keep playing. Repeat with `-as 0` to verify mute.
3. Disconnect one phone's network abruptly. Verify the other stream and eventual
   worker cleanup; then reconnect the first phone.
4. Connect beyond the configured limit; existing streams must survive. After
   releasing a slot, verify a new connection succeeds.
5. Quit the supervisor and check no worker/window remains. Repeat with an abrupt
   parent termination.
6. Check IPv4 and IPv6 LAN paths, then Windows/Linux. Measure CPU, memory, dropped
   frames and decoding/composition load at 2, 4 and 8 active phones.

Do not mark simultaneous mirroring or a particular fleet capacity as verified
until these hardware tests pass.

## Maintaining/reapplying the change

Starting point: upstream-derived commit `fd88c0c` (UxPlay 1.73.6). Most new logic
lives entirely in `multidevice/`; the existing renderers are unchanged.

The integration hooks to carry onto a newer upstream revision are:

1. Root CMake: optional module, target link/definition and opt-in tests.
2. `uxplay.cpp`: `-multi` parsing/help and validation; worker IPC initialization
   before logging; resolve receiver identity before the supervisor branch;
   initialize discovery without starting the parent's RAOP listener; workers
   use dynamic ports and skip discovery registration.
3. `uxplay.cpp` callbacks: forward admitted client metadata and video start/reset/
   last-control-disconnect transitions to the worker event channel.
4. `lib/raop.h` / `lib/raop.c`: optional original-peer override, set before the
   listener starts, with loopback-only acceptance for proxied control traffic.
5. Check the RAOP listener's return convention (negative means error; a positive
   value means newly started) and keep cleanup safe without a renderer logger.
6. Re-run normal and feature-disabled builds, transport tests, real RTSP smoke
   test and simultaneous phone mirroring. Compare DNS identity/public keys and
   confirm SETUP still returns directly reachable per-worker media ports.

For an upstream submission, keep the peer-override hook, supervisor module,
application integration and tests/docs as reviewable changes. Upstream acceptance
is not assumed; this first version explicitly trades process overhead for fewer
changes to established protocol and renderer code.
