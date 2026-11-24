# LAN shortcut pairing and startup

This mode adds automatic keyboard → IPv4 → AirPlay → serial-number binding to
UxPlay's ControlFloor branch. It is opt-in; the existing manual marker/About OCR
workflow remains available without `--controlfloor-lan`.

## Requirements and first run

- An **IPv4-only LAN**, one phone per source IPv4 address. Both the shortcut and
  AirPlay must reach this Mac from the same phone address, without a proxy/NAT
  collapsing multiple phones onto one address. No IPv6 matching is attempted.
- Import `ControlFloorLAN.shortcut` and bind **Mouse 8** to it. Complete any
  one-time iOS permission/setup prompts. The current 0.4.19 artifact targets
  `http://Brians-MacBook-Pro-2.local:9876` and reports the baked-in model `iPhone SE`.
  `.local` name resolution and incoming TCP port 9876 must work from the phone.
  This HTTP hostname is independent of the AirPlay receiver's display name.
- Run iphone-use with keyboard, consumer controls and positioned mouse support.
  Each connected `bluetoothHost` is independently addressable; several phones in
  its inventory are supported. No USB/go-ios/device development mode is used.
- Save a replay **for each Bluetooth host** in iphone-use. The recording must:
  1. Open Control Center.
  2. Start Screen Mirroring to this receiver.
  3. Dismiss the mirroring panel and Control Center.
  4. **Finish by pressing Mouse 8 to run ControlFloorLAN again.**

Do not put token typing, About navigation, or Home in the recording. The server
handles those. It does **not** press Mouse 8 again after replay; this avoids
launching the shortcut twice. A recording without the final shortcut trigger
will time out explicitly. Recording selection is iphone-use's existing per-host
`replayCapture`, not yet an OS/model-based script library.

Build and launch from the repository root (adjust the credential path as needed):

```sh
cmake --build build/controlfloor -j 8
./build/controlfloor/uxplay -multi 4 \
  --controlfloor-lan \
  --controlfloor-config /Users/briank/git/UxPlay/build-cf/test-provider.json \
  --controlfloor-insecure \
  --controlfloor-shortcut-file /Users/briank/git/cfa/shortcuts/build/ControlFloorLAN.shortcut \
  -n "ControlFloor AirPlay" -nh -vsync no
```

`--controlfloor-lan` enables the provider, automatic pairing, and the default
helper `http://127.0.0.1:7390`. Override the helper with
`--controlfloor-mouse-url URL`. It requires `-multi`, even for one phone.
`--controlfloor-shortcut-port N` overrides 9876; rebuild the shortcut's baked-in
URL to match. `--controlfloor-shortcut-file FILE` is optional; when supplied,
`GET /shortcuts/ControlFloorLAN.shortcut` downloads those exact signed bytes.
UxPlay does not compile or sign shortcuts.

`--controlfloor-insecure` applies to the test ControlFloor server's certificate;
the LAN callback service itself is plain HTTP. Provider registration/credentials
are unchanged. Do not run a legacy sole-phone provider against the same helper
concurrently; file leases reject competing owners when they share a log directory.

Before trying the phone, open `http://Brians-MacBook-Pro-2.local:9876/` for the
keyboard dashboard. `curl http://127.0.0.1:9876/health` checks JSON health.
Test the root URL from the phone to check reachability without
consuming a pairing command. **Do not use `/shortcuts/next` as a health check:**
it fetches real work.

## What happens under the hood

The parent process owns one IPv4 HTTP listener, the keyboard registry and the
pairing coordinator. It polls iphone-use's inventory approximately once a second.
Each keyboard has its own serialized input worker; one slow keyboard does not
hold up another keyboard or video decoding. The helper's currently selected
mouse profile is used without calibration.

1. **Discover and trigger.** Every available, unpaired keyboard with auto-connect
   enabled gets a fresh pairing token such as `01-17-45-07` (four zero-padded
   `00`–`99` groups). The coordinator sends a button-only Mouse 8 click, waits
   two seconds, types the token without any `X=` delimiters, then sends Enter.
   Button-only clicks preserve the pointer position. The keyboard hardware ID is only the input
   target; it is not typed into the phone.
2. **Fetch the initial command.** The stateless shortcut makes one
   `GET /shortcuts/next`. An unbound IPv4 address receives
   `{"command":"initiate-pairing","requestId":"..."}`. Its input prompt
   receives the coordinator's keystrokes.
3. **Bind keyboard to IPv4.** The shortcut POSTs the exact entered `pairingId`,
   its baked-in `model`, computed `iosVersion`, and the common report envelope.
   The server requires the current token, request ID and originating IPv4 address.
   Callback arrival order does not select the keyboard: the typed token does.
   Startup retries stop for that keyboard. Unknown, expired or reused attempts
   cannot establish a binding.
4. **Queue About, then replay.** The server queues `prefs-general-about` with a
   fresh request ID **before** invoking iphone-use's `replayCapture`. That helper
   call only starts playback; it is not evidence that mirroring is ready.
5. **Match the AirPlay stream.** A receiver worker reports an actual connected
   client and its source IPv4 address. The parent joins it to the keyboard's
   already-confirmed address. Ordinary additional AirPlay TCP connections are
   part of the same session, not replacements. The decoder's local video window
   continues to work throughout pairing.
6. **Replay triggers the next shortcut.** Its final Mouse 8 fetches the queued
   About command. If this fetch races the worker's connection event, HTTP waits
   up to ten seconds for confirmation. No About command is issued to an
   unconfirmed stream. The shortcut opens About, POSTs its result, and exits.
7. **Read hardware identity.** Only after the About callback **and** a fresh helper
   status confirming replay completion does the parent enable About OCR. The
   worker reads name/model/serial, requiring two agreeing reads. LAN mode skips
   the second token-display/OCR step: the typed callback proves keyboard → IP,
   and the IPv4 connection proves IP → video. The original token-OCR gate remains
   unchanged for manual mode.
8. **Go live.** The parent sends Home through the matched keyboard. Once that HID
   submission succeeds it authorizes the worker to register `airplay-<serial>`
   with ControlFloor. Home's resulting screen is not visually verified. Duplicate
   serial numbers are rejected. The reported `keyboardId` is now the actual
   helper host ID, not the temporary pairing token.

No session ID is stored or supplied by Shortcuts. Internal worker IDs and
connection revisions guard private process messages; they never become shortcut
URL parameters. Every new pairing attempt gets a fresh token. `requestId`
correlates individual commands and callbacks. Codes use cryptographically random,
uniform digits (100 million combinations), with duplicates rejected among known
keyboard tokens. The shortcut must echo `pairingId` as text, preserving all zeroes
and hyphens. Numeric values, missing separators and the old `X=...=X` wrapper are
rejected. This is pairing correlation on the trusted LAN, not authentication:
the dashboard deliberately exposes active codes to its operators.

## Callback protocol and further commands

- `GET /shortcuts/next`: return one queued JSON command. The command is consumed
  once; additional invocations while it is outstanding return
  `{"command":"ping","requestId":""}`. HEAD does not consume work.
- `POST /shortcuts/report`: require an object containing `protocolVersion: 2`,
  `shortcutVersion`, `command`, `requestId`, and `status` (`ok`, `error`, or
  `progress`). Terminal reports are correlated by source IPv4, request ID and
  command. Recent duplicate terminal reports are acknowledged without repeating
  input. Progress does not complete a command.
- `GET /`: human-accessible keyboard dashboard.
- `GET /health`: small JSON health response; no keyboard activity or queue mutation.
- `GET /api/keyboards`: keyboard snapshot and dashboard mutation token.
- `POST /api/keyboards/control`: dashboard controls (see below).
- `GET /shortcuts/ControlFloorLAN.shortcut`: optional artifact download.

Use the existing ControlFloor `runShortcut` API with
`{"command":"prefs-general-about"}` (or another installed shortcut command).
Existing actions with `agent: "Shortcuts"` are mapped to their shortcut command
names for clipboard, URLs, device/network/battery information and common settings.
Dedicated shortcut actions such as `getShortcutScriptVersion`, `setBrightness`,
`setVolume`, `getBatteryStatus`, `getNetworkState`, `restartNetwork`,
`prefsGeneralAbout` and `getIOSMajorVersion` also work without an agent override.
Brightness/volume `percent` arguments are translated to the shortcut's `value`,
and media download destinations are mapped to the corresponding command.
The shortcut itself checks command-specific parameters and returns any errors;
this is not a complete port of cfprovider's argument validation or settings-state
notices. Unsupported shortcut commands return the installed shortcut's error.
Pairing commands are reserved and cannot be injected through `runShortcut`.

The coordinator queues work before pressing Mouse 8, waits for the POST, and
returns a routed ControlFloor reply preserving correlation fields. It passes
progress notices separately. During a shortcut invocation, further UI input is
rejected as busy; video, screenshots and normal provider pings continue. No
background shortcut polling loop is required. Network-disabling commands can
naturally sever AirPlay and cause rediscovery.

Remote mouse/keyboard commands also travel over the private worker pipe to the
parent's matched input adapter. A viewer cannot choose a different helper target.
Input messages/results are bounded to fit a 4 KiB worker message; oversized
requests/results return an error. Video stays entirely in the receiver worker
and is never copied through this coordinator.

## Timeouts, disconnects and logs

Initial callbacks have a 30-second window. Failed attempts back off for 30
seconds before another Mouse 8/type/Enter sequence, using a new token. Pairing
stages after the initial callback have a 120-second deadline. UI timing and these
values currently live in `PairingOptions` (`pairing.h`); tests shorten them.

A missing/empty recording, replay error, vanished keyboard, callback timeout,
OCR timeout or changed AirPlay session revokes the binding and prevents CF
admission. Active replay is cancelled best-effort through `endCapture`, and the
adapter releases held mouse buttons. A startup/retry will not start while helper status
reports replaying or recording. Reconnect optimization is intentionally deferred: every
new AirPlay session requires discovery again. A ControlFloor WebSocket reconnect
alone retains the physical binding and resets pending remote input.

Automatic input is intentional: an unpaired keyboard may type the token into
whatever UI currently has focus if its Mouse 8 binding/setup is wrong. Use the
dashboard/logs to diagnose this. Turn auto-connect off for a keyboard before
configuring its recording. New keyboards default to auto-connect ON.
The server is intended for the trusted device LAN; it does not add authentication
or TLS to the existing shortcut protocol.

Logs begin before AirPlay under:

```text
log/keyboard-<sanitized-helper-host>.jsonl
```

One file per keyboard appends across retries and server restarts; the pairing code
is stored in each event rather than the filename. Dashboard actions are logged
even before the first pairing attempt. Previous session-named files are left intact.
These logs record startup, callback/IP binding, replay start/completion, worker binding,
About activation, identified serial, live admission and failure reasons. Existing
AirPlay/OCR connection logs remain alongside them. The initial callback metadata
is logged; About OCR supplies the permanent identity. Preferences and last-known
bindings persist in SQLite as described below. Live pairing authority does not.

## Incoming HTTP diagnostics

All requests delivered to the LAN HTTP handler are appended to `log/http.jsonl`
(or the directory selected by `--controlfloor-log-dir`). Logging is always on in
LAN mode, including dashboard polling, health checks, wrong URLs, unsupported
methods, rejected callbacks and malformed JSON. No keyboard binding is required.

An `http-request` line is flushed **before** routing/validation, with timestamp,
source IP/port, method, full URI including query parameters, headers and the
complete incoming body. `bodyEncoding` is `utf-8`, or `base64` for non-text bytes;
`bodyBytes` records the original size. No incoming body is truncated. An
`http-response` line uses the same `httpId` and records the returned status and
handler duration. Command/error response bodies are included; HTML, dashboard
snapshot and shortcut-download response bodies are omitted to avoid duplication.
The response entry records what the handler returned, not proof of phone receipt.

```sh
tail -f log/http.jsonl
tail -f log/keyboard-<sanitized-helper-host>.jsonl
```

Files append across restarts and are created with owner-only permissions. They
contain full request data (including callback text and header tokens) and have no
automatic rotation. `http-server-starting` marks coordinator starts. HTTP framing
errors, incomplete requests, or TLS sent to the plain HTTP port can be rejected
by the underlying HTTP library before the handler sees a request; these are not
packet-capture logs. This log covers the LAN web server, not AirPlay RTSP traffic.

## Keyboard dashboard and saved state

The dashboard is served by the same listener at `/` (by default,
`http://Brians-MacBook-Pro-2.local:9876/` or `http://127.0.0.1:9876/`). No separate
web server, JavaScript packages, CDN, or asset directory is required. The HTML
is embedded at build time. Both inventory and browser snapshots poll about once
a second. Search matches hardware ID, friendly label, IP and device identity.
Known but absent keyboards remain listed as offline. The compact table keeps
Auto-connect in the first column and exposes ID, IP, code, serial, model, iOS,
AirPlay, input/capture, ControlFloor, status and actions in one row. The action
selector executes immediately and returns to its placeholder; there is no Run
button. Options and recent events expand underneath the same row. Tables scroll
horizontally on narrow screens and the inventory has a bounded-height viewport
with sticky headings for large fleets. Expanded options and unsaved label edits
survive polling.

Above the keyboard inventory, **Unpaired AirPlay** lists connected receiver
workers whose source IP has no current keyboard binding. It shows IP, advertised
client name/ID/model (when supplied), and the time the coordinator first observed
that AirPlay connection. Periodic status updates do not reset that time. A saved
IP alone does not count as a current binding. Disconnected or revoked workers
are omitted; streams disappear from the table once matched. This view is part of
the keyboard page and the existing `/api/keyboards` snapshot (`unpairedAirplay`).

Each keyboard shows its phase, current/remembered IP, pairing token, last-read
identity, AirPlay/server state, helper capture state and recent coordinator events.
“Connected to ControlFloor” requires the worker's live provider WebSocket **and a
server confirmation**; admission and an open socket alone are not enough. These
are the most recently reported states, not an independent server health probe.
The 20 recent events are session-local; the per-keyboard log appends across attempts
and server restarts. Every event includes its pairing code for correlation.

| Control | Behavior |
| --- | --- |
| Auto-connect ON | Start/retry discovery automatically when input is ready; new keyboards default ON. |
| Auto-connect OFF | Stop the attempt, revoke the active provider/AirPlay worker, cancel replay best-effort, preserve the remembered IP/identity, and allow manual actions. |
| Mouse 8 | Send one atomic button-only click to this keyboard. Do not type a token or start the rest of pairing. |
| Pair Keyboard | Pause background startup, queue/initiate the shortcut prompt, type a fresh token and Enter. **Stop after the callback binds the phone IP.** |
| Start Screen Mirroring | Queue About, run the per-host replay, match AirPlay, read identity, send Home and admit to ControlFloor. |
| Resume auto-connect | Enable automatic startup again; if already at the manual paired stage, continue into mirroring. |
| Reset & pause | Stop the attempt/connection, forget the saved IP and identity, clear the token, and turn auto-connect OFF. |
| Friendly label | Save a human-readable label (up to 80 UTF-8 bytes), independent of hardware identity. |

Manual actions pause background startup for this process lifetime, even if the
checkbox remains ON; the card explicitly reports this pause. Resume auto-connect
clears it. Manual Pair Keyboard does not automatically change the saved checkbox.
Commands that send input are rejected when the keyboard is offline, busy, recording
or replaying. Reset/auto-connect OFF can interrupt an attempt. A request already
submitted to the helper cannot be undone; subsequent input waits for it to finish.
The checkbox suppresses automation; a paused keyboard is still inventoried and
available for manual troubleshooting.

SQLite stores preferences, labels, last-known IP, last-read identity and last-seen
time in `log/keyboards.sqlite3` (under `--controlfloor-log-dir` if overridden).
Use `--controlfloor-keyboard-db FILE` to choose another database location; its
parent directory must exist. Records are scoped by helper URL and hardware ID,
so changing helper URLs creates a separate source inventory. Do not commit the
database: it contains device identifiers. SQLite files and journals are ignored
by Git. SQLite's development library is needed at build time: macOS's SDK provides
it; on Debian/Ubuntu install `libsqlite3-dev` alongside the existing dependencies.

On restart, tokens, request IDs, active IP bindings and worker sessions are discarded.
Saved bindings appear as **remembered**, never live. Auto-connect ON still performs
fresh discovery. With auto-connect OFF, an explicit Start Screen Mirroring may
resume a remembered IP; it re-reads About and rejects a different serial if one
was previously recorded. This manual shortcut assumes your same-phone/static-IP
setup; use Pair Keyboard when the physical keyboard/phone relationship may have
changed. Reset & pause durably deletes the remembered association.

Dashboard POSTs require `X-UxPlay-Token` from the current snapshot and reject
cross-origin browser requests. This prevents casual cross-site form/fetch commands;
it is **not a login system**. The service is for the same trusted IPv4 device LAN
as the shortcut callbacks. Anyone who can access it can operate its controls.

Example control body: `{"id":"<bluetoothHost>","action":"pair"}`. Actions are
`pair`, `mirror`, `mouse8`, `reset`, `resume`, and `settings`; settings accepts
`autoConnect` and/or `label`. A `202` acknowledges queued work, not completion;
read the snapshot/events for the outcome. Conflicting input returns `409`.

## Code boundaries and tests

- `pairing.cpp/.h`: HTTP routes, per-keyboard state, command/report correlation,
  replay tracking and logs.
- `dashboard.html`: dependency-free browser UI embedded by CMake.
- `keyboard_store.cpp/.h`: SQLite persistence, separate from live pairing state.
- `mouse_input.cpp`: existing serialized input adapter, now optionally targeting
  an explicit host and supporting atomic button-only `clickMouseButton`.
- `identity.cpp`: managed About gate in LAN mode; manual marker mode unchanged.
- `controlfloor.cpp`: worker-state reporting, admission gate, remote input/replies.
- `multidevice/`: small generic bounded application-message pipes and optional
  IPv4-only listener. No JSON, shortcut, keyboard or OCR logic is added there.
- `uxplay.cpp`: lifecycle hooks connecting the coordinator to the supervisor.

With `BUILD_CONTROLFLOOR_TESTS=ON` and the existing test virtual environment:

```sh
build/test-venv/bin/python controlfloor/tests/dashboard.py build/controlfloor/controlfloor/controlfloor_pairing_harness
build/test-venv/bin/python controlfloor/tests/pairing.py build/controlfloor/controlfloor/controlfloor_pairing_harness
build/test-venv/bin/python controlfloor/tests/lan_worker.py build/controlfloor/controlfloor/controlfloor_ocr_harness
ctest --test-dir build/controlfloor --output-on-failure
```

The dashboard and pairing tests use mock HTTP/HID/worker events, including
restart persistence and manual partial execution. The worker test uses real Tesseract,
decoded frames and the provider WebSocket against generated images and a mock
ControlFloor server. Neither sends input to live phones. Real shortcut permissions,
replay navigation, LAN reachability and successful Home navigation still require
hardware verification.
