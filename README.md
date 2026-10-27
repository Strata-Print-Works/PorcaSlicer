# Porca

Porca is a multi-printer desktop slicer based on OrcaSlicer. Prepare work across multiple printers in one shared workspace, with each plate keeping its own printer setup, materials, print settings, and slice result. Beds stay at their physical scale; moving a plate keeps its models in place on that bed.

The project develops the existing C++/wxWidgets application and retains its mature slicing and printer tools. It is a development fork, and its builds and release notes must be distinguished from upstream releases.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="resources/images/porca_logo_dark.svg">
  <img src="resources/images/porca_logo_light.svg" alt="Porca" width="440">
</picture>

![A large project spread across named print beds in Porca's shared workspace](docs/screenshots/multi-plate-workspace.jpg)

*Keep a whole project's jobs in view. The overview above and the colored stage detail below were captured on October 17, 2026; material transfer on October 20. Unless labeled historical, the other screenshots and GIFs show the running macOS development build on October 27. Earlier captures have some older controls. Production counts are manually entered demonstration data, not evidence of completed prints.*

## What Porca adds to OrcaSlicer

Porca builds on Orca's slicing, calibration, modeling and printer tools. The additions below focus on preparing several jobs, organizing work across printers, and keeping each job's settings intact. Features describe this development branch; availability depends on the build and printer integration.

### Prepare each plate independently

- **A complete setup for every plate.** Keep separate printers, nozzles, materials, edited print settings, destinations and slice results in one project. Switching plates restores that plate's edits.
- **Different beds in one workspace.** See beds at their true relative scale, with their physical shapes and printable boundaries. Changing printers preserves model size and placement.
- **Direct plate movement.** Drag a title or grip, or select several plates and move them together. Models stay in place on their physical beds, locks protect the layout, and a drag is one undo step. Selecting a plate does not jump the camera.
- **Named stages.** Group jobs in movable, resizable frames. Rename stages, move plates between them, and retain empty stages for later work.
- **Shared settings where useful.** Set project or stage process defaults while preserving plate and object overrides. Mixed values remain visible, and changes are checked against each affected printer.
- **Room for larger projects.** Work with up to 72 plates, arrange beds into tidy rows, and remove empty unlocked plates. Save their names, locks, positions and groups with the project.

![A selected Connectors plate inside a named stage, with movement grip, title actions and colored parts](docs/screenshots/stage-and-plate.jpg)

*Named stages keep related jobs together; each plate keeps its own setup and placement.*

#### See independent plates, stages and settings in action

**Each plate has its own printer, materials and process settings**

![Each plate has its own printer, materials and process settings](docs/screenshots/independent-plate-setup.png)

**Move a bed while keeping its parts on the bed**

![Move a bed while keeping its parts on the bed](docs/screenshots/movable-beds.png)

**Drag a plate, then undo the move**

![Drag a plate, then undo the move](docs/demos/move-plate.gif)

**Keep related jobs in a named stage**

![Keep related jobs in a named stage](docs/screenshots/named-stages.png)

**Set defaults across the project**

![Set defaults across the project](docs/screenshots/project-settings.png)

**Set defaults for a stage**

![Set defaults for a stage](docs/screenshots/stage-settings.png)

**Switch between project, stage and plate settings**

![Switch between project, stage and plate settings](docs/demos/settings-scopes.gif)

### Pack parts and plan work across printers

- **Align, distribute and space objects.** Align to the bed, selection or a stationary reference object; distribute evenly or choose an exact gap.
- **Fill a plate.** Repeat selected parts as complete sets, with adjustable spacing and optional rotation. Preserve existing placement or allow selected parts to be rearranged.
- **Review arrangements before applying.** Preview packing changes and accept them in one undo step. Cancel leaves the project intact.
- **Optimize across plates.** Repack compatible parts, add beds when needed and remove emptied source plates. Locked jobs remain protected. Mesh-aware collision checks allow valid nesting of concave parts.
- **Plan across saved printers.** Propose compatible destinations using bed size, printer profiles and material assignments. Group jobs into numbered stages with at most one job per printer in a stage. Favor fewer plates, or explicitly favor fewer stages when that needs extra plates.
- **Keep material assignments meaningful when moving parts.** Match material identities and colors across palettes, review unresolved choices, and retain painted regions and part overrides. Cancel or undo the whole move.

![The material-transfer dialog asks which destination filaments should replace the source assignments](docs/screenshots/material-transfer.jpg)

*Review filament choices before moving parts to another plate. Slot numbers alone do not establish a material match.*

Planning does not connect to printers or start prints. Stages organize work; they are not an automatic dispatch queue. Physical readiness, installed nozzles and loaded materials still require checking.

#### See alignment, fill and packing previews

**Align, distribute or choose an exact gap**

![Align, distribute or choose an exact gap](docs/screenshots/align-distribute.png)

**Choose how to fill a plate**

![Choose how to fill a plate](docs/screenshots/fill-plate.png)

**Review the copies and effective clearance before applying**

![Review the copies and effective clearance before applying](docs/screenshots/fill-preview.png)

**A plate filled with nine copies**

![A plate filled with nine copies](docs/screenshots/filled-plate.png)

**Apply the fill preview**

![Apply the fill preview](docs/demos/fill-plate.gif)

**Choose packing options across plates**

![Choose packing options across plates](docs/screenshots/optimize-plates.png)

**Review a proposed five-plate layout**

![Review a proposed five-plate layout](docs/screenshots/packing-preview.png)

**The workspace after applying the layout**

![The workspace after applying the layout](docs/screenshots/optimized-workspace.png)

**Apply the reviewed packing plan**

![Apply the reviewed packing plan](docs/demos/packing-preview.gif)

**Options for planning across saved printers**

![Options for planning across saved printers](docs/screenshots/printer-planning.png)

*The saved-printer image shows planning options. This capture session did not produce a compatible saved-printer assignment for the example project.*

### Keep several projects open

- **Project tabs.** Switch between projects without reloading them. Each retains its camera, selection, editing state, undo history, settings and slice results.
- **Slicing across projects.** Queue work and view another project while results stay with their originating job. Hidden slicing that depends on plugins currently stops for review.
- **Save All and conflict checks.** Save open work together, detect externally changed files, and prevent one open project from overwriting another's file.
- **Startup restoration and recovery.** Reopen saved projects after quitting, or recover unsaved projects independently from local checkpoints. Recovery does not resume printing or restore the full undo history.

#### See multiple projects open together

**Keep a small slicing study beside a larger project**

![Keep a small slicing study beside a larger project](docs/screenshots/project-tabs.png)

**Switch between projects and their retained slice views**

![Switch between projects and their retained slice views](docs/demos/project-tabs.gif)

**Save all open projects from the File menu**

![Save all open projects from the File menu](docs/screenshots/save-all.png)

**Choose startup restoration and automatic backups**

![Choose startup restoration and automatic backups](docs/screenshots/project-recovery-preferences.png)

*The preferences capture shows restoration and backup controls; it does not demonstrate recovery from a crash.*

### Slice, inspect and compare

- **Choose the slicing scope.** Slice or reslice one plate, a named stage or the project. Reuse valid results, see scoped progress, and retain completed results if a later job fails.
- **Compare slices.** Compare the active result with its preceding slice or another plate's retained result. Inspect time, material and layer-count differences, changed settings, and extrusion-path overlays at a chosen physical height. Comparison history lasts for the session.
- **Estimate filament across jobs.** Report grams and metres for a plate, stage or project, including support and purge. Combine matching materials rather than matching slot numbers, identify incomplete totals, and copy the report as CSV.
- **A clearer Preview.** Navigate stages and plates, inspect time, weight and cost, and break down consumption into models, supports, flushing and towers. Expand print settings directly beside the preview.
- **Full-file G-code inspection.** Open a collapsible source drawer, follow the selected toolpath line, and scroll large files without loading every line into the display.

#### See slicing, estimates, source inspection and comparison

**Choose plate, stage or project slicing**

![Choose plate, stage or project slicing](docs/screenshots/slice-scopes.png)

**Review time, material and cost estimates**

![Review time, material and cost estimates](docs/screenshots/preview-summary.png)

**Break down usage by print feature**

![Break down usage by print feature](docs/screenshots/preview-line-types.png)

**Edit print settings beside the preview**

![Edit print settings beside the preview](docs/screenshots/preview-settings.png)

**Follow a toolpath into the G-code source**

![Follow a toolpath into the G-code source](docs/screenshots/gcode-inspector.png)

**Inspect layers and open the G-code drawer**

![Inspect layers and open the G-code drawer](docs/demos/preview-inspection.gif)

**Estimate filament in grams and metres, with CSV export**

![Estimate filament in grams and metres, with CSV export](docs/screenshots/filament-requirements.png)

**Compare estimates and changed settings**

![Compare estimates and changed settings](docs/screenshots/slice-comparison.png)

**Compare time and material by print feature**

![Compare time and material by print feature](docs/screenshots/comparison-print-features.png)

**See shared and changed toolpaths at a physical height**

![See shared and changed toolpaths at a physical height](docs/screenshots/comparison-toolpaths.png)

**Scrub the comparison height and inspect changed settings**

![Scrub the comparison height and inspect changed settings](docs/demos/compare-slices.gif)

### Manage and monitor saved printers

- **Scan, add and manage printers together.** Discover supported LAN printers or enter an address, choose the actual profile and nozzle, and add several printers without reopening the dialog. Adding a printer creates a plate; adding it again reuses an assigned plate.
- **Find the right destination.** A searchable chooser leads with saved printers and model pictures. A printer can have several jobs, and the Devices page can open the chosen assigned plate.
- **Monitor-first controls.** Camera and progress lead the selected printer's detail view, with temperatures and live filament information alongside. Expand native controls below or open supported vendor websites.
- **Inspect without changing Prepare.** Looking at another printer does not replace a plate's setup, materials or destination.
- **Clear connection state.** Distinguish connecting, fresh telemetry, availability checks, offline devices and rejected credentials. Supported native integrations retain device-owned sessions when the displayed printer changes; Bambu LAN sessions use separate worker processes.
- **Elegoo Centauri Carbon 2 additions.** Dedicated discovery, explicit connection correction and reviewed CANVAS filament synchronization. The added print-start path is restricted to single-filament jobs on printers reporting no CANVAS.

Discovery and status checks do not establish print readiness. Native controls and connection ownership vary by integration; see [device control coverage](docs/HLSD/device-control-coverage.md). Offline checks do not qualify live firmware or physical print delivery.

#### See printer selection and search

**Browse printer presets with model pictures**

![Browse printer presets with model pictures](docs/screenshots/printer-chooser.png)

**Find a saved printer by name**

![Find a saved printer by name](docs/screenshots/printer-search.png)

**Filter the saved-printer chooser**

![Filter the saved-printer chooser](docs/demos/find-printer.gif)

*Search uses the existing printer list. The displayed status is captured app state; this demo does not establish fresh telemetry or print readiness.*

#### Historical captures: adding and listing printers

**Add another printer without closing the dialog — October 12, 2026**

![Add another printer without closing the dialog — October 12, 2026](docs/screenshots/add-printers-archive.png)

**Earlier saved-printer list — October 13, 2026**

![Earlier saved-printer list — October 13, 2026](docs/screenshots/saved-printers-archive.png)

*These older development captures have connection addresses hidden. The add dialog includes test fixtures. The list predates the current Monitor layout and connection-state wording; its “connected” count does not prove simultaneous control sessions. Current camera, Monitor and live discovery behavior are not demonstrated in this gallery.*

### Track usable parts through production

- **Targets for each part.** Follow needed and usable quantities with visual part cards grouped by stage and plate.
- **Record partial successes.** Review which parts came out well, record prints started elsewhere, or mark a plate done. Sending a job does not count as producing a usable part.
- **Correct results.** Keep print history, revise accepted quantities and undo a specific mark-done action without erasing other runs.
- **Carry progress with the project.** Save targets and results in the 3MF, with local journal recovery for unsaved production updates.

#### See production targets, results and history

**Track how many usable parts are still needed**

![Track how many usable parts are still needed](docs/screenshots/production-tracking.png)

**Review usable quantities for each part**

![Review usable quantities for each part](docs/screenshots/record-print-results.png)

**See progress after recording a completed plate**

![See progress after recording a completed plate](docs/screenshots/production-completion.png)

**Review history, correct a result or undo a mark-done action**

![Review history, correct a result or undo a mark-done action](docs/screenshots/production-history.png)

**Record a plate as done and update the part totals**

![Record a plate as done and update the part totals](docs/demos/production-progress.gif)

*These production examples use manually entered demo results. No printer was commanded to heat, move, upload or print during capture.*

### Combine and share projects

- **Import plates and settings.** Append jobs from another project while preserving existing work. Incoming jobs retain supported independent setups, names, locks and stages.
- **Review compatibility changes.** See unsupported settings and adjusted values before accepting an import, or recover geometry without importing print settings.
- **Broader project recognition.** Recognize Orca-family project containers from additional forks and recover supported PrusaSlicer multi-bed layouts. Recovering geometry does not imply equivalent print settings.
- **Portable work, local credentials.** Save intended destinations without exporting connection addresses, passwords, access codes, API keys or certificate paths, including in embedded presets. A missing local printer does not prevent editing or erase the intended destination.

#### See project import and compatibility review

**Choose plates and settings, geometry only, or project replacement**

![Choose plates and settings, geometry only, or project replacement](docs/screenshots/import-project.png)

**Keep the existing cube plate beside the imported jobs**

![Keep the existing cube plate beside the imported jobs](docs/screenshots/imported-plates.png)

**Append a project’s plates without replacing existing work**

![Append a project’s plates without replacing existing work](docs/demos/import-plates.gif)

**Review unavailable and adjusted settings before importing**

![Review unavailable and adjusted settings before importing](docs/screenshots/import-compatibility.png)

*The compatibility example deliberately contains an unknown setting and an unsupported infill value. This is the actual import dialog produced by that example file; either import choice can be canceled before changing the workspace.*

### Make everyday editing clearer

- **Porca's native appearance.** Coral accents, warm light and charcoal dark themes, updated typography, compact tools, and clearer printer and material controls.
- **Experimental material finishes.** Preview approximate satin, matte, silk, galaxy, carbon-fiber and wood surfaces from assigned filament information, with studio lighting and workspace shadows. These are visual estimates, not measured material properties.
- **Consistent-perspective zoom.** Optionally move the camera closer or farther while keeping its field of view fixed.
- **Plate-aware Select All.** Select the active plate's models first; quickly repeat to select the whole project.
- **Copilot host extensions.** Plugins can inspect live settings, geometry, materials and slice results, offer guarded setting edits, and point to relevant settings or calibrations. The Copilot plugin is a separate installation.

#### See appearance, camera options and the Copilot extension

**Inspect parts with material lighting and workspace shadows**

![Inspect parts with material lighting and workspace shadows](docs/screenshots/material-appearance.png)

**Choose realistic lighting and enhanced material surfaces**

![Choose realistic lighting and enhanced material surfaces](docs/screenshots/material-rendering-preferences.png)

**Keep perspective consistent while zooming**

![Keep perspective consistent while zooming](docs/screenshots/camera-preferences.png)

**Zoom out and back in with a fixed field of view**

![Zoom out and back in with a fixed field of view](docs/demos/perspective-zoom.gif)

**The separately installed Copilot extension and its chat action**

![The separately installed Copilot extension and its chat action](docs/screenshots/copilot-plugin.png)

*Material rendering is an approximation. The Copilot screenshot shows the installed extension only; chat, suggested edits and model responses are not demonstrated here.*

### Additional development work

Current development also includes **Snapmaker U1 send-time toolhead assignments and selective flow calibration**, with checks against reported materials and nozzles, and **removal of the Orca Cloud dependency** while retaining local presets and installed plugins. These changes are not a promise of availability in an existing preview download; check the release's source revision and notes.

CAD/Design, customizable keyboard shortcuts, section views and the advanced infill tools already in the imported Orca base remain inherited capabilities. They are not counted here as Porca-exclusive additions.

## Builds and previews

[Build results](https://github.com/Strata-Print-Works/PorcaSlicer/actions/workflows/build_all.yml?query=branch%3Aporca) include macOS, Windows and Linux downloads. Pushes to `porca` and pull requests targeting it run the builds and tests on GitHub-hosted runners.

The **Porca preview release** workflow assembles a draft from one successful build run, with checksums and its source commit. Preview packages are not Developer ID signed/notarized on macOS or certificate signed on Windows. See [build and release instructions](docs/development-builds.md#github-builds-and-preview-releases).

## Work on this repository

Start with [agent instructions](AGENTS.md), the [feature map](docs/feature-map.md), and [local build and offline verification instructions](docs/development-builds.md). The [independent-plate design](docs/HLSD/independent-plate-setups.md) describes project compatibility and current workflow limits.

The current local app is built from this checkout with:

```sh
python3 scripts/build_porca.py OrcaSlicer libslic3r_tests slic3rutils_tests porca_unit_tests
```

The build target (`OrcaSlicer`) and local macOS build directory (`OrcaSlicer.app`) keep their inherited names; the distributed macOS app is `Porca.app`. The app's displayed name, installer package name and icon are Porca. The user-facing release notes are in [resources/porca-release-notes.txt](resources/porca-release-notes.txt). This repository does not advertise upstream binaries as Porca downloads.

## Project files

Porca saves normal `.3mf` projects. Its optional metadata carries independent plate setups and workspace positions. Other slicers may open the geometry without retaining that Porca metadata. Keep a Porca copy when moving a project through another slicer.

## Provenance and license

Porca is based on [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer), which builds on [Bambu Studio](https://github.com/bambulab/BambuStudio), [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer), and [Slic3r](https://github.com/Slic3r/Slic3r). The inherited source and resources retain their copyright notices and license information. Porca is licensed under the [GNU Affero General Public License, version 3](LICENSE.txt).

The inherited pressure advance calibration pattern includes work adapted from Andrew Ellis' generator and Sineos' Marlin generator under GPLv3. The optional Bambu networking plugin uses non-free Bambu Lab libraries; its availability and terms are separate from Porca's source license.
