# Printer agents

Printer agents isolate printer-specific communication from the rest of OrcaSlicer. The GUI and
`DeviceManager` operate on a shared set of printer operations and device state; a selected printer
agent implements those operations for a particular printer ecosystem. The agent boundary allows
Bambu, Moonraker-based printers, built-in integrations, and Python-provided integrations to use the
same application workflow without making the GUI understand every printer protocol.

The current boundary is an adapter boundary around the existing application contract. In particular,
some request fields and message payloads still use the Bambu-shaped representation that existing
`MachineObject` and `DeviceManager` code consumes. The printer agent is responsible for translating
that representation into the protocol spoken by its printer. This is an intentional compatibility
constraint of the current design; the interface is not yet a neutral printer protocol.

The v1 dialect migration path is deliberately narrow. `DeviceManager` currently speaks the Bambu JSON
dialect because that is the payload shape already used throughout the command and state workflow. The
v1 `OrcaPrinterAgent` also accepts that Bambu dialect. Its transport path places the small translation
needed for the target printer at `deliver_to_sink`, keeping the compatibility code at the edge rather
than spreading it through `DeviceManager` or the agent interface.

The eventual direction is for `DeviceManager` to produce an Orca JSON dialect. The Bambu agent will then
own the translation from Orca JSON to Bambu's protocol, while `OrcaPrinterAgent` can forward the Orca
payload directly to its sink. The v1 translation at `deliver_to_sink` can then be removed without
changing `DeviceManager`, the command callers, or the rest of the agent workflow.

## Components

The system has four relevant layers:

```text
GUI / DeviceManager / MachineObject
              |
        NetworkAgent
          /       \
 IPrinterAgent   ICloudServiceAgent
      |                 |
 printer protocol   authentication and cloud services
```

### `DeviceManager` and `MachineObject`

`DeviceManager` owns the application-facing printer workflow. It maintains `MachineObject` instances,
updates their state, filters devices for the active printer agent, and initiates operations such as
homing, temperature changes, printing, subscriptions, and camera playback.

`MachineObject` remains the shared state model used by the GUI. It does not contain the implementation
of a printer protocol. When a device is discovered or returned by a cloud query, the device is tagged
with the active `printer_agent_id`. Device lists and selected-machine operations use that tag to avoid
sending an operation through an agent that does not own the device.

### `NetworkAgent`

`NetworkAgent` is the façade used by the GUI and `DeviceManager`. It owns:

- the currently selected `IPrinterAgent` and retained device-scoped LAN sessions;
- the registered cloud-service instances, indexed by provider;
- callbacks shared by the active printer agent and the application;
- the forwarding methods for printer commands and cloud operations.

The selected printer agent supplies the visible native panel. Saved LAN devices
retain independently owned sessions and establish them without selecting a view.
An explicit connect with equal parameters reuses a pending or connected
attempt, retries an offline one and reuses a rejected-credential attempt until
the parameters change. Explicit Reconnect, edited credentials and removal
invalidate its callbacks before replacement. Bambu LAN
printers own headless worker processes with independent SDK state and private
pipe routing; Bambu account services remain in the application. Reopening the
SDK library within one process is not treated as isolation. Moonraker, Creality, Snapmaker and Qidi own separate instances and
workers per device. Saved profile identity is captured on the UI thread and
passed with connection parameters, independently of the active plate. Python
capabilities retain their existing single-instance contract. See
[device control coverage](device-control-coverage.md) for ownership, the Bambu
plugin compatibility boundary and the complete control matrix.

Each Bambu LAN worker runs four loops: control requests, certificate work,
uploads and SDK callbacks. Uploads and prints are serialized per printer on
their own thread, and a slow call on one loop never delays another. The
callback loop drops its oldest entry beyond 512, so a status burst cannot end
the connection. Each operation has its own deadline: 5 seconds for control,
60 seconds for certificates and 30 minutes for prints. A timeout returns an
error and leaves the worker running; only end of input or a protocol error
stops it. The parent keeps at most 32 requests unanswered, including timed-out
requests awaiting a late reply. Certificate installation never blocks the
caller, and at most one runs per worker. Cancellation and parent-pipe closure
wake every loop; shutdown joins them before destroying the SDK instance. A
worker watchdog bounds cleanup when the vendor SDK ignores cancellation or
hangs. A missing worker executable is reported as `bambu_worker_missing`.

In the application, callbacks without a UI queue run in order on an owned
thread, never on a pipe reader, so a handler may reconnect, send a request or
release its transport. Teardown never joins the current thread: a retired
worker is reaped by an owned thread, which the transport joins on destruction.

Availability checks report `not_ready` when a Moonraker or Snapmaker service
answers but Klipper is not ready, distinct from an unreachable printer.

LAN upload and print calls retain a transfer guard on their originating session.
Automatic status recovery cannot retire that session while the call is active,
and allows thirty seconds for status to resume after the last transfer finishes.
Starting a transfer and automatic expiration share a lock, so a reconnect check
cannot race a newly started upload. Explicit reconnect, removal and shutdown
retain their cancellation behavior; the guard does not affect other printers.

Elegoo Centauri connections use the existing `ElegooLink` print host and vendor
page rather than an `IPrinterAgent`. Explicit connection correction retains the
saved local identity and plate aliases. CC2 HTTP identification is background
availability only; its vendor MQTT control/status lifetime is the explicitly
opened page. Each page's native IPC handler owns its connection snapshot and
retires queued work when hidden/replaced. This remains a first-class concurrent
background-control gap, not evidence of connected status from polling.

Cloud operations are selected separately using a provider key. `NetworkAgent` forwards a cloud request
to the matching `ICloudServiceAgent`, and forwards cloud camera operations with a device ID. The
printer agent receives a cloud-agent pointer through `set_cloud_agent()` when it is created, allowing
printer communication to obtain cloud tokens without depending on a concrete cloud implementation.

### `IPrinterAgent`

`IPrinterAgent` is the printer-facing contract. It covers:

- cloud-relay and direct-LAN message delivery;
- LAN connection, discovery, binding, and certificates;
- printer subscriptions and callbacks;
- print operations;
- filament synchronization;
- camera capability and local camera URL reporting;
- printer command methods.

Concrete built-in implementations include the Bambu wrapper, the native Orca/Moonraker path, and
other printer-agent implementations registered by the application. A printer agent may use either
the cloud agent, a direct LAN connection, or both.

### `ICloudServiceAgent`

`ICloudServiceAgent` owns authentication and services provided by a cloud backend. It covers login
state, tokens, user and printer lists, settings synchronization, model services, cloud messages, and
cloud camera operations.

Cloud camera operations are device-scoped:

- `get_camera_url(dev_id, callback)` obtains a stream URL for one device;
- `create_camera_signaling_channel(dev_id)` creates signaling for one device where the provider
  supports it.

This is separate from the local camera URL exposed by `IPrinterAgent`, scoped to
the selected agent instance, which represents one physical LAN printer connection.

## Agent registration and selection

`NetworkAgentFactory` maintains the printer-agent registry. Each registry entry contains an agent ID,
a display name, and a factory function. Built-in agents register during application initialization.
Python printer-agent capabilities register dynamically and contribute an agent ID and factory entry.

The selected printer preset contains the printer-agent choice. If no explicit choice is stored, the
application preserves the existing default behavior: Bambu presets select the Bambu agent and other
presets select the native Orca agent. When a preset is changed, `GUI_App` resolves the effective agent
ID, obtains the corresponding cloud agent and device-scoped instance through the registry,
and selects it in `NetworkAgent`. Devices inspection supplies an isolated config;
it does not load that preset into Prepare.

The registry rejects conflicting agent IDs. This matters for Python plugins because an agent ID is the
stable identity used by presets and device ownership; two enabled plugin capabilities must not claim
the same ID.

## Message and command flow

There are two low-level message paths:

- `send_message()` publishes a command through the printer's cloud relay;
- `send_message_to_printer()` sends a command directly to the printer over the LAN path.

Both paths accept a JSON string, quality-of-service and flag values, and return the existing network
status code domain. The agent owns the conversion from that JSON contract to its native transport.

The typed `command_*` methods are the application-facing convenience layer. The five generic defaults
currently implemented by `IPrinterAgent` construct the existing JSON dialect and route through the
same message path:

| Method | Default operation |
| --- | --- |
| `command_xyz_abs()` | Send `G90` for absolute positioning |
| `command_auto_leveling()` | Send `G29` for bed leveling |
| `command_go_home()` | Use the supported homing operation or send `G28` |
| `command_set_bed()` | Use the supported bed control or send `M140` |
| `command_set_nozzle()` | Send `M104` for nozzle temperature |

These are compatibility defaults for common printer workflows, not a guarantee that every firmware
implements every command identically. An agent can override a method when its protocol needs another
operation. For example, a Klipper configuration may use `BED_MESH_CALIBRATE` instead of `G29`.

The remaining common command methods default to `ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED` because their
existing behavior is vendor-specific or has no portable implementation:

- AMS RFID refresh;
- AMS calibration;
- AMS tray selection;
- camera start;
- axis control.

The methods remain on the common interface so an agent that supports them can override them explicitly.
`sequence_id` remains part of the command contract because `DeviceManager` creates and tracks it as
the command ID.

## Device ownership and stale responses

Printer-agent ownership is represented by `printer_agent_id` on device records and `MachineObject`
instances. The active agent ID is attached when a device is discovered, returned by a cloud list, or
reused after a preset switch. Local-machine configuration also persists the agent ID so a saved LAN
device is not silently reused by an unrelated agent.

Cloud printer-list responses carry three pieces of request context added by `NetworkAgent`:

```text
provider   cloud provider used for the request
agent_id   active printer agent when the request was made
generation machine-list generation when the request was made
```

`DeviceManager` accepts the response only when those values still match the current provider, active
agent, and generation. This prevents a slow response from the previous preset or provider from
repopulating the current device list.

Moonraker connections use one owned connection worker and retain only the latest pending request.
Reconnect and disconnect invalidate the previous generation immediately without joining transport
workers on the caller's thread. The connection worker joins the previous status worker before
clearing its cache or starting another stream. It checks the generation after HTTP responses and
before starting a stream; queued connection notifications check it again when delivered. Shutdown
invalidates notifications and joins both workers, so neither can outlive the agent. WebSocket retry
delays wake on cancellation. In-flight synchronous transport operations still depend on the
transport returning; this ownership rule does not make those operations asynchronously cancellable.

The shared Moonraker path (including its Snapmaker, Creality and Qidi subclasses)
requires a recognizable `/server/info` result and a nonempty object-status snapshot
before reporting Connected. It publishes that initial snapshot after the connection
notifications, so monitoring does not depend on the WebSocket's first reply. Queued
telemetry checks the connection generation again on delivery.

HTTP hosts retain the existing WebSocket status stream. HTTPS hosts use the existing
HTTP transport for fresh status snapshots, with a cancellable five-second wait between
requests, five-second connection and ten-second total request timeouts. Each successful
query replaces the polling snapshot. A failed read reports lost status; HTTP 401/403
reports rejected credentials and stops polling. The saved-device owner controls retries.
This path retains the HTTP wrapper's existing TLS policy and is not a WSS implementation.
Print entry points without an implementation return Unsupported; they must not report
successful delivery when no upload or print-start request took place.

The provider mapping is currently selected by `GUI_App`: the Bambu agent maps to the Bambu cloud
provider and other agents map to the Orca cloud provider. The generation check protects that existing
selection from races; it does not make cloud-provider ownership intrinsic to an agent. Cloud-printer
ownership and the broader Orca cloud services are therefore still separate architectural concerns.

## Python printer agents

`PrinterAgentPluginCapability` implements `IPrinterAgent` directly. The live capability object is
registered with `NetworkAgentFactory` and handed out as the printer agent when its agent ID is selected.
The plugin receives the selected `ICloudServiceAgent` through `set_cloud_agent()` just like a built-in
printer agent.

Python plugins must implement the core communication and lifecycle methods required by the interface,
including agent metadata, printer connection, discovery callbacks, and the two message-send methods.
Methods that are meaningful only to a particular printer are optional overrides where the C++ base
class provides a default.

All ten `command_*` methods are available in the Python binding and in the trampoline. Their override
status is intentionally optional:

- the five generic commands use the C++ default when Python does not override them;
- the five vendor-specific commands return `NOT_SUPPORTED` unless Python supplies an implementation;
- a Python implementation can replace either behavior for its own protocol.

The Python camera binding exposes HTTP, HTTPS, RTSP, and HTTP-snapshot modes. WebRTC remains a
built-in C++ camera mode, but is not exposed as a Python mode because the current Python capability
does not provide the corresponding cloud signaling-channel contract.

## Camera playback boundary

The camera stream mode describes how a stream is obtained; it does not by itself define ownership of
the wxWidgets view that renders it. `MediaPlayCtrl` selects and tears down the active backend, while
the wx parent owns the child window or renderer. This is important because a web view, native media
control, and frame-based/WebRTC renderer have different wx window-lifetime requirements.

Changing the selected device resets the status panel before displaying the new device, including
camera ownership, print progress, filament, temperature values and cached camera capability flags.
The native camera clears its last frame immediately when its target or transport changes. Its idle
view is vendor-neutral and is not a live camera image. Thumbnail requests use distinct identities;
resetting a job invalidates and cancels the previous request so late responses cannot replace the
current printer's thumbnail.

Cloud URL and signaling requests are routed through `NetworkAgent` to the cloud provider selected for
the device. Local URL requests use the selected device's printer agent. The native camera view
has its own target token, invalidated on selection changes; late URL responses
cannot populate a replacement view, including an A → B → A switch. Retained LAN
transports do not imply simultaneous camera playback.

## Compatibility constraints

The printer-agent boundary intentionally preserves several existing application contracts:

- Bambu-shaped JSON is still the shared command representation;
- existing network status codes are reused, with Orca-specific unsupported/capability errors added
  in the Orca-reserved range;
- `MachineObject` remains the shared device-state model;
- preset and local-machine data retain compatibility with the existing agent-selection behavior;
- Python plugins use the existing capability and pybind11 registration system.

The agent abstraction is therefore responsible for containing vendor differences, not for pretending
that all vendor protocols are identical. The planned Orca JSON dialect is the protocol-neutral command
model for the `DeviceManager`/agent boundary. Once it is introduced, Bambu-specific translation remains
inside the Bambu agent and the Orca agent's v1 sink adapter can be removed as a self-contained cleanup.

## Main implementation locations

- [`IPrinterAgent`](../../src/slic3r/Utils/IPrinterAgent.hpp) — printer-agent contract and generic command defaults
- [`ICloudServiceAgent`](../../src/slic3r/Utils/ICloudServiceAgent.hpp) — cloud service and per-device
  cloud camera contract
- [`NetworkAgent`](../../src/slic3r/Utils/NetworkAgent.hpp) — façade and dispatch between active agents
- [`NetworkAgentFactory`](../../src/slic3r/Utils/NetworkAgentFactory.hpp) — built-in and Python agent registry
- [`DeviceManager`](../../src/slic3r/GUI/DeviceCore/DevManager.cpp) — device ownership, filtering, and
  stale-response checks
- [`PrinterAgentPluginCapability`](../../src/slic3r/plugin/pluginTypes/printerAgent/PrinterAgentPluginCapability.cpp)
  — Python bindings
- [`MediaPlayCtrl`](../../src/slic3r/GUI/MediaPlayCtrl.cpp) — camera backend selection and playback lifecycle

## Flashforge delivery identity

Flashforge LAN discovery identifies a reachable device but does not verify its
physical model or installed nozzle. The user explicitly chooses the matching
model and nozzle profile when saving it; a later generic discovery result keeps
that choice. Changing a saved profile does not rewrite an existing plate's
edited setup or make an earlier slice valid.

Before the legacy Flashforge upload dialog or local API request, Porca checks
the selected plate's destination against the active connection and compares the
saved printer model and nozzle profile, plate setup, active editing setup and
the settings footer of that plate's actual G-code. An unknown saved model,
missing or stale G-code, or a known disagreement blocks delivery and directs the
user to choose the correct setup and slice again. The guard never changes
G-code or sends printer commands. A matching profile still does not verify the
nozzle mounted on the physical printer.
