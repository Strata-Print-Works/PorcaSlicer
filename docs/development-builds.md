# Local builds and offline verification

Use the guarded build command when sharing a configured build directory:

```sh
python3 scripts/build_porca.py OrcaSlicer slic3rutils_tests
```

It builds Release with six workers by default. `--build-dir` selects another
configured CMake directory; `--jobs` changes parallelism. The default directory
is `build/prototype`. The wrapper never launches the application or runs tests.
It prints the configured build type and stops before building a tree without a
Release configuration: single-configuration generators ignore `--config`, so
their `CMAKE_BUILD_TYPE` must already be `Release`.

An operating-system lock prevents two instances of the wrapper from building
the same directory. An interrupted build stops its owned build processes before
releasing that lock. Direct `cmake --build` and `ninja` commands bypass the lock,
so do not run them against the same directory concurrently.

For Ninja builds, the wrapper validates `.ninja_deps` before invoking the build.
It understands dependency-log version 4; another version is reported and left
unvalidated rather than blocking the build.
Use `--check-deps` to inspect it without running Ninja. A damaged dependency log
can make every invocation recompile otherwise current objects. **Even a Ninja
dry-run can truncate an invalid log**, so preserve a copy before investigating
with Ninja directly.

`--repair-deps` handles only redundant records that repeat an existing path with
the same ID. It validates the entire log, retains a timestamped copy under the
build directory's `.porca-build-backups`, and replaces the cache atomically.
Incomplete records, conflicting paths and unknown IDs are rejected unchanged.
Removing a duplicate cannot recover newer dependency records that Ninja has
already discarded; one rebuild repopulates that information.

Use `--dry-run` to inspect pending Ninja work after validation. The inherited
commit-hash check may still run on an otherwise unchanged build. Windows locking
and process-tree cleanup need validation on Windows; local macOS evidence does
not establish behavior on other platforms.

## Offline verification

Use an existing Release tree configured with `BUILD_TESTS=ON`, with dependencies
already installed locally. The runner needs Python 3.8+ and the built Catch2
executables; it uses only the Python standard library. From the repository root:

```sh
python3 scripts/verify_offline.py --list
python3 scripts/build_porca.py libslic3r_tests slic3rutils_tests porca_unit_tests fff_print_tests
python3 scripts/verify_offline.py all
```

Build only the targets needed for your selected groups. Both scripts accept
`--build-dir <path>`; the runner supports single-configuration Release trees and
the `tests/<suite>/Release/` layout of multi-configuration generators. It rejects
Debug-only trees and missing executables. Paths to binaries are absolute, so
invoking the script from another directory does not change fixture isolation.

| Group | Coverage | Target(s) |
|---|---|---|
| `devices` | Device session reuse, routing, invalidated callbacks and Moonraker workers with fake transports; in-memory Flashforge reply parsing; Bambu sessions run in `porca_bambu_worker` child processes loaded with a fake SDK library and no network | `porca_unit_tests`, `slic3rutils_tests` (builds `porca_bambu_worker` and the fake SDK libraries) |
| `layout` | Selection alignment math, density/rotation packing, exclusions, complete sets and cancellation | `libslic3r_tests`, `porca_unit_tests` |
| `quick` | Pure panel allocation, scope estimates, filament totals, slice queue and discovery policy; no application library linkage | `porca_unit_tests` |
| `persistence` | Plate setup capture/restore, credential stripping, 3MF round trips and compatibility | `libslic3r_tests` |
| `production` | Production part counts, review correction and undo, and their 3MF round trip | `libslic3r_tests` |
| `workspace` | Stage serialization, move/resize rules, physical bed display transforms | `libslic3r_tests`, `slic3rutils_tests` |
| `preview` | Panel allocation, projection/picking math, overlay input, scope estimates and filament usage aggregation | `libslic3r_tests`, `slic3rutils_tests` |
| `slicing` | Slice queue selection, cancellation and stale callbacks; does not execute the slicing pipeline | `libslic3r_tests` |
| `discovery` | Interface/probe policy, backoff, destination identity and in-memory DNS/SSDP/MQTT parsing/encoding | `libslic3r_tests` |
| `delivery` | Printer model and nozzle checks across the saved printer, plate and slice before sending, plus saved destination identity | `libslic3r_tests` |

Pass one or more names, for example `python3 scripts/verify_offline.py persistence discovery`.
`all` means all listed groups, not all repository tests. Each tag runs separately,
so a missing tag fails even if the other tags still match. Tests with multiple
listed tags may run in more than one check. `--dry-run` prints the selected tags,
sources and build targets without starting any subprocess or creating files.

The runner holds the build wrapper's lock through preflight and execution, uses
a fresh temporary working directory for each tag, randomizes test order and prints
the seed. It fails on nonzero exits, timeouts, incomplete reports, skipped/expected
failure cases and zero matching tests. Reproduce an order with `--seed <number>`;
`--timeout <seconds>` sets a per-tag limit (default 300 seconds).

Every run saves Catch2 XML, process logs and `summary.json` in a unique directory
under ignored `artifacts/verification/`; `--report-dir <path>` changes that parent.
Report filenames include the group, executable and tag so overlapping groups and
repeated tags in the lightweight and full suites retain separate results. The summary records commands, binary paths, sizes/modification times, seed, results
and inspected test source paths. An interrupted run remains incomplete.

The runner never builds, configures, downloads, installs packages or launches the
application. **Rebuild affected targets before testing:** the report describes
existing binaries and does not certify that they include unbuilt source edits.
The separate build step still requires a configured tree whose dependencies are
available locally; do not bootstrap dependencies while the network restriction
applies.

This is an explicit list of inspected tests, not an OS network sandbox. Before
changing or expanding a listed tag, inspect its helpers, startup hooks and called
production paths for network activity. The GUI-linked selections exercise math
without starting wx or a GL context. Discovery tests use in-memory packets and
fake timing; they do not open sockets or resolve printer names. The `devices`
group's `BambuProcessSessions` tag is the one listed check that starts other
processes: it spawns the built `porca_bambu_worker` executable as child processes
connected by OS pipes, each loading only the built fake Bambu SDK library
(`porca_fake_bbl_plugin`), which has no network code. Offline results
do not qualify live discovery, physical printing, native appearance or another
platform. Use the [feature map](feature-map.md) for additional coverage a change
may need.

The runner's own regressions and build-lock tests run without a C++ build:

```sh
python3 -m unittest discover -s scripts -p 'test_verify_offline.py'
python3 -m unittest discover -s scripts -p 'test_build_porca.py'
```

## Fast logic checks

For changes to the pure subset, use:

```sh
python3 scripts/build_porca.py porca_unit_tests
python3 scripts/verify_offline.py quick
```

`tests/porca_unit/CMakeLists.txt` compiles the existing filament usage, slice queue,
discovery policy, Preview allocation and scope estimate tests with only their small production
sources, Catch2 and cereal headers. It does not depend on the `libslic3r` or
`libslic3r_gui` libraries, the Python runtime copy, or app packaging. Existing
locally installed dependencies are still needed to configure the overall project;
this is an incremental target, not a separate bootstrap build.

The test sources remain owned by the core/GUI suites and are compiled there too.
Preview allocation math lives in `tests/slic3rutils/test_preview_layout.cpp`;
projection and picking remain in `test_camera.cpp`. The `preview` group retains
both. Scope estimate validation lives in `test_preview_estimate.cpp` and runs in
both groups. The `quick` group does not cover camera integration, rendered output,
workspace geometry, project compatibility or the actual slicing pipeline.

## Measuring incremental builds

Measure the configured Release target with `scripts/build_porca.py`; its elapsed
time includes dependency-cache validation and the build. Warm the target first,
then record a no-change build, a small source-file rebuild and a shared-header
rebuild. Keep the same worker count and record which target and file were used.
Timestamp-only touches can force these rebuilds without changing code; preserve
the original timestamps and restore them only if the file contents have not
changed concurrently. Do not delete build caches to simulate an incremental edit.

For Ninja, retain the build output and `.ninja_log` records in ignored
`artifacts/verification/`. Records provide individual compilation/link durations;
several outputs from one command share a record timing, so count that command
once. Parallel compilation durations overlap and must not be added as wall time.
The test executable's link step may also include Catch2 discovery and runtime
copying. Compare a lightweight target only for the behavior it actually covers;
full-library builds remain necessary for integration verification.

## Static analysis without dependency setup

`scripts/run_unit_tests.sh` can install numpy and run network tests.
`scripts/run_clang_tidy.sh` can fetch Git refs, install tools/dependencies and
configure a build. Its `--no-fetch` flag disables only the Git fetch. Neither is
an offline verification entry point under the current restriction.

For changed-line C++ analysis, use the existing Python driver with an already
prepared `SLIC3R_PCH=OFF` compile database, generated headers and local clang-tidy:

```sh
python3 scripts/clang_tidy_diff.py -p build-tidy --base HEAD --clang-tidy /absolute/path/to/clang-tidy
```

`HEAD` checks uncommitted changes; choose an existing local base revision to cover
committed branch changes. Use the version in `scripts/clang_tidy_requirements.txt`
for CI parity. If the local tool, database or generated headers are missing, report
that prerequisite instead of invoking an installer. The normal Porca database
in `build/prototype` has PCH enabled and is not equivalent for missing-include checks.

## Editing the Prepare toolbar without rebuilding

The current executable supports an optional local style file. Start a fresh
process with `PORCA_TOOLBAR_STYLE_FILE` set; an already-running process keeps its
original environment. On macOS, from the repository root:

```sh
open --env PORCA_TOOLBAR_STYLE_FILE="$PWD/resources/design-system/toolbar-dev.json" \
  "$PWD/build/prototype/src/OrcaSlicer.app"
```

Then edit and save that JSON file. While Prepare is visible, supported colors,
spacing, control and icon sizes, and radii are checked every 500ms. Active drags
defer updates. Invalid edits keep the previous appearance and log a diagnostic;
removing an override field restores its compiled default. No compilation or
restart is needed for subsequent valid edits. Fonts, tool behavior, icon paths,
other screens and substantial layout changes still require their existing
build paths. See [branding](HLSD/branding.md) for the reload contract.

Reloading also changes the overlay revision used by the unchanged-frame check,
so edits can appear while scene reuse and frame skipping are enabled.

Application launch and runtime validation remain subject to the current local
testing restriction in `AGENTS.md`; this command is a usage reference, not a
validation result.
