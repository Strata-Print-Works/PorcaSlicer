# Independent plate setups

New installations default to independent plate setups; a configuration upgraded from a build without the setting keeps shared setups until it is turned on. **Preferences → Control → Configure each plate independently** controls new projects; an explicit existing preference is preserved, and turning it on changes only an untouched new project, never the open one. Creating a stage or assigning a saved printer in Prepare also enables independent setups. New FFF projects apply the preference after their plate list has been reset, including startup when presets are already loaded. A project carrying saved independent setups enables the workflow when opened, regardless of the preference for new projects. Existing projects without the extension retain Orca's shared setup until independence is enabled. **View → Next plate / Previous plate** provides menu and keyboard access to switching in independent projects.

Each `PartPlate` owns a `PlatePrintSetup`: the complete resolved print, printer, filament and project configuration, plus the selected destination identifier. Its existing plate overrides, model membership, `Print`, G-code result and validity remain plate-owned. Shared preset definitions are still managed by `PresetBundle`; the bundle is the editing context of the selected plate. Switching captures the outgoing setup and loads the incoming setup through the existing preset loader before applying it to the incoming plate's `Print`. Unsaved setting edits therefore travel with their plate.

Prepare uses one sidebar with **Project → Stage → Plate → Object** scope tabs.
Project is always available; Stage requires selected plates in the same stage,
Plate requires one selected plate, and Object requires selected geometry. Clicking
a scope changes the editing target without switching the active printer or fitting
the camera. Deselecting a plate leaves the process controls available at Project
scope. Ordinary shared-setup Orca projects retain their native settings surface.

The process cascade starts with each plate's own process preset, then applies
project defaults, its stage defaults and explicit plate edits. Native object, part
and height-range overrides remain the final layer. Project and Stage share the
native process pages for numeric, boolean and enumeration settings; printer,
material/tool assignments, scripts, text/list settings and plugin configuration
remain available at Plate scope. Printer and filament controls are hidden in the
broader scopes. Mixed inherited values display as mixed rather than pretending
that the active plate represents the whole project. After an edit the sidebar
reports how many plates inherit it and how many keep lower overrides. Use inherited
settings removes the current scope's process overrides. Row reset controls return
safe process settings to their inherited values in the Project, Stage and Plate editors.

Existing resolved setups are migrated without changing values: the saved process
preset supplies the fallback, differences and existing explicit plate values remain
plate overrides. If that preset is missing, its existing values remain explicit.
The saved project carries each plate's fallback and overrides plus an optional,
versioned project/stage catalog on the first exported plate. Undo stores the same
cascade. Importing plates into another project preserves their resolved settings
as plate overrides. Stage membership changes resolve and validate the destination
cascade before committing. New copies retain their source plate's inheritance.

An edit prepares every resolved candidate before changing the project. It checks
each affected plate's printer and nozzle limits and reports dependent-setting
conflicts without automatic corrections. One edit is one undo snapshot. Only
plates with changed values lose their slice validity. The active process fields
refresh without loading printer/material presets or sending printer commands.
Selecting a scope alone neither edits settings nor makes the project dirty.

The active slice worker must be idle before the editing context can switch. Loading a setup suppresses background scheduling, preset invalidation and capture callbacks until the incoming context is complete. Import restores the final selected plate after object-list selection is rebuilt. Unsaved project state survives loading another plate's presets. Batch slicing continues through Orca's existing sequential worker, activating each plate's setup before slicing it. Since the scene keeps each instance's validity against its own bed, the worker temporarily excludes non-owned instances while copying its slice input, restoring those scene flags even if applying fails. Printer operations remain in Orca's existing device system; independent preparation does not establish concurrent mixed-vendor connections or qualify hardware delivery.

The slice split button defaults to the active plate. Its menu explicitly selects a plate, a named stage or the project; choosing a scope does not start slicing. A selected stage is retained by its stable identifier rather than following the active plate. Stage-menu slicing uses the same queue. The queue snapshots stable plate identifiers in workspace order, including stage members separated by other plates, and excludes empty jobs and jobs whose instances are all marked unprintable. Pending configuration edits are applied before checking freshness. Ordinary slicing reuses current results only when their G-code and preview assets remain usable; the explicit Reslice action recalculates the targeted FFF plates. Scoped progress counts the jobs requiring work. Starting a scope opens the Preview tab, including when all results can be reused. Preview uses the same Quiet Charcoal viewport and thin physical bed as Prepare, with coral controls and compact, labeled empty plate entries. SLA and G-code-only workflows retain their native paths, without forced scoped reslicing.

Scoped slicing activates one plate at a time and stops on the first failure, identifying the affected plate while retaining earlier successful results. Cancel invalidates the queue generation before stopping the worker. Each job has its own completion event and a generation check, so late callbacks cannot advance a cancelled or replaced queue. Print and export controls, including queued delivery callbacks, remain unavailable during a batch. Editing settings or resetting the project cancels the batch rather than applying changes to subsequent jobs unexpectedly.

Filament needed uses the selected plate, stage or project and reports grams and metres from current slice results, including modeled support and purge extrusion. Empty jobs and unused filament slots contribute nothing. Quantities are calculated from each plate's measured extrusion volume and slice-time density and diameter; matching slot numbers across plates do not imply matching materials. Aggregation requires matching material, finish, colour, brand, profile and diameter. Unknown identities stay separate by plate and slot, and uncertain virtual routing is reported as incomplete rather than guessed. Missing or stale slices and unavailable quantities are listed as needing attention; totals with incomplete contributions are labeled as lower bounds. Copy CSV exports the same scoped report. The report estimates sliced consumption and does not track remaining stock or calculate spool purchases.

The XYZ origin marker in Prepare and Preview follows the selected plate’s print-coordinate origin as that plate moves through the workspace. It is not positioned from the displayed bed mesh or its bounding-box corner; printer profiles can place their origin inside the bed. Design retains its modeling origin.

The desktop FFF plate grid reserves cells large enough for the largest bed encountered in the project, with at least 80 mm between columns and 100 mm between rows (or the inherited 20-percent gap when larger). The clearance accommodates the full title panel, side controls and bed edges. Command-line and SLA layouts retain the inherited grid. Enlarging a grid cell translates the objects with their owning plate's origin, preserving their physical coordinates. Each plate retains its own outline, exclusion areas, tool areas and height. Changing a printer does not scale or recenter objects. Bounds and colors for inactive plates come from their own setups. Scene refresh and switching do not trim other plates' painted assignments to the selected printer's filament count.

Removing or merging a filament first separates copies of an object that span the active plate and other beds. The active copies receive their own object configuration and painted assignments before the palette shrinks. The separation preserves every instance transform, including deliberate sinking below the bed, and belongs to the filament edit's undo snapshot. Membership and selection are rebuilt without invalidating unrelated plates; plates whose object ownership changes become stale.

Moving whole objects or instances across independent plates preflights their material assignments before committing geometry. Filament numbers are local to each plate. A unique complete identity with equal resolved filament settings can move automatically. A unique exact color with the same known material and diameter is also preselected, preferring the same profile when it disambiguates equal colors. Printer-setting variants are kept separate from the physical color identity. Variants and differences in destination temperature, flow or scripts do not force the user to choose the same color again: the chooser opens with those assignments selected and reports that destination settings will be used. Unknown materials, ambiguous duplicates, or different colors or diameters require an explicit choice from the destination's physical filaments. Virtual mixtures are not inferred from their display color. Objects outside a plate have no established source palette and require explicit choices. The chooser groups parts by their source and destination plates and asks once for each used source filament. Side-by-side swatches, profile names, material, diameter and hex colors show the change before it is accepted. Physical destinations are ranked by identical settings, matching material and diameter, matching profile, then perceptual color distance. Only unique identical or same-color/material/diameter matches are preselected; approximate colors, ambiguous duplicates and incomplete material details still require a choice. Changed colors, differing or unknown materials, and assignments that combine colors are reported in the chooser. Painted regions are retained, and object, part, height-range, support and per-feature filament references are remapped together. Destination process settings remain the destination's own settings; inherited source process settings are not copied.

The move draft leaves the model and undo history unchanged while a choice is pending. Cancel, Escape and closing the chooser leave the parts in their preceding positions. A changed project, printer setup, palette, plate placement or object state rejects the draft, as does slicing or another running task; a message explains the rejection so the parts can be moved again. An accepted move forms one undo transaction, including a position typed or reset in the move tool or set in the measure tool. A move that completes a larger tool action, such as aligning faces in the measure tool, joins that action's undo step. Copies whose assignments must change are separated before remapping, preserving the settings and painted assignments of copies left behind. Transformations, including sinking below the bed, are preserved. Only affected plates lose slice validity. Existing 3MF model and independent-setup records carry the remapped assignments without a new metadata version. Part-only edits cannot transfer ownership to another plate: the user selects the whole object to match its materials. Automatic arrangement retains its existing path; this transaction handles whole-instance translations through the native move command.


Independent projects have a shared workspace plane beneath their beds, with a quiet grid and colors following the application appearance. The plane is display geometry; it never participates in picking, plate membership or slicing. Its cached geometry expands around the plate layout.

Each plate has a rounded title panel on the shared workspace plane, separated from the rear bed edge by an 18 mm gap. The panel is 44 mm high and follows the bed's width between 180 and 280 mm, independently of the physical bed shape. A six-dot grip, number badge, name and optional saved-printer name or configured printer model identify the job; long names are ellipsized. The selected panel has a coral number badge and outline. The panel body and grip move the plate; clicking its bed selects the setup without changing the camera. The pencil and actions button have separate hit regions, with actions opening the native plate menu. Change printer opens the existing sidebar picker for that plate, expanding the sidebar and printer section when needed. Printer changes continue through the normal per-plate setup path. Existing controls beside the bed remain accessible. Label textures and hover states are cached across movement and refreshed when their content, selection, lock state or display scale changes. The selected bed also has a thin coral perimeter following its printable polygon. Labels and their picking geometry share the workspace plane, and native actions beside the beds use the same plane. Shared-setup projects use the same title styling and rename/actions controls, without a movement grip; their existing move-to-front control remains available. Shared-project titles scale uniformly into the existing row gaps without moving beds or models.

The lock control prevents workspace movement as well as automatic arrangement. There is no separate movement mode. Explicit camera-fit commands remain available. Motion translates the bed and all its instances together using full floating-point precision. Beds keep a five-millimetre separation, and moves are unavailable during slicing. The first move fixes the existing plates' absolute workspace positions, preventing later additions or removals from reflowing them; a new plate is placed to the right of the layout. A gesture forms one undo step. During dragging, the existing object and prime-tower render volumes translate with their bed; full scene validation, thumbnail regeneration, assembly-view and sidebar updates wait until release or cancellation. Bed geometry and picking remain current throughout the gesture. Workspace positions are included in undo snapshots and optional `orca_plate_workspace_position` 3MF metadata. Missing metadata retains the automatic grid. Only the moved plate's slice becomes stale, since Orca's cached print geometry uses workspace coordinates; the generated physical coordinates remain bed-relative after slicing again.

Arrange plates in either the plate menu or the empty-workspace menu places unlocked plates outside stages in rows sized to their physical bounds, then frames the workspace. The empty-workspace menu also offers adding a plate or stage, removing empty unlocked plates, and explicitly fitting all plates. Cleanup keeps the selected plate when every plate is empty and is one undo step. Locked plates and stage frames keep their positions. The spacing includes room for plate titles and side controls. Beds and their models translate together, preserving positions relative to each bed; changed plates need a fresh slice. Arrangement is one undo step and the resulting positions are saved with the project.

Shift-clicking a plate title or empty bed toggles it in a transient workspace selection. The active plate continues to own the settings sidebar; adding plates to the selection does not switch configurations or fit the camera. A plain title press preserves a group when that plate is already selected, while clicking another plate replaces it. Dragging any selected title translates every member together, preserving spacing and bed-relative model transforms. Locks, coordinate limits and collisions with stationary beds are checked for the entire group before movement. Source stage frames stay fixed during the drag, and an eligible stage drop assigns all selected beds together within the drag's single undo step. Escape or capture loss rolls the whole group back. Selection itself is not saved in the project; moved positions and stage membership use the existing project metadata.

Workspace stages are named organizational frames containing independent plates. They do not impose a print order, share settings, or send printer commands. A stage can contain different bed sizes and destinations, and several plates can target the same saved printer. New stage groups the active ungrouped plate when its enclosing frame fits without overlapping another stage; otherwise it creates an empty frame beside the layout. Add plate inherits the active job's setup, and duplicates retain their stage membership. The Prepare toolbar offers Add stage beside Add plate. Stage titles offer membership, Add plate, Rename, Fit and Ungroup commands, including on empty stages. Plate menus offer Move to stage, and plates can also be dragged between stages. Ungroup retains the plates and their models.

Each stage has a stable identifier, a name and an anchor. Automatic bounds leave 60 mm below and to the left of member beds, 80 mm to the right for plate controls, and 100 mm above for the plate title panels. Empty stages start at 400 × 300 mm. Moving, resizing or creating a stage preserves a 20 mm gap from other stages, including their title bars and empty areas. Plate assignments also check that their enlarged destination frame retains this gap. Older projects with overlapping stages remain editable: a move or resize may reduce each existing overlap, but cannot increase it or create another. Corner grips resize the area without moving plates or changing slice results; the native stage menu also offers width and depth fields. Each dimension has a 200 mm minimum, and a frame containing plates cannot shrink through their padded enclosure. Plate locks prevent group motion but allow frame resizing. A resized area persists relative to its anchor and expands if a member moves beyond it; the explicitly reserved space is retained. New stage plates use an 80 mm horizontal gap and 100 mm vertical gap, while existing plates keep their placement. The frame is drawn on the shared plane beneath the thin sheets, with small depth offsets preventing z-fighting. Its compact title panel attaches above the top-left edge within the existing 44 mm display margin. A larger name, movement grip, clean pencil icon and native actions menu stay grouped together; the panel grows with the name up to a capped width, and long names are ellipsized. Its fill and controls follow the application palette in both themes. The grip and free title area drag the stage. Cached titles and controls use the same perspective and world scale as the plate labels and remain attached during orbit, zoom and movement. Stage fitting includes the title bar, and the member enclosure reserves clearance for plate title panels on small beds. Display clearance does not enlarge physical bed collision bounds. Corner brackets are 20 mm long; plane controls retain an 18 DIP minimum screen hit radius when zoomed out. Dragging a title moves all members and their instances by the same displacement, after checking every member's lock, coordinate limits and collisions with outside beds. One drag is one undo step. Physical coordinates on each bed, printer assignments and settings remain unchanged. Like an individual bed move, a successful group move invalidates the moved slices because the native cache uses workspace coordinates. Membership, naming and ungrouping edits leave slice results valid. During a plate drag, its source frame stays fixed instead of stretching around the moving bed. The pointer chooses a destination across the frame or its title bar; a bed-center fallback also accepts a plate whose title is just above the frame or outside the viewport. Only destinations whose enlarged area preserves stage separation are eligible. A coral stage outline and release hint identify the accepted destination. The drop outline has its own geometry cache; entering or leaving a stage does not rebuild its surfaces, title bars or controls. Its last geometry stays cached while hidden, so re-entering the same unchanged stage reuses the GPU buffers. Releasing commits membership within the movement's undo step; a plate outside eligible stages becomes ungrouped. Leaving a stage preserves its preceding area, including when its last plate leaves, so the empty board does not jump or collapse. Fit is explicit, and selecting or adding an independent plate does not fit the camera automatically.

Escape or mouse-capture loss cancels a workspace drag or frame resize. Resize cancellation restores the preceding explicit area or automatic sizing without changing plate placement. Cancelling an imported overlapping stage movement restores its captured position through a path that preserves physical movement checks while bypassing the overlap-improvement rule. With slicing idle, cancellation restores placement and prior free-layout metadata when the original coordinates can be recovered. Slice validity, apply status and progress are restored only when the affected plate's coordinates and settings, including the canvas configuration, match the captured state. A previously pending background update resumes after cleanup. A cancelled plate or stage drag leaves no undo step once its placement is restored; the step remains only when rollback is refused because slicing started during the drag, or when the layout cannot be fully restored. Rejected moves and simple clicks do not add a movement snapshot.

Undo snapshots include the stage catalog and per-plate membership. The optional `porca_workspace_stages` metadata stores the catalog on the first serialized plate, including empty stages. Automatic frames retain version 1; catalogs containing explicit resized areas use version 2, and the importer accepts both. Older Porca builds reject version 2 instead of silently losing the resized area; `porca_plate_stage` stores membership by stable identifier. Import checks versions, unique identifiers, valid names and anchors and membership references before restoring plates. Stages organize the workspace only, so the project reader accepts a member plate saved without its own printing setup; that plate keeps its stage and the project's shared settings. Unsupported or malformed stage data rejects full project restoration while geometry-only import can recover the models. Existing files without these additions retain their layout and setup behavior. Other slicers can ignore or discard the stage metadata.

Undo snapshots carry the setup with the plate. Project 3MF archives store versioned `orca_plate_print_setup` metadata alongside each plate in `Metadata/model_settings.config`; workspace positions use `orca_plate_workspace_position`. These are optional additions to Orca's existing project metadata, without a new file extension or a required core 3MF extension. The readable configuration is supplemented by numeric values for enum, floating-point and bed-point options, since Orca's text formatting can round numbers or lack enum names. Live dynamic routing and local connection addresses, credentials and certificate paths are excluded at serialization and import, including configs supplied directly to the serializer. Unknown print options, unsupported versions, incomplete required settings and malformed bed dimensions fail project restoration instead of substituting defaults. Geometry-only import skips these additions and can recover models even when the plate setup is unsupported. Saved workspace positions are applied after all plates have been constructed, before rebuilding model membership.

FFF project saves normalize owned model instances to the conventional Orca/Bambu grid for the shared printer configuration, without modifying the live workspace, mesh data, Z positions, assembly transforms or slice results. As in Orca, the grid uses the shared bed's size in whole millimetres, and readers apply the same truncation to fractional beds. Every plate uses that one grid, so a plate whose independent bed is larger than the shared bed can overlap its neighbours in the conventional coordinates. Porca restores each plate from its recorded file origin instead; another slicer sees the overlapping conventional placement. The optional version-1 `porca_plate_layout` metadata records that file-coordinate origin and the Porca workspace position for independent plates. It replaces the older workspace-position key on normalized saves, so readers that ignore the new metadata see conventional model placement rather than applying a Porca position to those coordinates. On desktop import, explicit plate membership translates instances from the file origin into their saved Porca position or the roomier automatic grid. Older Porca files with saved workspace positions retain their layout; ordinary Orca/Bambu files use their conventional grid as the source. Repeated copies of one object on the same plate retain every instance reference. Unknown layout versions reject full restoration while geometry-only import remains available. Another slicer may discard these optional records when saving, in which case Porca rebuilds its automatic layout on reopen. This preserves conventional homogeneous plate placement; another slicer’s shared printer configuration cannot represent every heterogeneous Porca setup.

Opening or dropping a 3MF offers **Import plates and settings**, **Import geometry only**, and **Replace current project** when Orca's saved project-load preference asks: always, or when relevant and models are already open. Otherwise the saved preference applies directly, so loading everything replaces the project and loading geometry only imports the models as Orca does. Import appends jobs beside the existing workspace rather than resetting it. Each imported plate owns its resolved printer, filament and print setup, including Porca's optional independent setup, destination, name, lock and stage records. Shared Orca settings become the base setup of each incoming plate, with its existing plate overrides retained. Source stages receive new identifiers so importing the same project twice cannot join unrelated stages. Model instance references and per-layer commands are shifted to the appended indices, and material identities are isolated from existing materials. Imported slice caches are discarded because their workspace coordinates change; existing plates and completed slices retain their state. One imported file forms one undo step. Save continues to use the current project's filename.

A project supports up to 72 plates across creation, import and restored layouts. The bound also covers plate labels, arrangement indices and bed picking. Imports exceeding it fail before changing the current workspace.

Both append choices translate each incoming bed's models from its file origin into the appended layout without rotating, scaling, recentering or changing Z. Geometry-only import uses the current printer and print settings on the new plates, retaining valid optional spatial metadata even when print settings cannot be restored. It strips object and part print overrides, variable layer settings and custom layer commands. When the shared project settings contain options Porca does not define, or values it must replace, settings import pauses for review: it lists the missing settings by readable name and each replaced value beside the one that will be used. Importing with the available settings then matches Orca's project open, leaving unknown options out and applying the replacements; importing models only re-reads the archive as a geometry-only import, so the choice is made before the workspace changes. Porca's own per-plate setups stay strict and still reject unsupported settings. Prusa settings remain unsupported: importing its recovered beds requires accepting the current printer and print settings. Declared empty beds remain present. Published selective settings and arbitrary foreign slicer settings are not treated as complete job setups.

PrusaSlicer 2.9 FFF imports into an empty workspace recover beds from the declared wipe-tower bed indices, source bed shape and Prusa’s square-shell grid. Each instance is assigned by its transformed bounding-box center, then translated to the destination plate without scaling, recentering or changing Z. Empty declared beds survive. The selected Porca printer and edited setup are copied to these plates; incompatible Prusa print settings remain excluded and the import warning still applies. Missing, malformed, unsupported or ambiguous layout data retains geometry-only import. Importing models into an existing workspace does not replace its plates. Recovered projects save through the same conventional Orca coordinates and optional Porca metadata described above.

Orca-family 3MF import identifies structured projects by their project and model
settings entries as well as the recognized application tags. An Elegoo, Creality,
Snapmaker or other fork's name does not make the reader split shared instances,
bake their transforms or rename their objects. Generic 3MF geometry retains its
existing import path; identifying a familiar container does not establish that
all of its slicer's print settings have equivalents.

Project JSON accepts strings, native numbers and booleans, including mixed numeric
arrays. Renamed settings with equivalent meanings pass through the common legacy
mapping. An unsupported value is isolated to its setting instead of aborting the
remaining settings. Opening a project and importing plates both offer a review of
unavailable and adjusted settings before applying them, with Cancel and models-only
alternatives. Models-only reading skips object, part and layer print overrides.

When a foreign project uses nil for a setting Porca stores as a non-nullable
vector, project import resolves it. Native nullable settings keep their nil values.
A nil object or part override for a non-nullable setting inherits the corresponding parent slot when the vector
sizes match; layer ranges load after project and object settings regardless of ZIP
entry order. All-nil overrides remain inherited. When a matching parent is absent,
missing slots use the first explicit value, never an invented zero. A vector for a
scalar setting keeps its first value. Backfills, differing scalar values and
materialized inheritance appear in the review. Materialized values preserve the
load-time selection but no longer follow later parent edits; layer-range resolution
uses the object and project, not a part-specific override. These are explicit
compatibility adjustments, not full Bambu semantic equivalence. Porca's complete
per-plate records retain strict validation, and ordinary preset editing and CLI
parsing do not enable project recovery.

The version-1 setup also carries an optional printer display name beside its destination identifier. Earlier version-1 files without a display name still open. A printer missing from this computer's saved devices does not prevent editing or erase the intended destination on subsequent capture and save. An actionable notification offers Add printer; selecting another printer replaces the intended destination, and explicitly removing a saved printer clears that association. Access codes and local connection details are resolved exclusively from the computer's saved devices.

Embedded preset files are also written from a copy with connection addresses, credentials and certificate paths removed; saving leaves the in-memory preset's connection settings intact.

This extension is specific to this fork. Other slicers and older Orca builds do not restore these independent setups and may discard the optional data when saving. Their ability to open the geometry is not evidence of project fidelity. Print-file metadata is enriched from the relevant plate rather than the last active printer.

The implementation retains Orca's editing tools and interface. It is an experiment in preparation and project ownership, not a completed MultiSlicer interface replacement. Virtual-mixture reconciliation, heterogeneous project-wide arrangement, concurrent printer connections and physical send qualification require separate verification.

Stage appearance, scoped slicing controls, native keyboard interaction, DPI behavior and runtime behavior on macOS, Windows and Linux require platform observation; they have not been established under the current local validation restriction. Source review and offline checks do not establish live connections, concurrent control or physical printing.

Inactive plates retain their physical bed meshes, resolved from their own custom model or saved printer model in independent projects, or the shared printer configuration in shared-setup projects. Display caches are separate from the active printer and are rebuilt only when bed settings change; movement updates the placement without reloading the mesh. Missing or invalid models fall back to that plate's printable polygon. Printable areas and slicing inputs are unchanged by physical bed rendering.

Quiet charcoal follows Porca’s appearance tokens. The selected bed is brighter than inactive beds, which have opaque darker surfaces and fainter grids. Physical bed meshes retain their XY silhouettes, with their displayed vertical span transformed into a one-millimetre sheet whose underside rests on the shared plane. This is display geometry, not a vendor thickness specification or a change to the printable plane. Polygon fallbacks use the same selection colors. Switching plates swaps the surface tones without changing geometry or camera. Stage fills and title bars blend gently into the plane instead of forming raised platforms. Titles, corner brackets, hover marks, and their hit tests use consistent plane heights with small depth offsets to prevent z-fighting. Shared-setup Orca projects use the same appearance and physical bed cache, resolved from their shared printer configuration. Styling does not convert them to independent setups or change their models, setup ownership or slicing inputs. With Satin plastic appearance enabled in Prepare, shadow receivers follow each bed's displayed polygon or physical model, and the shared plane receives shadows at its own height. The optional [studio rendering](prepare-studio-rendering.md) changes only the view.

Printer discovery and saved devices
----------------------------------

The Prepare sidebar's Add printer action and the Devices page share a scan-and-add dialog. Discovery selects at most four up, running, multicast-capable IPv4 LAN addresses, excluding loopback, point-to-point, VPN, bridge, virtual and Apple peer-to-peer interfaces and link-local addresses. It fails closed when no eligible interface exists. On macOS, Snapmaker, Creality, Moonraker and OctoPrint use mDNSResponder through DNS-SD requests scoped to the selected interface indices; Creality's long service types are queried through PTR/SRV records. Bambu SSDP and Flashforge use owned sockets and a shared budget of ten raw probes per second. Flashforge broadcasts come from actual netmasks. Probe sockets bind their egress interface explicitly to avoid VPN policy routing (`IP_BOUND_IF` on macOS, `SO_BINDTODEVICE` on Linux, `IP_UNICAST_IF`/`IP_MULTICAST_IF` on Windows); an interface that cannot be bound, or whose index cannot be resolved, is skipped. On Linux kernels before 5.7 binding requires `CAP_NET_RAW`, so unprivileged discovery finds nothing there and direct-address entry remains the route. A process-wide scan lease is held until cancelled workers have finished and sockets or DNS-SD references are released. The portable raw mDNS fallback also owns its sockets and has an absolute deadline. Results arrive without blocking the GUI and remain available through retries. Direct-address entry handles machines that do not announce themselves. Discovery never guesses a nozzle: the exact printer/nozzle profile is shown before adding, and an unknown model requires an explicit profile choice.

Saved devices are identified by vendor and serial, with address and port as the fallback when no serial is available. A saved device can be the destination of several independent plates in the current project. Adding an existing device through discovery remains idempotent and selects an existing plate; the explicit New plate action creates another job for that printer. The Devices page lists every assigned plate and opens the chosen plate only on request. Adding another device preserves existing bed positions and creates a bed beside them. The first device can reuse an empty, unassigned initial plate. Profile changes are applied after capturing the outgoing plate setup, without invoking the library's save/discard dialog. The add dialog stays open and regains focus after the main frame's queued updates.

The application configuration retains saved device identities and local access codes separately from project settings. Explicit serial-based scan destinations retain their device identifier. Address-based destinations and bare native device identifiers use a random UUID; the local `scanned_printer_destinations` section maps discovery keys and native identifiers to those portable identities. Bare native identifiers are kept local because they can be DNS hostnames as well as numeric addresses. Address edits retain the UUID and old alias. GUI import migrates legacy local destinations, including printers missing locally, while retaining their display names; the serializer rejects unmigrated address-bearing destinations. The aliases and addresses stay on this computer. Selecting or reopening a plate restores connection settings from its locally saved device or native alias. Another computer without that mapping retains the plate's geometry, settings and intended destination but needs an explicit printer association before connecting.

Connection checks run independently of the selected printer and of the legacy Device tab's current agent. Each saved device gets a bounded read-only authentication/status check: Bambu MQTT over TLS, or the existing vendor's local HTTP endpoint. At most two checks may run concurrently, with one new check per second. Checks pause during discovery and skip every retained native control attempt; those attempts supply their own live, connecting or failure state. Successful checks, and replies from a printer that is busy or not ready, repeat every thirty seconds; failures back off from five to 320 seconds, while rejected credentials retry after five minutes. Shutdown cancels and joins pending checks. Transient failures settle before becoming offline, and rejected credentials remain actionable. Periodic availability remains separate from persistent native control sessions; it does not establish a control connection. No upload, motion, heater, or start command is sent by these checks.

Bambu printers present a device-issued TLS certificate that no local authority verifies, so the availability check trusts it on first use: the first authenticated check stores the leaf certificate's SHA-256 fingerprint in the local `scanned_printer_certificates` section, never in projects, presets or exported profiles. Later checks compare it before sending the access code; a different certificate fails as a changed printer identity, not as offline. The pin survives address edits (DHCP). Re-entering the access code, changing the serial or removing the printer clears it. Native control sessions use the vendor's connection support and are outside this pin.

The Prepare printer chooser leads with saved printers and their model pictures. Selecting one assigns it to the active plate; re-selecting the same device preserves that plate's edited nozzle, materials and print settings. Selecting a model or profile instead prepares the plate without a saved destination.

The Devices page keeps a saved-printer list beside the selected printer's control panel. Printer names and status occupy separate columns; telemetry updates change cells without replacing the selected row. The detail header identifies the printer and its connection, while plate actions sit below monitoring. Each native page or website owns its scrolling. Add printer opens the scanner. Edit printer changes the local name, address and access code, updating the destination display name on every assigned plate; a plate named after the old printer name follows the rename. Remove printer forgets the saved connection and clears every assigned plate's destination while retaining its setup and models. Selection survives list updates by device identity. Open plate uses the selected entry in the assigned-plate chooser and is disabled when no assigned plate exists or slicing is busy.

After initialization, saved native printers establish independent control/status
sessions at a bounded rate without selecting a machine, opening a detail view,
loading a preset or assigning a Prepare destination. Each receives a snapshot of
its own locally saved connection and profile identity. Healthy sessions survive
view changes. Offline attempts back off; rejected credentials require an edit or
explicit Reconnect. Missing profiles and connection support remain explicit.
Connected sessions whose telemetry has expired are retired and retried with the
same bounded backoff as timed-out attempts. A new attempt has thirty seconds to
establish status before retirement. Fresh telemetry clears an earlier connection
error; a successful transport callback alone does not establish fresh status.
Availability checks continue for hosts without a native control integration and
must not be interpreted as persistent control sessions.

Connection checks retain structured failure reasons for connection timeouts, refused connections, secure-handshake failures, invalid responses, rejected access, HTTP errors and a reachable service whose printer is not ready. A Flashforge check reads the machine state from its `/detail` reply: only a ready printer is Available. Any other reported state is Busy, and a finished print asks the user to clear the bed and confirm it on the printer; a reply without a state keeps the printer Available. These state names come from independent API documentation and are unqualified on this firmware. A busy or not-ready printer is excluded from planning. HTTP transport errors without a response remain a general address/network failure because the HTTP wrapper does not expose a typed transport error. Raw server bodies and error strings are not retained in UI state. The picker shows green only for live telemetry, gray for availability without a live session or a reachable printer that cannot take a job, and red for a failed connection. Failure text remains visible during bounded retries.

CC2 automatic discovery uses its UDP JSON protocol, independently of generic
Moonraker/mDNS. A candidate's reported LAN/protection flags guide setup and
never grant a connected state. Cloud-mode candidates remain visible. Installed
nozzle selection is explicit for new Elegoo printers. Validated serial continuity
merges DHCP changes without changing the saved local key, credentials, chosen
profile, user name or custom HTTP port. Discovery alone never creates/selects a
plate or replaces its setup. See the [control coverage](device-control-coverage.md)
for scan bounds and the remaining view-scoped Elegoo control lifetime.

Edit printer can explicitly correct a saved connection type without deleting its
entry. The local `identity_key` retains credential and destination associations
when the vendor changes; rediscovery preserves it. Elegoo routing requires an
explicit choice and a Centauri Carbon or Carbon 2 model from the already selected
profile. Connection correction changes local host routing, not the plate's
model, nozzle, materials, geometry or slice result. Generic Moonraker entries
are never migrated merely because their slicing profile is Centauri. HTTP
identification is bounded background work; a loaded Elegoo page is view-scoped
and does not establish persistent background control.

Control inspection uses a separate copy of the saved device's printer configuration. DeviceManager and MonitorPanel use that copy for the selected view while Devices is visible; printer selection and telemetry skip Prepare's filament/preset synchronization. Hiding Devices releases the inspector configuration and restores the active plate's connection, retaining independently owned native sessions. Equal parameters reuse a pending or connected session and retry an offline one; a rejected-credential attempt is reused until its parameters change, while explicit Reconnect intentionally resets it. Each saved Bambu LAN device owns a non-GUI worker process with isolated SDK state, callbacks and private command routing; account services remain in the application. Selecting a view does not transfer a LAN connection. Inspection never loads a printer preset, changes the Prepare destination, changes plate geometry or captures a different device into a plate. Project loading also skips the inherited connected-printer preset reconciliation while inspecting or restoring independent setups, so a retained monitor cannot replace the imported model, nozzle or filament configuration. One native panel is selected at a time; Bambu, Moonraker, Creality, Snapmaker and Qidi sessions retain independent transports and status when not selected. [Device control coverage](device-control-coverage.md) documents SDK qualification limits, routing and missing controls.

Bambu and registered native printer agents reuse MonitorPanel. A native view selector replaces its redundant printer picker and navigation sidebar; the camera and printing progress lead the detail view, with the existing temperature inputs and a compact live filament overview beside them. The native movement, fan, speed, light and full AMS controls expand below monitoring through Show controls; collapsing their parent keeps background telemetry from revealing hidden controls. Printer parts, options and calibration remain in the native control header. The legacy standalone monitor retains its original layout. The saved list uses a native single-selection data view for keyboard and accessibility support. Missing Bambu connection support shows an explicit setup action within the selected printer's detail area without blocking the saved list or scanner. Other supported browser-based printers use their local web control page. Registered non-Bambu native adapters also expose Printer website, keeping vendor actions reachable without replacing native monitoring. Their Bambu-only Storage, Firmware and Troubleshooting tabs are omitted because those adapters do not implement those protocols. A printer website loads only when its view is selected, so native monitoring does not start a hidden vendor page or camera. Cached websites are device-owned; editing/removing a device closes its old page, and hidden pages cannot dispatch native script messages. Unsupported controls are stated explicitly. Control actions remain in the existing native/vendor panel; discovery and the connection checker send no physical commands.

Perspective zoom
----------------

**Preferences → Control → Camera → Keep perspective consistent when zooming** optionally moves the camera along its view axis with a 45-degree vertical field of view. Zoom retains its pixels-per-world-unit meaning at the target plane, so fitting, panning and cursor-anchored navigation share the existing scale. Clipping adapts to camera distance and permits close inspection inside the overall workspace bounds without pushing the camera back outside the entire layout. The preference defaults off to retain Orca’s field-of-view zoom, applies immediately to perspective canvases and leaves orthogonal views unchanged. It is local application state, not project metadata.

Preview navigation and estimates
--------------------------------

Preview groups the plate list beneath the project's stage headings, retaining empty stages and an Outside stages group. Selecting a plate keeps the camera and uses Orca's existing slice-selection event. Selecting a stage shows an estimate for its currently valid, nonempty slices and suppresses individual G-code. The scope view reports coverage and excludes unsliced or invalidated results. Filament mass and cost use each result's own slot densities and prices; a stage does not reconcile slot numbers across different printers. Missing measurement metadata is shown as unavailable. Stage time is explicitly the sum of plate estimates, not an elapsed completion prediction or print order.

Editable FFF Preview keeps the stage and plate navigator, workspace and slice inspector visible. Settings start collapsed on each application launch. The Print settings toggle above the plate list expands the native settings sidebar to the left. Its highlighted state and directional chevron indicate whether settings are open. Preview retains that choice while switching tabs within the session. The settings sidebar reuses Prepare’s printer, filament and process editor for the active plate; selecting another plate restores its own setup. Stage and project overviews remain available without hiding the settings. SLA, G-code-only and exported read-only workflows retain their native paths. Preview’s settings visibility is independent of Prepare’s saved sidebar preference. The existing sidebar shortcut and menu action toggle the current view’s settings. The Summary tab leads with time, filament weight, cost and per-filament model, support, flushing and tower use. The Line types tab uses bounded columns for time, usage and visibility; distance and percentage remain available on hover. Existing alternate coloring controls remain available, with supporting estimates under Estimate details. Navigation, the inspector, sliders, orientation controls and source drawer share one pixel allocation that scales with the viewport. Preview’s orientation cube and view controls sit at the top-left of the workspace, clear of the plate navigator; their popups open beside them. The G-code inspector starts collapsed to a bottom bar on each application launch. Its menu action, shortcut and full-width bar toggle the same state. Expanding offers the full file through a virtualized scrolling list, reads only the visible bounded range from the existing memory-mapped file, keeps its scroll position in double-precision line coordinates so small trackpad movements remain responsive deep in large files, uses a dedicated bundled monospace font and highlights the current toolpath line. Moving the toolpath selection follows its source line; manual source scrolling is preserved until that selection changes. No file parsing is performed while collapsed. Scrolling the source or inspector and dragging the source scrollbar update only the overlay and reuse the unchanged 3D scene when scene reuse is enabled. Preview’s ambient-occlusion pass copies and shades only the exposed scene rectangle, with a depth-sampling guard band beneath the opaque panels; the full-canvas projection and final appearance remain unchanged. Preview reserves screen space through a uniform clip-space camera transform after the base projection, preserving model proportions, camera target and the projection used for screen picking. The full GL viewport is retained; no child window is overlaid on the canvas.

Selection alignment and plate filling
-------------------------------------

Prepare offers Align objects and Fill plate beside Arrange. Alignment operates on
whole instances belonging to the active unlocked plate. Its directions follow the
physical bed axes, independent of camera orientation. Edge and center alignment
use the selection bounds, the plate bounds or an explicitly chosen stationary key
object. Distribution preserves the outer entries in positional order; exact-gap
spacing starts at the first entry. These are XY translations only. They preserve
scale, rotation, height and multipart relationships; normal scene validation still
reports overlaps and parts outside a nonrectangular printable outline.

Fill plate repeats the selected instances as complete sets. Spacious, Balanced,
Tight and Max request 5, 3, 1 and 0.2 mm gaps respectively; Custom permits explicit
spacing from 0.2 to 1000 mm. Max retains a nonzero gap between separate pieces and
does not promise an optimal packing. Existing placement remains fixed unless
Rearrange selected objects to fit more is selected. Optional rotation compares
unchanged orientations, reversed directions and additional 45-degree Z rotations.
It never mirrors, tilts or scales a model. Arrange with preview uses the same
placement checks for the selected objects or the active plate without making
copies. The inherited arrangement remains available, including its belt-printer
path; the new preview packer does not handle belt beds.

Optimize plates is available from the empty workspace and from a stage menu. It
repacks every eligible printable piece in scope onto compatible beds, adds plates
when the pieces need more room and removes emptied source plates. A plan can still
improve the layout without reducing the plate count. Objects retain their size,
print face, Z placement and assigned filaments. Cancel leaves the project untouched;
Apply rechecks the workspace and commits placement, plate creation/removal and
optional workspace arrangement as one undo step.

Pieces already on plates retain their resolved printer, process, material and
stage setup. The settings comparison excludes local connection details,
preset-difference bookkeeping and the workspace-wide wipe-tower position arrays;
actual print settings must match before pieces share a bed. Each destination's
tower footprint, exclusions, skirt, brim, support allowance and tool clearance
remain protected during packing. Objects with different assigned filament sets
are packed separately; an object using several filaments stays whole. Locked
plates and jobs with fixed or unprintable obstacles remain intact.

Workspace optimization includes printable pieces not assigned to any plate by
default. Its dialog explicitly chooses an unlocked plate whose printer and
filament settings those pieces will use. The preview states the piece count and
chosen settings source. Missing filament slots block the plan rather than being
replaced. Turning off inclusion leaves those pieces where they are and reports
how many were omitted. Stage-scoped optimization includes only that stage's jobs.

The bounded search samples the full physical bed with multiple object orders
and optional Z rotations. It does not claim a global optimum or reconcile
materials between different printer setups. The preview shows each changed bed,
added plates and emptied plates. New plates use the corresponding job's setup and
are placed together beside existing beds. Optional workspace arrangement can
reposition surviving unlocked plates outside stages; locked and staged plates
stay put. Face changes remain unavailable because the top-down preview cannot
show their bed-contact and support consequences.

For printing by layer, body-collision validation confirms overlapping convex
outlines against the actual projected meshes. This allows separated pieces to
nest in concave outlines without a false collision warning. Exclusion areas,
prime towers and sequential-print toolhead clearances retain their conservative
checks.

Workspace optimization can instead plan the surviving jobs across saved FFF
printers. It uses each printer's selected profile, compatible quality and
filament presets, and physical printable area when checking whether objects
fit. It first combines compatible source plates, then also tries to repack
their individual objects across printer-sized beds and consolidate lightly
used beds when that can reduce the number of stages. If a whole source job
cannot fit a destination, the object-level pass can use additional plates.
It also separates objects assigned different filaments or colors, while an
object using several filaments remains whole. Every assigned filament slot
must retain its material type, preset and color when a job changes printers.
Flashforge discovery provides a brand rather than a verified model or nozzle.
Its saved model comes from the person's explicit printer-profile choice and a
later scan preserves that choice. The planner excludes a saved Flashforge whose
physical model is still generic or disagrees with its chosen profile. This does
not establish which nozzle is installed on the machine.
Jobs with fixed objects can move only
when their setup and destination remain unchanged. Jobs are assigned in numbered
stages with at most one plate per printer in each stage, prioritizing jobs with
few eligible printers. An offline printer may still receive a future job;
planning never starts a
connection or a print. By default the planner keeps the plate count at or
below the fewest-plate plan; an explicit option favors fewer stages when that
requires extra plates. The preview shows the resulting layouts, destination
and stage for each job. Apply updates the surviving plates' printer setups,
bed shapes, object positions and stage memberships as one undo step. Existing
locked and empty plates stay where they are. A stage is an organizational
frame, not an automatic print queue or a print-time estimate. Reachability
does not confirm loaded filament, actual nozzle or physical readiness; these
must be checked before printing.

Placement uses projected printable geometry and actual per-plate origins rather
than workspace grid strides. Polygon containment and separation checks apply to
every candidate, including fills exceeding 100 copies. Bed exclusions, the prime
tower, wrapping/calibration reservations and IMEX zones remain unavailable.
Brim/support allowances and sequential clearance can increase the requested gap;
skirt clearance reduces the usable boundary. These conservative envelopes do not
replace slicing validation of generated supports or toolpaths.

A worker consumes copied model/configuration state and returns placements for a
native top-down preview. The bounded search compares candidates without modifying
the live project and reports when a search/copy limit was reached. Cancellation
leaves the project untouched. Apply verifies the original plate, settings, models,
transforms and painted assignments, then creates a single undo snapshot. Copies
retain the selected instance's complete transform and object settings; other
instances of the source object are not cloned or moved implicitly. Printer
assignments, bed positions and camera remain unchanged. Resulting geometry uses
normal project persistence without additional metadata.


CC2 filament synchronization captures the active plate's identity and full edited
setup before its read-only request. Results are discarded after a plate, setup or
connection change. Known materials and colors sync automatically using an installed,
compatible plain generic preset. Printer-specific generics take precedence over
the library fallback, including when hidden from the normal picker. Equally preferred
matches, unknown materials and missing colors require review; the dialog lists only
unmatched slots and allows skipping them. Explicitly empty trays are ignored.
Synced entries are appended without replacing existing assignments, and repeated
syncs do not duplicate the same preset and color. Unrelated plates retain their material lists. Explicit
same-model nozzle correction changes only the affected destination's nozzle and
preset identity, preserving process edits and placement while invalidating its
incompatible slice.
