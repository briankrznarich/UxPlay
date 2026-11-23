# ControlFloor provider with OCR identification

This branch combines the multi-device receiver with the existing ControlFloor
JPEG and keyboard/mouse adapter. Each AirPlay worker keeps its normal local
video/audio playback and runs an independent identification gate. No ControlFloor
phone connection is opened until that worker has read a keyboard marker and a
confirmed About-page identity.

## Build

The receiver's usual GStreamer, OpenSSL, libplist and service-discovery dependencies
still apply; see the root README. Additional OCR dependencies:

```sh
# macOS (English language data is included)
brew install tesseract pkg-config

# Debian/Ubuntu
sudo apt install libtesseract-dev libleptonica-dev tesseract-ocr-eng pkg-config
```

CMake finds Tesseract and Leptonica through `pkg-config`. `eng.traineddata` must
also be available at runtime. If installed in a custom location, set
`TESSDATA_PREFIX` to the directory containing it. Missing OCR support/data is an
error, not a reason to bypass identification. macOS and Linux are the current
ControlFloor targets; the file-lock/input integration has not been ported to Windows.

From the repository root:

```sh
cmake -S . -B build/controlfloor -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_CONTROLFLOOR=ON -DBUILD_CONTROLFLOOR_TESTS=ON \
  -DBUILD_MULTIDEVICE_TESTS=ON
cmake --build build/controlfloor -j 6
```

The first configuration downloads pinned IXWebSocket and nlohmann/json sources.
They are build dependencies, not files to commit. Running the binary does not
rebuild it. Do not copy a populated build directory with its nested `.git`
folders removed; use a fresh build directory after moving the checkout.

A receiver-only build needs neither OCR nor the ControlFloor networking libraries:

```sh
cmake -S . -B build/receiver -DENABLE_CONTROLFLOOR=OFF
cmake --build build/receiver -j 6
```

## Provider account

An existing provider credential JSON from the previous UxPlay checkout can be
used directly with `--controlfloor-config /absolute/path/to/provider.json`.
Do not add account credentials to Git. The default path, when no option is given,
is GLib's user config directory followed by `uxplay/controlfloor.json`.

If the server database was replaced, register a provider account again. This
explicit operation runs once in the launcher and exits; workers only read the
saved credentials. In bash:

```sh
read -r -p 'ControlFloor admin: ' CF_ADMIN_USERNAME
read -r -s -p 'Admin password: ' CF_ADMIN_PASSWORD; echo
export CF_ADMIN_USERNAME CF_ADMIN_PASSWORD
./build/controlfloor/uxplay --controlfloor-register \
  --controlfloor-server https://hoc.test:8080 --controlfloor-insecure
unset CF_ADMIN_USERNAME CF_ADMIN_PASSWORD
```

`CF_PROVIDER_USERNAME` optionally requests a provider username. Registration uses
`POST /login?noredirect=1` and `POST /admin/registerProvider?noredirect=1`; credentials
are written atomically with mode 0600. `--controlfloor-insecure` disables TLS
certificate/hostname verification and is intended for the self-signed test server.
It must also be present when subsequently running against that server.

## Run and manually identify phones

```sh
./build/controlfloor/uxplay -multi 4 \
  --controlfloor-provider --controlfloor-insecure \
  --controlfloor-mouse \
  -n 'ControlFloor AirPlay' -nh -vsync no
```

Add `--controlfloor-config FILE` if using a nondefault credential file. `-multi 4`
advertises one receiver and permits four AirPlay peers. Omit `-multi` for a single
receiver. Keep `-vsync no`: it was necessary for smooth playback on the development
Mac. ControlFloor mode requires decoded mirroring video, not HLS, RTP forwarding,
recording mode or disabled video. Local audio remains enabled; remote audio is
not implemented.

For each phone:

1. Start Screen Mirroring to the advertised receiver. Its normal local video
   window should appear immediately; it is not yet advertised to ControlFloor.
2. Manually display a complete marker such as `X=4523452345234=X` in a Shortcuts
   text dialog. Leave it visible until the log reports `keyboard-identified`.
   Accepted IDs start with an ASCII letter/digit and contain at most 128 ASCII
   letters, digits, colons, underscores or hyphens. Both `X` delimiters and both
   `=` signs are required. Conflicting markers on one screen are rejected.
3. Open **Settings → General → About** and leave the name, model name and serial
   number visible together. The PoC expects the English labels. It also reads
   model number and iOS version when present.
4. The first complete read logs `identity-candidate`; a second scan agreeing on
   serial, name and model name logs `device-identified`. The worker then connects
   to ControlFloor as **`airplay-<serialNumber>`**, with the OCR name as its display
   name. The original AirPlay model identifier remains its `deviceType`; additional
   About fields and `keyboardId` are included in device information.
5. Open the ControlFloor viewer and start video. JPEG forwarding is controlled by
   the viewer independently of local mirroring. Enable live mouse in the viewer
   to test the existing input path.

Scans run roughly five seconds apart (recognition time is additional). Allow
about 5–15 seconds per stage. `--controlfloor-ocr-interval N` changes the delay,
from 0.2 to 60 seconds; the small values are useful for automated tests. A new
frame must arrive after the keyboard scan before About parsing begins. A static
About frame may be read twice; this is recognition consistency, not independent
proof of hardware ownership. OCR stops after successful identification.

During identification, typing, navigation, shortcut execution and discovery of
physical keyboard bindings still require manual setup. Remote input becomes
available after admission, as described below. A failed/ambiguous OCR read leaves local
playback working and keeps the phone out of ControlFloor. Serial numbers must be
8–16 uppercase ASCII letters/digits; ambiguous characters are never silently
substituted. If a mistaken ID is accepted, disconnect, correct the displayed
screen, and reconnect to repeat identification.

## Logs and connection lifetime

Logs default to `./log/`, or `--controlfloor-log-dir DIR`. Each connection gets a
private (0600), flushed JSON-lines file:

```text
log/22_70_9B_0E_36_5D-<timestamp>-<unique suffix>.jsonl
```

A connection initially has a `pending-...` filename because the AirPlay hardware
ID is not available until SETUP. It is renamed when that ID arrives, retaining
its first events. Unsafe filename characters become underscores. A connection
that never reaches SETUP retains its pending filename.

```sh
tail -f log/*.jsonl
```

Events include `connection-open`, `airplay-client`, `ocr` (text and recognition
time), `keyboard-identified`, `identity-candidate`, `device-identified`,
`controlfloor-admitted`, provider connection/errors, and `connection-closed`.
OCR text and identifiers are diagnostic data; logs are ignored by Git and no
screenshots are saved. Log rotation is not implemented yet.

AirPlay disconnect/replacement revokes admission, drops queued frames/input,
closes the device WebSocket and requires identification on the next connection.
A server-only reconnect preserves identification for the same AirPlay connection.
Failed login/upgrade attempts retry on a ten-second schedule. Connection loss
returns to the same retry loop. Invalid credentials remain an error: workers do not register a
replacement account, edit the credential file or repeat OCR to repair it.

`controlfloor-admitted` means the local OCR gate has allowed a connection attempt;
it is not an acknowledgment from the server. A WebSocket upgrade is logged as
`provider WebSocket transport open; awaiting ControlFloor commands`. The first
server command produces `ControlFloor protocol active`; this confirms protocol
traffic, not visibility in a particular user's device list. WebSocket failures
log the HTTP status, and closes log the close code and which side closed it.
Structured events include `provider-connect-attempt`, `provider-connect-failed`,
`provider-websocket-error` and `provider-websocket-close`.

The server sends application pings every 20 seconds. If no server command arrives
for 60 seconds, the worker logs `provider-heartbeat-timeout` and reconnects with
the retained identity. Transport-level pongs alone do not keep a stalled
ControlFloor command loop alive. This watchdog also runs when video forwarding
is off.
Connection generations reject late OCR results and prevent old queued input from
being dispatched as part of a replacement connection. Already-issued helper
requests cannot be undone, and uncertain text requests are not retried.

Workers using the same log directory take OS file locks for serial uniqueness.
A second worker with the same serial waits instead of replacing the first
provider connection. Locks are released when a worker disconnects or exits;
the `.lease-*` files can remain on disk and are not themselves evidence of a
held lock. Use a common log directory for workers/instances that should coordinate.

## Input: deliberately still a one-helper PoC

`--controlfloor-mouse` uses `http://127.0.0.1:7390`.
`--controlfloor-mouse-url URL` enables input at another iphone-use endpoint.
The adapter requires exactly one Bluetooth device in that helper's inventory and
uses its current profile without calibration. Supported commands are
`enableLiveMouse`, `disableLiveMouse`, `mouseDown`, `mouseMove` (including a path),
`mouseUp`, `typeText`, `tapText`, `typeSpecialKey`, `pressKeyChord`, and
`executeKeyboardShortcut`. `getKeyboardKeys` and `getKeyboardShortcuts` expose
the iphone-use catalogs through the normal ControlFloor response envelopes. The
frontend expresses ordinary taps/swipes through the live-mouse commands; native CFAgent tap/swipe and multitouch APIs are
not emulated.

Mouse `button` accepts integers 1–8, matching iphone-use's HID report: 1 is left,
2 right, 3 middle, and 4–8 are additional buttons (including shortcut triggers).
Button-only down/up calls preserve the pointer position.

`typeSpecialKey` accepts the same `{ "key": "Backspace" }` arguments as cfprovider.
The modern iphone-use endpoint owns all mappings and aliases: Backspace, forward
Delete, Enter/Tab/Escape, arrows, Home/End/PageUp/PageDown, F1–F12, ordinary keys,
modifiers, numeric keypad, Fn/Globe, consumer/media keys and its virtual recording
commands. Unknown names return the helper's error. The legacy `/key` endpoint is
not used because it treats `delete` as Backspace. These are physical HID key
submissions; individual iOS/app behavior (especially function/media keys) depends
on the target phone. Pressing a modifier alone does not hold it for a later call;
use `pressKeyChord` for combinations.

The frontend's existing `CFAgent`/`Bluetooth` routing hints work with this bridge,
as they already do for text: there is only one hardware backend here. No native
CFAgent connection is implied. Special keys need a keyboard or consumer-control
subscription, not live mouse/AssistiveTouch. The helper checks the specific HID
reports required by each key/chord before emitting input. Read-only catalog calls
do not require subscriptions. They require the configured helper adapter.

Key/chord/shortcut calls use the same serialized, bounded input queue as mouse
and text. Only validated key/id/revision fields are forwarded; the provider selects
the device. A helper error/timeout is returned once, without automatic replay.
Catalog responses and shortcut revisions pass through unchanged (with the
`Bluetooth` agent label added to discovery), so the existing keyboard menu can
use them without a frontend change. An older iphone-use build without these
endpoints must be updated; there is no lossy fallback to typing control characters.

### Navigation and hardware buttons

Existing ControlFloor commands are translated locally; no frontend change,
Shortcuts callback server or USB/CFAgent connection is required:

| ControlFloor call | Preferred input |
| --- | --- |
| `simulateHomeButton`, `showHomeScreen`, `performGesture: ShowHomeScreen` | `GoHome` consumer HID; otherwise the `home` keyboard shortcut |
| `showControlCenter`, `performGesture: ShowControlCenter` | `controlCenter` shortcut |
| `showAppSwitcher`, `performGesture: ShowAppSwitcher` | `appSwitcher` shortcut |
| `showNotificationCenter`, `performGesture: ShowNotificationCenter` | `notificationCenter` shortcut |
| `simulateLockButton`, `performGesture: LockScreen` | `lockScreen` shortcut |
| `performGesture: AskSiri`, `AccessibilityShortcut` | `siri`, `accessibilityShortcut` shortcuts |
| `simulateVolumeUpButton`, `simulateVolumeDownButton` | `AudioVolumeUp`, `AudioVolumeDown` consumer HID |

For example, `performGesture` takes `{ "gesture": "ShowControlCenter" }`.
Named keyboard shortcuts are resolved from iphone-use's current catalog and
submitted with its revision, so configured overrides apply. Its shipped system
bindings use Fn/Globe (Control Center: Fn+C, App Switcher: Fn+Up); other bindings
such as Lock Screen require Full Keyboard Access. The phone must be configured
for the selected binding; a successful response means input was submitted, not
that the resulting screen was visually verified.

When a binding is missing/unassigned or its HID reports are unavailable, system
navigation can fall back to a mouse drag **before any keyboard submission**.
A disabled binding is an explicit error. Catalog/POST failures are errors, never
an excuse to send a second action: a failed network request may already have
operated the phone. Home/volume use the consumer report without requiring the
ordinary keyboard report. There is no held-home-button emulation (`duration`
other than zero is rejected).

Mouse fallback uses the OCR model from the provider, never a viewer-supplied
model. SE and older named iPhones through iPhone 8 use a bottom-up Control Center
swipe; newer iPhones/iPads use top-right-down. Home and App Switcher swipes are
only used for the latter layout. SE App Switcher requires its keyboard binding.
An unknown model has no model-dependent system swipe fallback. Notification
Center can use a top-center-down swipe.

`performGesture` also supports `CenterSwipeUp/Down/Left/Right`,
`EdgeSwipeUp/Down/Left/Right`, `PageUp`, `PageDown`, and `JiggleScreenContents`.
The eight center/edge gestures also accept their direct cfprovider action names
(`centerSwipeUp`, etc.). Page gestures optionally accept a logical `x`/`y` origin;
otherwise they start at screen center. These follow CFA's useful single-pointer
paths with small in-screen edge offsets. Timing is approximate because helper
HTTP latency and Bluetooth pacing add to it. Mouse profile accuracy and iOS
recognition of pointer edge gestures still need hardware verification.

Each drag owns its down/move/up sequence on the input worker, without requiring
`enableLiveMouse` or interleaving another input call. It uses the helper profile's
logical dimensions and the stream's orientation, releases on failure, and stops
remaining steps on session disconnect. Video and CF ping run independently.
Replies include `gesture` and `inputMethod` (`bluetoothKeyboard` or
`bluetoothMouse`). Unsupported gestures (including CFA placeholder names and
multitouch) return errors. Gesture translation lives in `navigation.cpp`; hardware
I/O and cancellation remain in `mouse_input.cpp`.

The recognized keyboard marker is recorded, **not yet used to select hardware**.
The first identified worker to acquire the configured helper's file lock gets
input; other workers remain video-only. This prevents two viewers from driving
the same helper concurrently, but cannot establish that the helper actually
controls the displayed phone. For input testing, use one connected phone. There
is no automatic input reassignment to an already admitted video-only worker;
reconnect that phone once the helper is free. Configure a consistent helper URL
and log directory across workers; URL aliases/different directories are not a
global hardware registry.

Text is validated in full before dispatch: printable US ASCII, tab and LF newline,
up to 1024 characters. Emoji, accented characters, unsupported controls and
invalid input types produce an error without typing a prefix. Helper/network
failure after dispatch may still mean partial physical delivery.

## ControlFloor protocol retained from the original PoC

Each identified worker logs in with the shared provider account and opens its own
`/provider/ws?udid=airplay-...` connection. It implements ping, device information,
status, capabilities, orientation, video configuration/status/debug, start/stop
video, screenshots, transport reset, frame ACKs and the input commands above.
Unsupported native-iOS/CFAgent, multitouch and audio operations return explicit
errors; `stopAudio` is harmless. Capability replies retain the nested envelope
required by the existing frontend. No runner-status emulation is included.

The existing JPEG sizing, quality, frame-rate/bandwidth, burst and configuration
validation behavior is preserved. JPEG headers and binary payloads share one
network writer. ACK flow control uses a bounded two-frame window with recovery
probes, rejects stale generation ACKs, and bounds the WebSocket output backlog.
Slow clients cannot accumulate an unbounded history of frames.

## Implementation boundaries

- `uxplay.cpp`: optional lifecycle/options hooks; the multi-device supervisor is
  unchanged and remains responsible for one advertised destination and worker
  processes. Registration/preflight happen before it starts accepting peers.
- `renderers/video_renderer.c`: a small guarded tee after decoding/conversion,
  before the local sink. A leaky one-frame queue feeds an appsink.
- `controlfloor.cpp`: existing provider protocol/encoder, now with distinct
  AirPlay-connected and identified/admitted states. Globals remain process-local
  because each worker owns only one phone; this is not a multi-phone singleton
  inside the supervisor. It retains one latest decoded sample.
- `identity.h/.cpp`: Tesseract reading, row reconstruction, marker/About parsing,
  per-connection logs and the staged identity gate. It owns its OCR thread and
  returns typed identity results, independently of server registration.
- `mouse_input.h/.cpp`: existing asynchronous HTTP hardware adapter and bounded
  command queue. Future hardware backends belong behind this boundary.
- `test_source.cpp` and `tests/`: explicit synthetic source and headless tests.

The frame callback only records caps and replaces a sample reference with a
try-lock. RGB conversion/readback and OCR run on the OCR thread; JPEG encoding
runs separately. Both consumers share the existing decode. No JPEG/network/ACK
work or OCR is performed in the AirPlay callback. The supervisor never relays
pixels. Private IPC for coordinator-driven pairing, shortcut callbacks and a
real keyboard registry remain future work; see the development guide.

## Tests

Install Python dependencies in a virtual environment, then use the build above:

```sh
python3 -m venv build/test-venv
build/test-venv/bin/pip install aiohttp Pillow
ctest --test-dir build/controlfloor --output-on-failure
build/test-venv/bin/python controlfloor/tests/integration.py build/controlfloor/uxplay
build/test-venv/bin/python controlfloor/tests/mouse.py build/controlfloor/uxplay
build/test-venv/bin/python controlfloor/tests/keyboard.py build/controlfloor/uxplay
build/test-venv/bin/python controlfloor/tests/navigation.py build/controlfloor/uxplay
# About two minutes; exercises the real retry and heartbeat deadlines.
build/test-venv/bin/python controlfloor/tests/reconnect.py build/controlfloor/uxplay
build/test-venv/bin/python controlfloor/tests/identification.py \
  build/controlfloor/controlfloor/controlfloor_ocr_harness
```

The tests use localhost mock servers, not a real ControlFloor database or input
hardware. Identification fixtures use generated fake IDs. To test real screenshots
without checking them into the repository:

```sh
build/test-venv/bin/python controlfloor/tests/identification.py \
  build/controlfloor/controlfloor/controlfloor_ocr_harness keyboard.png about.png
build/controlfloor/controlfloor/controlfloor_ocr_harness --ocr-file about.png
```

`CF_TEST_H264=1`, `CF_TEST_GL=1`, or `CF_TEST_STATIC=1` exercise alternate protocol
test sources. `CF_TEST_GL=1` also exercises GPU-backed frame readback in the OCR
harness on systems with GStreamer GL support. `--controlfloor-test-source` is an
explicit test-only dummy identity path; it advertises no AirPlay listener and is
rejected with `-multi`. Real AirPlay clients cannot use that bypass.

The receiver smoke tests in `multidevice/tests/test_receiver.py` still apply.
`python3 controlfloor/tests/receiver.py build/controlfloor/uxplay` runs them with
ControlFloor enabled, checking worker startup, parent-pipe EOF cleanup and logs.
Provider cleanup is registered before its threads start because the receiver's
parent monitor exits directly when the supervisor disappears.
Live verification remains necessary: two phones, local video/audio, manual
keyboard/About identification, distinct server devices, orientation changes,
viewer video control, and disconnect/reconnect. Mock tests do not validate
AirPlay encryption, phone UI behavior or real-time playback performance.
