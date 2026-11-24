# LAN web mouse calibration

LAN startup automatically runs mouse detection/calibration **after About OCR has
identified the hardware and before ControlFloor admission**. Mouse 8 invokes the
LAN shortcut's `open-url` command to launch the page. The calibration itself uses
no inspector app, developer mode, USB connection, OCR, or AirPlay video measurement.
The phone needs portrait Safari, AssistiveTouch mouse input, and the installed
ControlFloorLAN shortcut bound to Mouse 8. Leave the phone untouched during a run.

## Running it

1. Build/restart UxPlay with the usual `--controlfloor-lan` arguments.
2. Normal startup proceeds from hardware identification into calibration automatically.
   An existing profile is detected/validated when possible; otherwise a full curve
   is generated. Only a successful commit can allow ControlFloor admission.
3. To test manually, pair the keyboard so its current phone IP is known, then in
   that keyboard's **Action…** menu choose **Detect / calibrate mouse**, or
   **Calibrate mouse — new profile** to skip matching and measure a fresh curve.
4. UxPlay queues `open-url` with a session-specific HTTP URL, then clicks Mouse 8.
   The shortcut opens Safari. The page waits for a stable viewport and completion
   of Mouse 8, then starts automatically.
5. Watch the page and keyboard status. During startup, successful calibration is
   followed by a one-second result-display grace period, Home, and ControlFloor
   registration. A standalone manual run before hardware identification stays on
   its result page. **Cancel mouse calibration** stops it from the dashboard.

Cancellation, failure, or timeout during startup leaves **Mouse calibration
required**, retaining the IP/hardware binding but keeping ControlFloor admission
closed. Retry with **Detect / calibrate mouse**, **Calibrate mouse — new profile**,
or **Resume auto-connect**. It does not automatically spin on a failed calibration.
The dropdown and server both reject calibration once ControlFloor is admitted,
connecting, connected, or reconnecting. Reset/pause that connection before manual
recalibration. Previously opened calibration pages cannot issue further mutations
after completion or while ControlFloor is active.

“New profile” uses the driver's force policy: in current iphone-use it may replace
an existing generated profile file with the same canonical name, **only after
validation and commit**. Ordinary detection/generation uses normal catalog reuse
and naming. Failed/cancelled runs do not activate a partially measured profile.

The open URL uses the **actual local IPv4 destination of the phone's most recent
shortcut request**, captured with `getsockname()` on the accepted TCP socket. It
never uses the dashboard browser's Host header, a guessed Wi-Fi IP, DNS resolution,
or the default route. This matters when Wi-Fi and USB networking coexist. A small
additive patch to the pinned IXWebSocket dependency exposes that destination;
`cmake/ix_local_address.cmake` applies it on configuration and checks its anchors.
The phone's source IPv4 still must match its current keyboard binding. If the
phone changes interfaces, pair again using the current path.

## Measurement and profile selection

`calibration.html`, `calibration_page.js`, and `calibration.js` are standalone,
framework-free assets embedded at build time. Another host can serve the same
files and implement the page protocol without linking UxPlay or using Bluetooth.
The recipe in `calibration.js` is independent of DOM and network transport.

The central canvas receives real Pointer Events. Three zero-displacement raw
trials establish/check the screen-to-page mapping using absolute positions at
45%, 55%, and 50% of the portrait screen. This measures Safari's content offset
and scale instead of assuming browser content occupies the whole screen. Profile
width/height are portrait `screen.width`/`screen.height` (logical points), **not**
canvas dimensions or AirPlay pixel dimensions. This interpretation still needs
real Safari/AssistiveTouch validation, especially with Display Zoom/page zoom.

Trials stay in the intersection of the canvas interior and the middle 60% of the
screen. Starts alternate slightly to avoid ignored identical absolute reports.
The page disables normal canvas scrolling/selection and rejects incomplete,
cancelled, clipped, wrong-axis, or unstable-viewport observations. Browser events
measure endpoint displacement; their timestamps are not HID transport timestamps.
Do not rotate, change zoom, resize browser chrome, or background Safari mid-run.

Normal detection uses up to four adaptive vertical probes against dense known
profiles. Survivors with the same dead zone that agree within detection tolerance at every
count across the shortest survivor's full domain may reuse that shortest profile.
This handles Safari-generated curves alongside longer native-canvas versions,
without activating unverified longer tails. Catalog order breaks equal-domain
ties. Divergence within the chosen domain remains ambiguous and triggers generation.
A selected existing profile also
passes the validation suite before activation.

Full generation measures every integer HID magnitude from 1 through 127 **within
the usable screen area**, retaining only directly measured counts. It stops before
predicted overflow, or discards the first clipped trial. The measured domain is
bounded; it does not extrapolate to unmeasured larger reports. The recipe builds
a monotonic radial-linear-component-gate table and dead zone, then asks the driver
to prepare that curve (possibly returning an equivalent existing candidate).

The prepared/selected curve is validated with endpoints, repeated movements,
both axes, reverse directions, diagonals, dead-zone probes, report intervals of
10/30/100 ms, packet size, return-to-start, and hover versus drag. Detection uses
max(0.75 points, 1% of displacement); validation uses max(2 points, 3%). A mismatch
fails the session rather than fitting a timing-dependent or asymmetric model.
Profiles use the same version-1 JSON schema as iphone-use/Inspector.

## Coordinator/page protocol

The page is served at `/mouse-calibration`; the random calibration UUID is in its
URL fragment. Same-origin JSON `POST /api/calibration` includes `session` and
`operation`. This UUID identifies a **calibration attempt**, not the phone's
permanent identity or the earlier typed pairing code. Requests must originate
from the currently bound phone IP. Old pages cannot control new attempts.

`status` returns `active`, `busy`, `job`, `inputIdle`, `force`, `status`, `result`,
and `error`. Hardware operations return HTTP 202 with a monotonically increasing
job ID; the page polls `status` until that job completes. Only one hardware
operation is outstanding per keyboard. `abort` cancels the attempt. Other
operations are the hardware API operations below; the coordinator owns target,
version, force flag, and device ID rather than trusting page routing fields.

The page reports readiness by requesting `begin` after load/viewport settling.
Hardware I/O runs on the existing input adapter worker, never the HTTP handler or
AirPlay decoding thread. HTTP completion means the complete hardware trial has
finished. Browser fetch/JS timers never schedule individual HID packets.

There is no human-interference lock. Ordinary coordinator commands defer while
its own calibration is running, and its inventory/recovery polling does not
release a button in the middle of its own trial. Local AirPlay video continues;
ControlFloor registration waits for calibration and the final Home command. Closing/resetting the session, disconnecting, a driver error, or 60
seconds without page contact cancels the raw session. The driver must also expire
abandoned sessions independently. Existing profile remains active on failure.

Results and errors appear in the keyboard's recent events and append-only log;
all page requests also use the existing HTTP request log. Startup sends Home after successful calibration; standalone manual runs leave the
result visible for development.

## Keyboard/mouse server contract

The hardware server is independent. UxPlay currently adapts the iphone-use HTTP
API; a USB-backed server can implement this contract without knowing about
Shortcuts, AirPlay, ControlFloor, Safari, or the calibration UI. A transport using
a different HID descriptor/report model must validate its own compatibility;
profile equivalence is not implied merely by sharing a JSON schema.

The adapter selects one stable hardware identifier. `GET /devices` returns
`bluetoothHosts` records with `bluetoothHost`, `keyboard`, `consumerControls`,
`liveMouse`, and a `mouse` status object. These legacy field names belong to this
adapter, not to the conceptual calibration protocol. Status includes `calibrating`,
`down`, optional logical dimensions, active profile/fingerprint, and capture state.
Inventory must be safe to poll during calibration. A driver may later advertise a
more general inventory and use a different adapter.

Mouse 8 launch uses existing `POST /mouseDown` and `/mouseUp` with `device` and
`button:8`. Raw calibration uses **POST `/mouseCalibration`**, JSON:

```json
{"testVersion":1,"session":"uuid","device":"hardware-id","operation":"begin","deviceID":"airplay-serial","force":false}
```

Every operation includes `testVersion`, `session`, and the driver-selected target.
Success is HTTP 200 JSON; errors use a non-200 status and `{"error":"message"}`.

| Operation | Request / required behavior |
| --- | --- |
| `begin` | Start a fresh session; require absolute pointer and relative mouse support, release any held buttons; remember deviceID/force policy. |
| `profiles` | Return `{"profiles":[{"name","fingerprint","profile"},…]}` in catalog priority order. Only fully measured detection candidates. |
| `trial` | Strictly increasing `sequence`, normalized full-screen `x,y` in 0.02…0.98, `orientation:"portrait"`, `reports:[[dx,dy],…]`, seconds `interval`, boolean `hover`. Counts are signed integers −127…127. |
| `select` | Stage an existing `fingerprint`; return its saved envelope including `profile`. Forbidden for forced sessions. |
| `prepare` | Stage supplied `profile`; validate schema, perform catalog reuse if appropriate, return the **actual** candidate envelope for phone validation. No activation yet. |
| `commit` | Persist and activate the exact staged `fingerprint`, associated with the hardware/device identity; return name/fingerprint. End session. |
| `cancel` | Release held buttons, discard pending candidate, retain prior active profile, end session. |

A `trial` must perform this sequence entirely in the hardware server:

1. Release buttons, move absolute pointer to normalized screen x/y, allow settling.
2. For a drag: button 1 down, settle, send raw relative reports with driver-timed
   intervals, settle, button up. For hover: click at start, move with button up,
   click at end. The page therefore expects one or two down/up pairs respectively.
3. Return only after final button release and settling; release on errors too.

**Never pass raw calibration counts through the current compensated planner.**
The current iphone-use backend uses its own absolute relocation detour and
settling delays. A replacement must provide equivalent repeatable positioning,
but need not copy that implementation. A relative-only mouse backend would need
a separate positioning strategy before it can implement this trial contract.

The profile schema contains `testVersion:1`, `width`, `height`,
`model:"radial-linear-component-gate"`, `deadZone`, `axisPoints` (index = HID
magnitude, value = screen-point distance), `measuredCounts`, and optional numeric/
boolean `evidence`. Keep fingerprint computation/catalog selection in the driver.
The adapter adopts the committed candidate's dimensions for subsequent live input.
Unsupported model/version, insufficient space, or driver errors must be explicit.

## Tests

```sh
node controlfloor/tests/calibration.js
build/test-venv/bin/python controlfloor/tests/calibration.py \
  build/controlfloor/controlfloor/controlfloor_pairing_harness
```

The second test uses aiohttp and an isolated headless Chrome installation at the
usual macOS application path. It runs the real hosted assets and coordinator,
simulates raw HID movement into browser pointer events (including a 70-point
browser-content offset), checks full generation/validation/commit, stale tokens,
Host-independent URL selection, cancellation, and driver failure. It never uses
live phone input. Chrome simulation is not a substitute for the first on-phone
Safari trial.
