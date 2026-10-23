# Porca Slicer — agent instructions

## Product direction

Porca is our OrcaSlicer desktop fork, focused on preparing work across multiple printers in one shared workspace. Bring the useful MultiSlicer workflow into Orca: each plate is its own editable job, with its own printer configuration, bed, materials, print settings and slice result. Preserve Orca's mature slicing algorithms, editing tools and printer controls while making that workflow easier to use.

Develop the existing C++/wxWidgets application. Evolve the UI incrementally from the approved layout and stay close to Orca for now. The earlier standalone MultiSlicer application is workflow reference and prior work; its Rust/Tauri/React architecture and paid proprietary application brief do not govern Porca. Monetization is unsettled. Do not introduce payment, licensing gates or a new application shell as assumed requirements. Retain inherited licenses, copyright notices and provenance.

## Experience we are building

- **Independent plates:** switching plates restores that plate's edited setup without overwriting another plate's settings, material assignments or completed work. Changing printers must not silently scale or recenter models. Workspace motion preserves their placement on the physical bed.
- **A shared workspace:** show beds together at their true relative scale on a quiet, flat plane. Keep every bed's physical shape visible. The active plate has a clear cue; inactive beds fade into light gray without becoming generic squares.
- **Direct movement:** plates are movable through a visible grip or title unless locked. Do not require a separate movement unlock mode. Selecting a plate must not jump or fit the camera. A drag is one undo step, and layout survives save/reopen.
- **Easy printer management:** scanning, adding, editing and removing printers should be quick and clear. Adding a printer gives it its own plate; adding it again reuses that plate. Keep the add dialog available for consecutive additions and in front of the application.
- **Obvious monitoring and control:** Devices is the saved-printer list, with the selected printer's detail panel beside it. Use the approved Monitor first layout: camera and progress lead, temperatures and live filament information sit alongside, and full native controls expand below. Keep Orca's existing controls and supported capabilities accessible. Inspecting a printer must not change the Prepare destination or another plate's setup.
- **Honest device state:** distinguish saved, connecting, connected, offline and rejected credentials. Do not invent capabilities, guess an unknown nozzle or imply that a discovered device is ready to print. The current native panel controls one selected printer; periodic checks for several saved printers do not prove simultaneous control connections.
- **Portable projects:** use normal `.3mf` projects with versioned optional metadata for independent setups and plate positions. Restore empty plates, names, locks, edited settings and intended destinations. A missing local printer must not prevent editing or be forgotten on the next save. Keep addresses used for connection, passwords, access codes, API keys and certificate paths local, including when exporting embedded profiles.
- **Careful compatibility:** ordinary Orca projects retain their existing behavior. Unsupported project settings must be reported instead of silently replaced with defaults; geometry-only import can recover models. Other slicers may ignore or discard Porca metadata, so opening geometry is not proof of full project fidelity.

Read [the independent-plate design](docs/HLSD/independent-plate-setups.md) for the current implementation and its limits. Local HTML studies in `mockups/` record the approved UI direction; they are layout references, not a mandate to replace the native application. Treat material reconciliation across different palettes, heterogeneous arrangement, simultaneous mixed-vendor control and physical print delivery as work requiring their own implementation and validation.

## UX copy

- Write user-facing copy from the user perspective: describe purpose, action or outcome.
- Never expose or summarize implementation structure, domain models, schemas, components, data objects or feature inventories in UI copy.
- Avoid noun-list subtitles such as “Colors · Layers · Objects.”
- Omit supporting copy when it adds no useful human message.
- During UI review, flag violations as **domain-model leakage**.

## Current local validation restriction

The user reported repeated Wi-Fi driver crashes on this Mac and explicitly restricted testing. Until the user changes that restriction:

- After each completed native UI change, the user authorizes and expects a Release rebuild and restart of Porca for visual review. Save the current project first and use this checkout's current app at `build/prototype/src/OrcaSlicer.app`; do not ask again for routine restarts. This launch authorization does not permit network tests, printer commands, or launching an older build or another fork as a workaround.
- Do not run tests or scripts that send network packets on this Mac. Use static analysis, builds and pure/fake tests that never open real sockets.
- Authorization to edit code, commit, push or move files does not lift this restriction.
- Offline checks do not establish working live discovery, a resolved driver crash or physical printer qualification. Report those limits clearly. macOS build evidence does not verify the Windows or Linux build.

Preserve the discovery hardening: eligible physical LAN interfaces, bounded probe rates and polling, cancellation, owned sockets and workers, and complete cleanup. Prefer system discovery services where available while retaining supported platform fallbacks. Slicing or connection success alone is never permission to send motion, heating, upload or print-start commands.

## Workspace and Git

- This Mac checkout is `/Users/peter/code/experiments/porca-slicer`. Work in this repository; the former MultiSlicer prototype paths have moved. Leave the sibling MultiSlicer and original Orca checkouts alone unless the user requests work there.
- Preserve existing Git changes before editing or restructuring. Keep Porca's Git object storage independent of the original checkout.
- `origin` currently points to the local OrcaSlicer repository; `upstream` is the original public Orca remote. Inspect remotes before pushing. A local push is not a GitHub publication, and pushing to upstream requires explicit user direction.
- `artifacts/` holds app bundles, saved projects, device data, backups and verification records. `mockups/` holds local UI studies, and `base/` holds generated test presets. These directories and build outputs are ignored. Keep credentials, personal printer data and optional vendor binaries out of source commits.

## Build and verification

Build Release using the guarded wrapper. On this Mac the configured tree is `build/prototype`:

```bash
python3 scripts/build_porca.py OrcaSlicer libslic3r_tests slic3rutils_tests porca_unit_tests
python3 scripts/verify_offline.py all
```

For another configured tree, pass `--build-dir <path>` to both scripts. Local builds sharing a configured tree must use the wrapper; direct `cmake --build` and Ninja commands bypass its lock and must not overlap.

The verification runner only runs existing binaries and explicitly listed offline test groups. It never configures, builds, installs dependencies or launches Porca. Rebuild affected targets first; results describe the binaries tested, not proof that they match unbuilt edits. Use `--list` for groups or `--dry-run` to inspect a selection. See [local builds and verification](docs/development-builds.md) for prerequisites, cache diagnostics and live toolbar styling.

For panel allocation, filament totals, slice queue or discovery policy logic, build `porca_unit_tests` and run `python3 scripts/verify_offline.py quick` first. This small target reuses pure tests without linking the application libraries; full integration coverage remains in the existing groups.

Do not run blanket suites under the current restriction. `scripts/run_unit_tests.sh` can install dependencies and run network tests. `scripts/run_clang_tidy.sh` can fetch and install even when used for static analysis; `--no-fetch` alone does not make it offline. Use an existing compile database and local tools as described in the build guide.

Catch2 conventions and test placement are in [tests/AGENTS.md](tests/AGENTS.md). Rebuild after moving a checkout: some fixtures have source paths compiled into the executable. Add targeted format and actual slicing regressions when required, after inspecting their setup and dependencies for network activity. The runner does not replace those additional checks.

## Documentation

- Docs live in `docs/`; the high-level design of a subsystem goes in `docs/HLSD/<subsystem>.md`.
- Describe the design as it stands — what the subsystem does, why it exists, and the constraints that shape it. Not the route that got there: no phases, task lists, status markers, or "before/after this PR" framing.
- Planning and investigation output (brainstorms, superpowers design and plan docs) stays in `docs/superpowers/`, which is gitignored. Never commit it.
- Write a doc only when the design is not evident from the code, and when a change invalidates an existing one, update it in the same PR.

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Include what you use: include the header for every symbol a file uses, and keep headers compilable on their own. Never rely on the precompiled header or a transitive include. The `clang-tidy` CI job enforces this on changed lines; local checks must follow the offline workflow in the build guide.
- Parallelization via TBB — be mindful of shared state

## Key Entry Points

- App startup: `src/OrcaSlicer.cpp`
- Slicing pipeline: `src/libslic3r/Print.cpp`
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- GUI: `src/slic3r/GUI/`
- Core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- Printer profiles: `resources/profiles/[manufacturer].json`

## Critical Constraints

- **Backward compatibility required** for .3mf project files and printer profiles
- **Cross-platform** — all changes must work on Windows, macOS, and Linux
- Profile/format changes need version migration handling
- Dependencies built separately in `deps/build/`, then linked to main app

## Code review focus areas

- Changes must not cause regressions in existing functionality, defaults, profiles, or project compatibility.
- Features gated by options must not affect existing behavior when those options are disabled.
- Changes should follow the existing code style and architecture. Architectural changes should be justified in code comments and the PR description.
- Add helper functions or utilities only when existing code cannot reasonably be reused. Avoid duplication.
- Keep code concise and clear. Manually simplify AI generated bloated codes before review.
- Include targeted tests or documented verification for behavior changes, especially in slicing logic, profiles, formats, and GUI defaults.
- For profile changes (`resources/profiles/<Vendor>/**`), check that `version` in the sibling `resources/profiles/<Vendor>.json` was bumped.
- For translation changes, follow the glossary and preservation rules in [localization/AGENTS.md](localization/AGENTS.md).

## Scoped guidance

- Start with [the feature map](docs/feature-map.md) to find implementation owners, design constraints and verification groups.
- GUI work: [src/slic3r/GUI/AGENTS.md](src/slic3r/GUI/AGENTS.md) and its linked wxWidgets skill.
- Tests: [tests/AGENTS.md](tests/AGENTS.md).
- Translation catalogs: [localization/AGENTS.md](localization/AGENTS.md). Also read it when changing translation extraction or disambiguation in source.
- System profiles: [.claude/skills/orca-profiles/SKILL.md](.claude/skills/orca-profiles/SKILL.md).
- Keep shared instructions here or in the relevant scoped file; `CLAUDE.md` files only import them.
