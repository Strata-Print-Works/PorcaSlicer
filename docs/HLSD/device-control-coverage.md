# Device control coverage

Devices exposes the existing Orca monitor and vendor website without changing a
plate's editing configuration. Exposure is not hardware qualification: saved
profiles describe an intended setup, while firmware reports and the selected
adapter determine available controls. Installed nozzles and accessories must not
be inferred from a saved profile or printer name.

## Connection ownership

`NetworkAgent` owns a registry keyed by the transport's device identity.
Bambu factories cache separate worker processes by device identity; Moonraker,
Creality, Snapmaker and Qidi cache separate instances by host and port. Their
connection/status workers remain owned by that instance when Devices changes
selection or returns to Prepare. Saved native devices establish their sessions
after initialization, starting at most one new attempt per second, independently
of the selected view. Missing profiles, credentials or connection support remain
actionable failures. Unsupported hosts retain bounded availability checks rather
than being labeled connected.

An equal connection request reuses its attempt, including a pending connection
or reported failure. Reconnect invalidates the previous token and resets that
device intentionally. Editing connection details and removing a device retire
its session. Replacement invalidates callbacks before disconnecting the old
transport. Shutdown clears callbacks on every retained instance and disconnects
them. Retired transports are destroyed on an owned reaper thread rather than the
GUI thread; the agent drains it on destruction and before the network module unloads.
Each implementation remains responsible for joining its workers.

Native command routing uses the requested device's owner. Unknown LAN targets
fail closed. Typed UI controls reject an inactive transport. An inactive LAN
transport can receive only version and full-status requests through
the JSON message path; stale control dialogs cannot send physical commands to it.
Cloud message and print APIs retain their explicitly device-addressed contract.
Connection and LAN telemetry callbacks carry an attempt token and device ID,
checked again after GUI dispatch. Discovery notices are stamped with the
originating backend before GUI dispatch, so background discovery cannot reassign
a device to the visible backend. Camera URL requests also carry a view token, so
an A → B → A selection cannot accept a URL from the first view of A.

Bambu account services retain their SDK handle in the application. Each saved
Bambu LAN printer owns a `porca_bambu_worker` process, loading the exact active
plugin library as its first SDK agent. Reopening a module in the application is
not isolation: vendor builds can return null or reuse an existing agent. The
process boundary separates library globals, callbacks, sockets and destruction.
The same-process opaque-handle guard remains required and rejects duplicates
without disconnecting or destroying the existing owner.

`BambuLanProcess` retains the mature `BBLPrinterAgent` command builders and
forwards local requests over private inherited stdin/stdout pipes. Device
identity, credentials and print parameters never enter argv or temporary files.
Frames are bounded; requests and callback queues have limits. The worker binds
to one device and rejects other print destinations. All five LAN print entry
points preserve their ABI dispatch, parameters, cancellation, progress and wait
callbacks. Cloud subscriptions, account operations and cloud camera requests
retain their explicitly device-addressed account path. The worker does not start
discovery, account connections or cameras. SDK analytics are disabled before its
workers start; unsolicited SDK output cannot corrupt the private protocol.

SDK initialization distinguishes library load, version, missing symbols, null
creation, duplicate ownership, configuration, certificate and country failures.
Worker readiness alone is not Connected; only the SDK connection callback sets
that state. Sanitized failure reasons remain available in the selected detail
and list tooltip. Replacement invalidates queued callbacks before closing the
worker's input. macOS/Linux launch uses `posix_spawn`, without executing C++
code after a GUI process fork. Peer pipe handles are excluded from child
inheritance. Polling only the owned child avoids Boost timed waits that fork a
timeout helper and change the application signal handler on macOS. A worker is
reaped on failure, and shutdown bounds an uncooperative SDK to two seconds
before terminating only that owned process. Parent EOF also starts a worker
cleanup watchdog. No live SDK or hardware compatibility is established by the
socket-free tests.

Reconnect and credential changes retire the cached worker. The replacement
receives new callback storage; queued callbacks retain the retired attempt's
invalid token. Retrying a visible device changes its transport without changing
the selected machine or Prepare. Other devices keep their transport and state.
A connection attempt that has not completed within thirty seconds is invalidated
and disconnected before bounded retry backoff. The same recovery applies when a
connected attempt no longer has fresh machine status, with a thirty-second grace
period for a new attempt. Expiration closes its command route and keeps the
selected identity for eventual replacement. Rejected credentials wait for an
edit or explicit Reconnect. Python capabilities return one live capability
object and are not cloned into independent transports.

Availability checks remain bounded and independent of control connections.
They skip owned native attempts, including background, failed and connecting
ones. A successful HTTP/MQTT check means Available, not Connected. Native
connection failures, rejected credentials, pending control connection and stale
telemetry are distinct. A silent Moonraker socket does not renew telemetry
timestamps by replaying its cache. Reopening the inspected monitor does not
renew its machine's timestamp either.

Moonraker server availability alone is insufficient for Connected: a recognizable
server reply and a fresh nonempty status snapshot are both required. HTTPS hosts
retain monitoring through bounded HTTP queries at five-second intervals rather
than abandoning status because the native stream has no WSS implementation.
Failed queries stop that poller and report either lost status or rejected credentials.
Initial and polled telemetry retain their attempt generation through GUI dispatch.

## Adapter and view matrix

**Native** means wired through Orca's existing panel and adapter, conditional on
firmware capabilities. **Web** means the complete printer-hosted interface is
accessible from Devices; its contents depend on firmware and are not reproduced
or qualified by Porca. **Missing** means the integration is absent, not that the
physical printer cannot support it. **Unsupported** means this adapter cannot
translate the operation. No row establishes live or physical validation.

| Family / saved model | Adapter | Devices exposure | Retention / limits |
|---|---|---|---|
| Bambu A1 mini (`N1`) | `bbl` | Native Status, Storage, Firmware and Troubleshooting; expanded control header and AMS UI | Independent device process; plugin and runtime capability reports required |
| Bambu A1 (`N2S`) | `bbl` | Same native panel, with A1 capability definitions | Independent device process |
| Bambu X2D (`N6`) | `bbl` | Same panel, including supported chamber, air-duct, multi-extruder, calibration and safety actions | Independent device process; no assumption that reported accessories are installed |
| Bambu A2L (`N9`) | `bbl` | Same panel, including supported AMS organization and calibration actions | Independent device process; no assumption about nozzle or accessories |
| Creality SPARKX i7 | `crealityprint` | Native monitoring and translated basic commands; Printer website for vendor controls | Separate instance; Moonraker compatibility is required by this adapter and is not established by discovery or the model's name |
| Snapmaker U1 | `snapmaker` | Native monitoring and translated basic commands; Printer website for toolhead, filament and other vendor functions | Separate instance; native filament pull is supported, but it does not imply full toolhead control |
| Elegoo Centauri Carbon / Carbon 2 | `ElegooLink` upload; CC local website / CC2 bundled vendor panel | Explicit Elegoo connection choice; complete existing vendor page and its IPC upload/file actions; CC2 registration gates commands; Prepare offers read-only CANVAS filament review; Upload and Print starts single-filament prints on a CC2 without CANVAS | HTTP availability and background identification only outside the view. Panel MQTT/status/control exists while explicitly opened; no retained native CC2 agent. Firmware handshake remains unqualified. |
| Flashforge Adventurer 5M | No registered control agent | Saved availability and plate actions; existing guarded upload remains in Prepare | Missing native monitor/control adapter; upload support establishes no control capability |

The four Bambu capability definitions live in `resources/printers/N1.json`,
`N2S.json`, `N6.json` and `N9.json`. These include version overrides and must be
interpreted together with actual machine reports. For example the A1 family
does not enable chamber/auxiliary fans, chamber heat, lidar or AI monitoring in
its base definitions. X2D has a different fan and extruder model. Reusing its
panel does not enable those operations on an A1.

## Existing Orca controls

| Control surface | Bambu | Creality / Snapmaker native | Vendor website | Flashforge |
|---|---|---|---|---|
| Camera, playback, virtual camera, recording, timelapse, resolution | Native, gated by camera/firmware support | Camera URL capability is absent in these adapters | Web, where supplied | Missing monitor/camera adapter; accessory presence unknown |
| Job name, stage, progress, remaining time, temperatures and live filament | Native | Native translated status; available fields only | Web | Availability only; detailed monitoring missing |
| Pause, resume, cancel / stop | Native | Native `pause`, `resume`, `stop` translation | Web | Missing control adapter |
| Skip parts, job error recovery, HMS acknowledgment, buzzer | Native where supported | Unsupported vendor commands | Firmware-dependent | Missing |
| Bed / nozzle temperatures, movement / home, extrusion | Native | Existing G-code/temperature/home translation; firmware dialect still matters | Web | Missing |
| Chamber heat, fans/air ducts, lights, speed modes, print options | Native where supported | Bambu-specific controls unsupported; fan/lamp/speed widgets omitted | Web | Missing |
| Load/unload, AMS RFID, tray metadata, backup, drying, AMS settings / calibration, external spool | Native where supported | Filament metadata pull exists; Bambu AMS actuation omitted | Web; changer/toolhead support depends on firmware | Missing; 5M must not be treated as AD5X/IFS |
| Multi-nozzle/extruder selection, nozzle rack / parts / nozzle configuration | Native where reported | No equivalent neutral toolhead-control implementation | Web | Missing; no nozzle inferred |
| Bed leveling, vibration/motor-noise, flow/PA/ratio and calibration results/history | Native where supported | Advanced vendor calibration unsupported; native calibration header omitted | Web | Missing |
| Print/safety options, AI detection, recovery, runout/tangle, prompt sounds | Native where supported | Bambu-specific messages unsupported | Web | Missing |
| SD/internal files, downloads/deletion, timelapse files, model/job storage | Native Storage | Bambu storage protocol unsupported; native tab omitted | Web | Existing upload only; browsing/history/control missing |
| Firmware/module updates and notices | Native Firmware | Bambu upgrade protocol unsupported; native tab omitted | Web | Missing |
| Assistant/HMS, error help and device diagnostics | Native Troubleshooting | Bambu HMS protocol unsupported; native tab omitted | Web | Missing |
| Task history | Current-job detail and Storage are exposed; fleet/task-history pages are not embedded in Devices | No native history service | Web if provided | Missing |

The evidence owners are `StatusPanel.cpp`, `DeviceCore/`, `DeviceTab/`,
`MediaFilePanel.cpp`, `UpgradePanel.cpp`, `HMSPanel.cpp`, `CalibrationPanel.cpp`,
`CalibrationWizardPage.cpp`, and the `MachineObject::command_*` operations in
`DeviceManager.cpp`. `MoonrakerPrinterAgent::handle_request` implements the
narrower translation, `CrealityPrintAgent::fetch_filament_info` adds CFS reads,
and `SnapmakerPrinterAgent::fetch_filament_info` adds U1 filament reads. Neither
filament reader implements generic AMS actuation. Unknown Moonraker commands
return `ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED`.

## Elegoo routing and control boundary

Add by address and Edit printer offer Elegoo Centauri explicitly. The selected
Centauri Carbon or Carbon 2 profile supplies the model; the connection projection
selects `htElegooLink` without modifying that profile, nozzle or material setup.
A Centauri profile alone never reclassifies a generic Moonraker host. Correcting
the connection in Edit printer retains the old local identity key, credentials,
serial and destination aliases. It replaces only the old protocol's default
port; custom ports remain editable. Repeated correction is idempotent and
rediscovery preserves the saved key. No printer is deleted and re-added.

CC2 participates in Add a printer's automatic search with JSON method 7000 on
UDP 52700. The first-party
[discovery strategy](https://github.com/elegooofficial/elegoo-link/blob/main/src/lan/adapters/elegoo_fdm_cc2/elegoo_fdm_cc2_discovery_strategy.cpp)
and [adapter declaration](https://github.com/elegooofficial/elegoo-link/blob/main/src/lan/adapters/elegoo_cc2_adapters.h)
define its separate protocol. Porca's implementation is written independently.
Private reference sources also inform its protocol details; no proprietary code,
hardware fixtures, runtime, deployment policy or credentials are copied or
imported.

Search owns at most four sockets on eligible physical broadcast interfaces and
constrains them to the interface where supported. A six-second scan sends at
most one broadcast per interface every two seconds, sharing Porca's aggregate
probe limiter; limiter waits also honor the scan deadline. Responses must come
from the receiving interface's subnet and the discovery port. Oversized packets
are counted and rejected. Bounds are 8192 response bytes, 1024 received packets,
32 packets per interface/pass and 256 serial identities. Cancellation checks
surround sends, receives and publication; socket ownership ends with the joined
scan worker. Pure transport fixtures exercise the production loop without sockets.

Validated model/serial syntax identifies CC2 candidates but authenticates no
printer. Sender IP is authoritative; advertised addresses are ignored. LAN mode
and access-code protection retain unknown values explicitly. Cloud-mode and
unknown-mode candidates remain visible with setup guidance. Positive LAN mode
is required for discovered candidates to be added; protected or unknown
protection requires a supplied/saved code. A positively unprotected candidate
may use the vendor default. New Elegoo candidates require explicit installed
nozzle/profile selection. Manual entry remains available. Availability and
upload preflight reject a changed known serial instead of overwriting its cache.
The upload host captures its expected serial with its connection settings; the
same guard applies to Windows resolved-IP delivery. Discovery updates with the
same serial preserve saved profile, custom HTTP port, local identity,
credentials and user name while updating the address. This is advertisement
continuity, not authenticated identity or proof that controls are usable.
The seven scan producers complete independently; duplicate or unknown producer
completions cannot finish the scan early.

CC2 availability uses bounded `/system/info` with the existing `X-Token` policy,
including the vendor default when no custom code is saved. A valid serial is
required; 401/403, service refusal, incompatible replies and unavailable HTTP are
separate results. Success means Available, not a registered control connection.
Identification runs in the background. URL construction and `get_sn` use local
caches without blocking the GUI on HTTP. All URL parameters are encoded; raw
server bodies and credential-bearing navigation URLs are excluded from new logs.

The bundled vendor page opens explicitly and unloads on leaving the view.
Its IPC handler owns a configuration snapshot, including the upload target,
rather than consulting Prepare or the next selected printer. Queued replies
check handler lifetime and visibility. Upload workers are cancelled/joined on
retirement; synchronous transport return can still delay joining. File dialogs
run after the WebView callback returns. There is no native persistent Elegoo
control adapter: HTTP polling and loading a web page do not fulfill first-class
background CC2 control or full all-printer parity.

The bundled page uses `elegoo/{sn}/api_register` and registers only on a
matching serial/request/client reply with `ok` or `already registered`. Commands
remain gated after refusal or timeout. The page's WebView handler includes the
upstream [CC2 registration fix](https://github.com/OrcaSlicer/OrcaSlicer/pull/14024).
Vendor MQTT reconnect/register retries follow the bundled implementation's
policy, including a warning at ten registration failures rather than a hard
retry cap. Hardware authentication, camera behavior, firmware compatibility and
actual uploads are unqualified.

## Flashforge protocol boundary

`Flashforge.cpp` implements authenticated `/detail` reads, material-slot parsing,
HTTP upload and the older TCP upload path. It does not implement an
`IPrinterAgent`, job controls, native camera, file browsing or control telemetry.
The Flashforge identity guard before delivery remains required. The send dialog
reuses the machine state from the `/detail` reply that lists material slots to
show a busy printer or a finished print awaiting confirmation; it does not block
uploading. A request refused with result code 2 is reported as a busy printer,
with how to clear a finished print; the firmware's own text stays in the log.

The vendor's published [Orca-Flashforge network façade](https://github.com/FlashForge/Orca-Flashforge/blob/main/src/slic3r/Utils/NetworkAgent.cpp)
delegates communication to an external networking library; it is not a reusable
open HTTP control adapter. Independent implementations provide useful protocol
evidence: the [Flashforge Python API control source](https://github.com/GhostTypes/ff-5mp-api-py/blob/main/flashforge/api/controls/control.py)
and [job control source](https://github.com/GhostTypes/ff-5mp-api-py/blob/main/flashforge/api/controls/job_control.py)
describe authenticated control requests. They do not qualify this machine or
its firmware. Several operations are model-specific, and printer-control
payloads combine speed, Z offset and fan values, so blindly reusing defaults
can change unrelated settings. This integration does not install that library,
borrow its cloud service, assume a camera/IFS, or send speculative commands.
Flashforge control parity remains an explicit missing implementation.

Hidden vendor websites unload their documents and reload only when explicitly
viewed, releasing camera streams and page timers independently of native sessions.

## Verification boundary

`[ElegooDiscovery]` covers the pure CC2 parser and production scan loop with fake
clock/transport: setup flags, malformed/unrelated/oversized/off-subnet replies,
serial merging, DHCP updates, cancellation, deduplication, packet/result budgets
and concurrent completion accounting. No fake opens even loopback sockets.

`[ElegooLink]` exercises actual saved-connection projection, host construction,
identity serialization/merge, custom ports, CC/CC2 classification, encoded local
panel URL construction and structured identity failures without calling a
transport. Its packet fakes also cover the CC2 start request, refusals,
unconfirmed replies, the CANVAS/filament/identity gates and the upload gate. `node tools/test_elegoo_link.cjs` executes the shipped register class
in a VM with fake MQTT and fake timers: registration gates, target matching,
refusal, timeout, recovery and independent page state. It does not run the full
web application or qualify firmware/native WebView lifecycle.

The `devices` offline group loads explicitly built socket-free fake SDKs through
the real loader, wrapper and headless worker. It reproduces first-only creation,
reused singleton handles, null creation, bad ABI, missing exports/library/worker
and configuration failure. Four concurrent workers run while the parent also
owns the fake singleton, proving address-space isolation rather than idealized
fresh-handle behavior. Tests cover routing, unchanged-request reuse, retirement,
stale callbacks, recovery, print callbacks/cancellation, worker crash and bounded
hung-worker cleanup without affecting another device. Pure fake agents also
cover `NetworkAgent` dispatch, credential changes, explicit reconnect, failure
state and shutdown. Existing Moonraker lifecycle tests override every transport
entry point. No test loads the installed vendor library or opens sockets.
In-flight synchronous HTTP, DNS and WebSocket calls still depend on their
transport returning before the connection worker can join; generation invalidation
does not establish an independently interruptible transport deadline.
The Moonraker fakes also exercise HTTPS polling through the production stream
selector, initial-status rejection, malformed response envelopes, fresh snapshot
delivery, authentication loss, in-flight disconnect and unsupported print entry
points. The request and poll-wait boundaries are fake; these cases open no sockets
and do not qualify the HTTP wrapper's TLS policy or the real transport timeouts.
Native view geometry, focus and event ordering, vendor website contents,
concurrent live connections, installed hardware and physical command behavior
require separate validation. A macOS build does not verify Windows or Linux.


## CC2 filament synchronization and nozzle selection

The Prepare filament-sync action uses a separate, bounded read-only MQTT request
for an ElegooLink CC2 destination. It does not depend on the printer selected in
Devices or reuse a Bambu `MachineObject`. The connection captures the active
plate identity, edited setup and local connection before starting a cancellable
UI job. Its ten-second session owns its socket, uses a unique client ID, verifies
registration and the saved serial/model with method 1001, then requests method
2005. Only matching, non-retained responses can supply CANVAS data. This read
sends no other method. It neither changes discovery nor establishes a
persistent background-control session.

Connected CANVAS units supply reported slot identifiers, material names and
colors. Disconnected units are omitted. Empty, invalid or unknown values are
kept unknown; an explicit empty/unsupported slot status clears stale material
values. A review dialog offers compatible visible presets with the same material
type, without inferring a brand or tuned profile. Every row starts with Keep
unchanged. Selected known-color materials are added to the active plate using
existing filament insertion; identical preset/color pairs are reused. Existing
filaments and object assignments are retained. Mixed-filament and read-only
exported projects require an editable, unmixed plate first. Changed plate,
settings or connection discard the result, including changes during the review
dialog. Original Carbon does not use this CC2 path. Physical firmware, LAN mode,
accessory state and this client's interoperability still require permitted live
validation.

The Add nozzle selector has no implicit first choice for an unknown Elegoo
nozzle. A saved explicit profile is retained across sightings. Edit printer offers
same-model nozzle profiles. An explicit correction updates only nozzle diameter,
variant and printer-preset identity on that destination's plates, preserves other
process/material edits, and invalidates those plates' slices. On activation,
Orca rebases unedited nozzle-dependent defaults to the selected preset; explicit
edited settings retain their usual difference metadata. It does not change
model placement or derive installed hardware from discovery or a profile name.

The method 1001/1002/2005/1020 exchanges and CANVAS normalization follow private
reference sources. Porca's fixtures are synthetic; no proprietary source or private
hardware fixture is copied. The bundled Elegoo panel remains available for
vendor controls, with the lifetime and persistent-control limits described
above.

## CC2 upload and print

ElegooLink uploads CC2 files over HTTP. When the user chooses Upload and Print,
a separate start command follows once, after the whole upload succeeds. Upload
only, a failed or canceled upload and each attempted Windows address never reach
it. The start uses the same bounded session as filament sync with a fresh client
ID: registration, the serial/model check with method 1001, then method 1002
status. It sends method 1020 only when that status explicitly reports no attached
CANVAS (`external_device.type` 0, `"0"` or empty) and the sliced plate uses
exactly one filament. The request names the uploaded file on local storage, the
chosen bed leveling and plate side (A textured, B smooth), and keeps time-lapse
off. Like the vendor's single-material request it carries no slot map, so the
printer uses the filament loaded at the print head. The printer itself refuses a
start while busy.

Porca has no confirmed mapping from plate filaments to CANVAS slots. Filament
sync adds presets and colors without recording slots, and a reading goes stale
when spools change. Prints with several or unknown filaments, or with an attached
or unreported CANVAS, are therefore uploaded only, and the send dialog or upload
queue asks the user to start them from the printer's screen. Time-lapse needs a
camera this exchange does not confirm, so the CC2 dialog does not offer it.

The matching reply decides the outcome. Error code 0 is a started print; any
other code is a refusal shown with its reason and code. A missing, mismatched,
retained or late reply after sending is reported as unconfirmed and asks the
user to check the printer. No start is retried. The queue's cancel applies until
the command is sent; the reply then gets its own ten-second window. Logs carry
no access code. Firmware acceptance of this request, CANVAS reporting and the
print head's behavior are unqualified until a permitted live print.
