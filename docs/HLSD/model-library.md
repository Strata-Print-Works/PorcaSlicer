# Model library

Porca's model-library page keeps reusable models and their supporting files beside
the Prepare workspace. It is currently hidden from the tab bar. The native
C++/wxWidgets page is owned by MainFrame. Adding
geometry uses Plater's normal model-import path and an undo snapshot; opening a
3MF project uses the existing project-open and compatibility review. Browsing,
organizing and previewing the library do not change a plate's printer or settings.

## Catalog and originals

`Utils/ModelLibrary/Library` owns a SQLite connection and serializes access with a
recursive mutex. Its six numbered migrations retain Strata Library's schema,
including content and geometry hashes, annotations, collections, version stacks,
print history and watch settings. The chosen folder contains `library.db`,
`models/`, `thumbs/` and `backups/`. Porca remembers the chosen path locally and
does not open a personal Strata library implicitly. A newer schema is refused;
daily backups retain fourteen snapshots and pre-migration backups are retained.
The native SQLite and BLAKE3 sources are in `deps_src/library_storage`; no Rust
runtime, web application or separate library service is involved.

Imports stage copies inside the library, hash the source and staged bytes, and
commit catalog rows with the installed model directory. The source stays intact.
Folder and ZIP packs retain relative paths, recognized editable source files,
images and instructions. Unsafe archive paths, duplicate archive names, links and
oversized archives are rejected or excluded. A folder is a duplicate only when a
single saved model contains every selected file at its requested relative path
and the saved bytes still match. An unchanged source folder can grow in place;
editing or removing a previously imported file creates a separate version.

Organized-folder import previews additions, verified matches, possible versions,
unreadable items and excluded files before the user chooses categories. Each
category becomes a collection, including reused exact matches. Merging and
splitting move files transactionally and preserve annotations and memberships.
Restoring an old flattened pack requires its unchanged original ZIP and
unambiguous name/hash matches; it retains a recoverable directory if rollback
cannot complete. Merged and split packs retain their intentional layout.

Library-relative paths reject traversal and symlink components. Recoverable
platform Trash is used for removal, with no permanent-delete fallback. Download
cleanup is separately enabled: every source file, including hidden and unsupported
contents of a pack, must exist byte-for-byte in the library. Both copies are
hashed again immediately before trashing. Slicer-managed download folders are
excluded from cleanup. Archive and file metadata are treated as input data.

## Work and presentation

The panel owns a `BoostThreadWorker` for catalog, import, extraction, watching and
optional enrichment. UI completion callbacks check lifetime, selected library and
request generation. A separate owned worker renders interactive previews and
coalesces camera changes. Workers return pixels and plain data; wx controls and
bitmaps are created on the UI thread. The library uses mesh-only format readers:
background extraction must never construct Orca `Model`, `ModelObject` or
configuration objects with main-thread-owned identity counters.

STL, OBJ, PLY, AMF and 3MF supply mesh previews and geometry facts. 3MF readers
apply component and build transforms, units and repeated instances. STEP supplies
header metadata; G-code and binary G-code supply print facts and embedded images.
Embedded PNG/JPEG previews are validated, blank placeholders are skipped, and a
bounded software renderer supplies missing mesh thumbnails. Geometry fingerprints
use complete triangle sets, translation and proper axis-rotation normalization,
and coordinates rounded to 0.0001 mm. Reflections, size and repeated instances
remain significant. Files exceeding complete-parser/fingerprint limits are not
used for geometry matching. Interactive previews may sample triangles for speed;
this does not authorize approximate geometry matches.

The native grid/list, inspector, searchable pack tree, command palette, actions
and dialogs expose metadata editing, tags and rules, favorites, archive,
collections and saved searches, grouping, version stacks, print logs, file details,
cover selection, merge/split, cleanup review and external-file opening. External
slicer launch uses argument arrays, never shell interpolation. Porca's existing
theme and native widgets govern the page's appearance.

Automatic version grouping combines name/source evidence, complete geometry and
overlapping pack contents. Name revisions respect designer, part count and
physical dimensions; each revision is compared with the latest matching member
so gradual changes remain together. Existing manual stacks and explicit unstack
choices survive automatic organization. Rule changes remove obsolete automatic
tags while preserving user tags and deliberately hidden tags. URL query strings
and fragments do not contribute tag keywords.
Name families use a shared prefix across at least three distinct designs, counting
each version stack once. They are computed from the active catalog before search
filters so their labels remain stable while browsing a subset.

Watch polling is local filesystem work. It waits for stable files/folders,
ignores incomplete-download names, bounds each poll and import batch, and records
catch-up timestamps. Adding a watched folder does not import old contents until
Scan is chosen. Slicer download paths are discovered from local configuration and
registered once so a user's removed watch stays removed.

## Optional online work

Page details and AI tags are disabled by default and require a local Porca opt-in
as well as their catalog preferences. Page enrichment fills missing designer,
license, description and photos from supported Printables/Thingiverse pages.
AI tagging sends the documented model description fields and a bounded preview
using a vocabulary of rule and user tags. Responses are validated against the
requested model IDs and allowed tags. Deliberately removed tags remain excluded.
Anthropic credentials use the OS secret store and never enter the portable
catalog, activity messages or request error text. HTTP transport has verified TLS,
response limits, timeouts and cancellation; it resets inherited account headers.

## Verification boundaries

The `library` offline group tests imports, file protection, catalog persistence,
search, annotations, organization, cleanup proof, pack restoration, organized
previews, metadata, fake-clock watching and injected online responses. The
`library-formats` group tests mesh readers, component transforms and image
render/decode without wx, GL, sockets or printer commands. Native interaction,
focus, layout and app restart require separate visual review. Fake HTTP results do
not qualify live APIs, OS Trash behavior or Keychain prompts; a macOS build does
not establish Windows/Linux compilation or behavior.
