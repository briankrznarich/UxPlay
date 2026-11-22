# ControlFloor branch workflow and integration plan

This guide lives on the `controlfloor` branch. The receiver's default `master`
branch remains focused on reusable multi-device AirPlay support.

The branching workflow below is established. The architecture section is a
proposal for the next implementation: OCR pairing and ControlFloor integration
have not yet been ported into this branch.

## Repository and branch arrangement

Use two long-lived branches, with updates flowing in one direction:

```text
FDH2/master --> our master --> our controlfloor
```

| Reference | Purpose |
| --- | --- |
| `upstream/master` | Fetched view of FDH2's development branch |
| `master` | Our general-purpose multi-device receiver |
| `controlfloor` | OCR pairing, input orchestration, shortcut callbacks and ControlFloor adapter |

`origin` is our GitHub fork. `upstream` is `https://github.com/FDH2/UxPlay.git`.
A separate local branch mirroring upstream is unnecessary: `upstream/master`
already supplies that reference after fetching.

The refork preserves FDH2 ancestry. At creation of this branch, `master` was
`e35b03d` (Initial multi-device support), based on FDH2 commit `48f5ce4`.
Preserve Git ancestry when updating; do not synchronize source trees with rsync
as the recurring update mechanism.

All branches pushed to this public repository are public. The branch boundary
organizes development; it does not provide confidentiality.

## First-time setup for another checkout

Inspect the existing configuration first:

```sh
git remote -v
git branch -avv
```

If `upstream` is absent:

```sh
git remote add upstream https://github.com/FDH2/UxPlay.git
```

Fetch the remotes:

```sh
git fetch origin
git fetch upstream
```

If `origin/controlfloor` exists and there is no local `controlfloor` branch:

```sh
git switch --track origin/controlfloor
```

Otherwise, create it once from our receiver branch:

```sh
git switch master
git switch -c controlfloor
```

Publishing this branch, when intended, is a separate step:

```sh
git push -u origin controlfloor
```

## Routine upstream updates

Begin with a clean working tree. Save unrelated work before switching branches.
For a shared repository, first fetch `origin` and reconcile any collaborators'
changes to the branch being updated. Do not overwrite remote work.

Update the receiver first:

```sh
git fetch upstream
git switch master
git merge upstream/master
```

If Git reports conflicts, resolve them, stage only the resolved paths, and run
`git merge --continue`. Preserve upstream changes as well as our integration
hooks; choosing an entire file from one side can silently lose functionality.
`git merge --abort` returns to the pre-merge state if the update is abandoned.

Build and test before publishing. Keep build directories separate between
branches so dependencies and configuration do not leak between them:

```sh
cmake -S . -B build/receiver -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_MULTIDEVICE_TESTS=ON
cmake --build build/receiver -j 6
ctest --test-dir build/receiver --output-on-failure
python3 multidevice/tests/test_receiver.py build/receiver/uxplay
python3 multidevice/tests/test_receiver.py build/receiver/uxplay --single
```

The receiver smoke tests briefly advertise a uniquely named service and require
local socket access, Bonjour/Avahi and GStreamer. Also test two real phones with
motion and audio, including reconnect and rotation. Synthetic RTSP tests do not
validate encrypted media delivery or playback timing.

After verification:

```sh
git push origin master
git switch controlfloor
git merge master
```

Resolve any product-integration conflicts, then build in `build/controlfloor`.
Run the receiver checks plus the pairing, video-protocol and input checks as
those components are ported. Do not publish solely because the merge compiled.

```sh
# After the controlfloor branch's checks and live-device trial pass:
git push origin controlfloor
```

Use merge commits for published long-lived branches. Rebase short-lived,
unpublished feature branches when useful. Routine upstream integration should
not require force-pushing or replaying the entire feature stack by cherry-pick.

## Where new work belongs

- General receiver fixes and reusable worker/frame interfaces originate on
  `master` or a short-lived feature branch based on it. Merge `master` forward
  into `controlfloor` after they are integrated.
- Product behavior originates on `controlfloor` or a short-lived feature branch
  based on it, such as `feature/ocr-pairing`.
- Do not merge the whole `controlfloor` branch back into `master`. If a generic
  fix was first made there, extract the focused fix into a receiver change.
- Keep commits focused. Separate upstream integration hooks, new modules,
  protocol changes and test/documentation updates when they are independently
  reviewable. Avoid unrelated formatting or whole-file replacement.
- Keep product dependencies optional. The receiver must remain buildable
  without ControlFloor/OCR packages. Concrete build flags belong with the port;
  this guide does not imply those flags already exist on this branch.

## What to reuse from the existing ControlFloor proof of concept

The reference implementation is the working tree at
`/Users/briank/git/UxPlay`, primarily `controlfloor/` and its renderer hooks.
That checkout currently has uncommitted changes, including text-input support
and tests. Review and preserve its working-tree changes before porting; its
committed history alone does not represent everything that currently works.
Do not overwrite or clean that checkout as part of this integration.

Reuse the existing implementation and tests for:

- JPEG sizing, quality, frame-rate/bandwidth settings and ACK/backpressure logic.
- Bounded latest-frame capture and separate encoding/network execution.
- ControlFloor command envelopes, capability/status replies and error behavior.
- Input translation, complete-string validation before typing, queue expiry,
  session cancellation and best-effort release of held buttons.

The existing implementation uses process-global state in `controlfloor.cpp`, a
constant `airplay-test-12345` device ID, and a `cf_client()` callback that makes
an arriving AirPlay client immediately eligible for a ControlFloor connection.
`MouseInput` requires exactly one entry in `iphone-use`'s Bluetooth inventory.
Those assumptions must change before combining the implementations.

## Proposed ownership and module boundaries

Keep the supervisor as the central coordinator and one receiver process per
AirPlay peer. A new orchestration service/process is not needed for this stage.

| Component | Ownership |
| --- | --- |
| Supervisor pairing coordinator | Hardware inventory, startup attempts, fresh challenges, identity registry, unique assignments and admission/revocation |
| Worker frame tap | Bounded access to decoded samples, frame sequence/time and stream generation, independent of ControlFloor connectivity |
| Worker OCR task | Tesseract instance, requested region/stage, bounded latest-frame processing, typed results |
| Worker ControlFloor session | Approved device identity, WebSocket, commands, JPEG encoder, video configuration and ACK state |
| Coordinator input adapter | Explicit hardware target, serialized input per controller, queue cancellation and button-release handling |
| Provider account/configuration | One explicit registration/configuration operation; workers read credentials and never register/rewrite the shared account config concurrently |

Encapsulate the old ControlFloor globals in a session object with explicit
start/admit/revoke/stop operations. One such object per worker is sufficient;
there is no need to turn the existing renderer into an in-process multi-phone
engine. Extract components where responsibilities differ, rather than rewriting
the working protocol implementation wholesale.

The supervisor grants a binding to the worker. The worker can then open its own
per-device ControlFloor WebSocket and send JPEGs directly to the server. The
supervisor does not relay continuous video frames.

Input commands arriving over that WebSocket should be routed through the
coordinator with their session and binding generation. The coordinator validates
ownership and sends them to an explicit backend device. Reuse the iphone-use
HTTP translation, but remove the implicit single-phone selection. This is also
the boundary for future USB keyboard/mouse hardware.

A small typed, versioned command/event extension to the existing worker pipes
is enough: request/cancel OCR, OCR result, admit/revoke binding, input request and
input result. Bound message sizes and pending requests. Do not use human logs
as IPC or transmit credentials in diagnostic output. The current worker pipe
protocol handles lifecycle events and STOP only; these extensions are planned.

## Separate video capture from ControlFloor admission

Currently the frame tap is attached through `cf_attach()` and capture state is
coupled to the provider's `running`/`active` flags. OCR must work before provider
admission, so frame availability must be independent of ControlFloor state.

Retain one decoded-frame tap. Feed OCR and JPEG encoding through separate,
bounded latest-sample slots, retaining sample references as needed. The capture
callback must not perform OCR, JPEG compression, blocking network I/O or wait
for ACKs. Perform CPU image conversion/readback in the consuming worker thread;
do not decode the video a second time for OCR or encode JPEGs just to OCR them.

OCR runs only while requested, at a modest sampling rate with cropped regions.
Cancel it after identification or session invalidation. JPEG forwarding remains
controlled by the viewer and its independent flow control. A slow OCR task or
viewer must not stall AirPlay playback.

## Identification is the admission gate

```text
AirPlay present, unbound
    -> fresh keyboard challenge matched
    -> device serial confirmed
    -> binding validated and admitted
    -> per-device ControlFloor connection allowed
```

1. The coordinator creates a fresh attempt token for a known keyboard and has
   that keyboard type a decorated identifier containing the token and a checksum.
   Use a restricted, OCR-friendly alphabet. The permanent keyboard identifier
   alone is insufficient because an old display could otherwise be reused.
2. A worker reads the token from its screen. The coordinator matches it to one
   outstanding attempt and tentatively associates that stream with the keyboard.
3. Read the serial number from the About page, using the field label/region and
   consistent readings from fresh frames. Do not silently guess ambiguous
   character substitutions or reuse results from the preceding OCR stage.
4. Validate uniqueness: one active keyboard assignment and one active serial
   assignment. Ambiguous or conflicting evidence keeps the device unadmitted
   with a useful diagnostic reason.
5. Persist a generated ControlFloor device ID keyed by the confirmed serial.
   Keep the AirPlay identifier/name as metadata, not the permanent identity.
   Only now grant the worker permission to open the device WebSocket and
   advertise availability.

Provider-account enrollment is distinct from advertising a phone. It may be
configured independently, but no per-device connection is allowed before both
keyboard and phone identity checks complete. Missing OCR dependencies or failed
recognition must not silently bypass this gate.

Track three different lifetimes: the AirPlay connection, the pairing attempt,
and the ControlFloor WebSocket session. A worker process can survive multiple
AirPlay connections, so its process/session UUID alone is insufficient to reject
stale results. Carry connection and attempt generations on OCR results, binding
grants and input commands; discard late results after invalidation.

A new AirPlay connection or hardware reassignment requires fresh pairing.
Revocation closes the per-device connection, cancels queued input/OCR and
releases held buttons. Already-issued hardware actions cannot be undone, so
validate immediately before dispatch and avoid retrying uncertain text input.
A ControlFloor network reconnect alone need not repeat hardware identification
if the same AirPlay connection and verified hardware binding remain valid.

## Incremental implementation order

1. Add the independent frame tap and OCR result path, without ControlFloor.
2. Demonstrate two keyboard challenges and two serials correctly associated
   with two streams. Initially navigate the phones manually; show pairing state
   and failure reasons locally.
3. Test ambiguous text, stale results, reconnects, duplicates, hardware loss and
   cancellation before enabling product admission.
4. Port the existing ControlFloor session and video protocol behind the gate,
   replacing the dummy ID with the verified registry identity.
5. Route remote input to the explicitly bound controller and extend the old
   input tests for multiple devices and binding generations.
6. Automate startup navigation and shortcut callbacks around that same state
   machine. Preserve the manual path for diagnosis and recovery.

This sequence reuses the proven video/protocol work while establishing identity
and ownership before adding unattended control.
