# Slice comparison

Preview's **Compare slices** opens a read-only comparison of the active plate's
completed FFF slice with its preceding retained slice or another plate's last
retained completed slice. Later edits on that other plate do not overwrite the
retained result. Opening the window and choosing a comparison do not activate a plate,
load presets, edit the project, invalidate a result, slice or contact a printer.
The action is unavailable while slicing or while the active result is stale.

The window shows estimated time, filament mass and layer counts beside their
differences. Print features breaks down filament and extrusion time by role;
extrusion time excludes travel, heating and preparation. Settings lists the retained values that changed. Toolpaths overlays
shared paths, paths unique to either result, and paths whose printing changed;
a height slider selects the physical Z plane. Zooming the overlay changes only
its display. A different layer height remains a separate plane rather than being
paired with a neighbouring layer. Placement relative to each physical bed is
preserved; there is no automatic model alignment.

## Retained results

`SliceComparisonHistory` is owned by Plater. Stable plate identities associate
current and previous results independently of plate order. Successful completion
captures an immutable summary before the result can be reused by the next slice.
Repeated completion of the same parser revision does not create another history
entry. Cancellation and failure do not replace a completed result. Resetting or
opening a project clears the session history; comparisons are not project data.

A snapshot owns its estimates, settings and compact extrusion paths, without
holding pointers into a mutable G-code result. The history has a 128 MiB target
budget and evicts the least recently recorded other plates when necessary. Each
capture retains at most 500,000 paths. Above that bound it discards all paths and
marks their comparison unavailable; it does not present a sampled overlay as
complete. Estimates and settings remain usable. Retained references keep an open
comparison stable if history entries are evicted.

An explicit slice captures the Print's applied full configuration and the sliced
model's object, part and height-range overrides. Connection secrets and preset
bookkeeping are excluded. A loaded slice without captured settings retains no
configuration: the window reports that its settings are unavailable. It never
assigns current presets to an older file. Preview can retain a loaded, parsed
result without changing its plate. Other plates are looked up by stable identity
when selected, and availability is checked again after modal events.

## Measurements and paths

Total time uses the parser's normal-mode estimate. Mass uses its total measured
volume per filament and that filament's density, including support and purge.
Missing or invalid density makes mass unavailable. Loaded files without captured
slice settings cannot distinguish declared densities from parser defaults, so
their mass remains unavailable. Material slots are local to a
plate; the path comparison uses a digest of captured material identity and
slice-time metadata instead of treating matching slot numbers as matching
materials. Unknown identity is kept distinct between parser revisions.

G-code preview positions include the plate's workspace origin. Capture subtracts
that origin to compare physical bed coordinates. Synthetic internal preview moves
are excluded. Heights use the parser's print-height annotation, falling back to
move Z, rounded to 0.001 mm. Belt and machine-frame transforms do not provide this
flat layer view, so their individual-path comparison remains unavailable.

Path endpoints are quantized to 0.01 mm and matched in either direction. Repeated
moves remain separate. Matching geometry with different speed, material,
extrusion role, width, height or flow is marked as changed printing. The overlay
is a view of extrusion paths; it does not establish print quality or equivalence
of travel order, acceleration, temperatures or printer commands. Settings and
estimates remain separate evidence alongside the overlay.

The core comparison and history have no wx, GL or transport dependencies. The
snapshot adapter reads existing parser output and applied settings. The native
window uses the existing design tokens, controls and DPI-aware dialog lifecycle.
`verify_offline.py comparison` covers pure comparisons, snapshot measurements and
an actual pair of local slices; rendered appearance and native interactions
require the app build and visual review.
