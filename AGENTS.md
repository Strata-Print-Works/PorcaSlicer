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

- Keep Porca and other suspect apps closed. Do not launch an older build or another fork as a workaround.
- Do not run tests or scripts that send network packets on this Mac. Use static analysis, builds and pure/fake tests that never open real sockets.
- Authorization to edit code, commit, push or move files does not lift this restriction.
- Offline checks do not establish working live discovery, a resolved driver crash or physical printer qualification. Report those limits clearly. macOS build evidence does not verify the Windows or Linux build.

Preserve the discovery hardening: eligible physical LAN interfaces, bounded probe rates and polling, cancellation, owned sockets and workers, and complete cleanup. Prefer system discovery services where available while retaining supported platform fallbacks. Slicing or connection success alone is never permission to send motion, heating, upload or print-start commands.

## Workspace and Git

- This Mac checkout is `/Users/peter/code/experiments/porca-slicer`. Work in this repository; the former MultiSlicer prototype paths have moved. Leave the sibling MultiSlicer and original Orca checkouts alone unless the user requests work there.
- Preserve existing Git changes before editing or restructuring. Keep Porca's Git object storage independent of the original checkout.
- `origin` currently points to the local OrcaSlicer repository; `upstream` is the original public Orca remote. Inspect remotes before pushing. A local push is not a GitHub publication, and pushing to upstream requires explicit user direction.
- `artifacts/` holds app bundles, saved projects, device data, backups and verification records. `mockups/` holds local UI studies, and `base/` holds generated test presets. These directories and build outputs are ignored. Keep credentials, personal printer data and optional vendor binaries out of source commits.

## Build Commands

Build the Release configuration unless asked otherwise.

```bash
# macOS: current Porca Release build
cmake --build build/prototype --config Release --target OrcaSlicer libslic3r_tests -j6

# Linux
cmake --build build --config Release --target all --

# Windows
cmake --build . --config Release --target ALL_BUILD -- -m
```

## Testing

Catch2 framework. Tests in `tests/`; see [tests/AGENTS.md](tests/AGENTS.md) for where a new test belongs and the conventions to follow.

While the current local validation restriction applies, run only inspected offline tests that never open real sockets. The verified filter for plate persistence, discovery policy and protocol parsing is:

```bash
build/prototype/tests/libslic3r/libslic3r_tests \
  '[PlatePrintSetup],[DiscoveryPolicy],[DiscoveredPrinter]' --order rand
```

Run tests that generate local presets from an isolated temporary working directory, using the absolute path to the executable. Rebuild after moving a checkout: some test fixtures have their source path compiled into the executable. Add targeted format and actual slicing regressions when the change requires them, after confirming they remain offline. Do not run a blanket suite without checking it against the current restriction.

The usual suite commands, subject to that restriction, are:

```bash
cd build && ctest -C Release --output-on-failure    # all tests
ctest --test-dir ./tests/libslic3r -C Release       # individual suite
ctest --test-dir ./tests/fff_print -C Release
```

## Documentation

- Docs live in `docs/`; the high-level design of a subsystem goes in `docs/HLSD/<subsystem>.md`.
- Describe the design as it stands — what the subsystem does, why it exists, and the constraints that shape it. Not the route that got there: no phases, task lists, status markers, or "before/after this PR" framing.
- Planning and investigation output (brainstorms, superpowers design and plan docs) stays in `docs/superpowers/`, which is gitignored. Never commit it.
- Write a doc only when the design is not evident from the code, and when a change invalidates an existing one, update it in the same PR.

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Include what you use: include the header for every symbol a file uses, and keep headers compilable on their own. Never rely on the precompiled header or a transitive include. The `clang-tidy` CI job enforces this on changed lines; run the same check locally with `scripts/run_clang_tidy.sh` (`scripts\run_clang_tidy.ps1` on Windows), which sets up everything it needs
- Parallelization via TBB — be mindful of shared state
- Always use `SetSizerAndFit(sizer)` instead of `SetSizer(sizer)` on top level window. Unless `SetSizer` must be called before the full layout is built, call `sizer->SetSizeHints(window)` afterwards in this case.

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
- For translation changes (`localization/i18n/**/*.po`), check that recurring terms match the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md) for that language.

## Localization & translations

Catalogs live in `localization/i18n/<lang>/OrcaSlicer_<lang>.po`; the template is `OrcaSlicer.pot`.
See the [Localization guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_guide.md) for the human-facing version of these principles.

### Terminology

- Use the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md) as the source of truth for recurring terms, so the same English term is always rendered the same way within a language, and terms that must stay in English (brand/product names, acronyms, materials, file formats, G-code tokens, macros/variables/identifiers) are not translated.
- If a term's established translation changes, update both the affected `.po` files and the glossary (`localization_glossary.tsv`, then regenerate) so they stay in sync.
- Translate the *meaning*, not the words. Check what the string actually controls before translating it — English reuses one word for different things. `Flow ratio` (multiplier), `Flow Rate` (throughput) and `Flow Dynamics` (pressure compensation) are three different terms; `extruder` may mean the toolhead, the feeder motor, or the nozzle depending on the string.
- Reuse one template per recurring message shape (`Failed to connect to …`, `Are you sure you want to …?`), even where the English wording varies.

### Editing rules

- Only edit `msgstr` — **never** change `msgid`, and never "fix" wrong English in the translation alone. Report the source string instead.
- Preserve exactly: placeholders (`%s`, `%d`, `%1%`, `%zu`, `%%`), every `\n` (count *and* position, including leading/trailing), leading/trailing spaces, HTML tags, `℃`, and the file's encoding and line endings.
- **Never reorder positional arguments** in a `c-format` string. If the msgid is `%d` then `%s`, that order must hold — swapping them breaks at runtime.
- `msgctxt` separates homonyms — always read it. `Back`/`Camera View` is the rear view of the 3D navigator, while `Back`/`Navigation` is the go-back button; `Top` exists in the *Alignment*, *Layers* and *Camera View* senses.
- When a string needs disambiguating, add context in the source (`_L_CONTEXT`/`_u8L_CONTEXT`), don't work around it in the translation.
- A literal `%` inside a string xgettext flagged `possible-c-format` will fail `msgfmt`. Fix it with a `// xgettext:no-c-format, no-boost-format` comment above the string in the source — do not mangle the translation or use `%%` in text that is never passed through printf.
- Plural entries: read `nplurals` from the catalog's `Plural-Forms` header (it is **not** always 2 — ja/ko/zh/th/vi use 1, ru/cs/pl/lt use 3, uk uses 4). Each form must be genuinely inflected for its quantity; repeating one sentence across all forms is a bug in Slavic/Baltic languages, though it is correct for Turkish and Hungarian.
- An entry whose `msgstr` equals its `msgid` is untranslated even though it is not empty; a plural entry with any empty form is likewise incomplete.
- Mark machine-produced translations with an `# AI Translated` translator comment. Don't add it to a human translation you didn't actually rewrite.
- When you can't be sure of a machine translation's meaning or UI wording, also add `# Needs human review: <what to check>`. Never use `fuzzy` for this — fuzzy entries are hidden from users.
- Don't reflow or re-wrap unrelated entries — keep the diff limited to the strings you changed.

### Verifying

- `scripts/run_gettext.bat --full` (Windows) regenerates the template, merges every catalog and compiles the `.mo` files. It must exit 0.
- Or check a single catalog with `msgfmt --check-format -o <out>.mo localization/i18n/<lang>/OrcaSlicer_<lang>.po`.
- Fuzzy entries are not shown to users. If you correct one, clear its `fuzzy` flag, otherwise the fix never ships.
