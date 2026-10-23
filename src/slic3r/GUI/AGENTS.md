# Native GUI work

Read the root [AGENTS.md](../../../AGENTS.md) for product invariants, UX copy and the current local validation restriction. Use the [feature map](../../../docs/feature-map.md) to locate the relevant code and tests.

- Apply the [orca-wxwidgets skill](../../../.claude/skills/orca-wxwidgets/SKILL.md) and read the references relevant to the change. Use the locally built wx source for API contracts; the root network restriction also applies to commands shown in skills.
- Use `SetSizerAndFit(sizer)` on top-level windows. If the sizer must be attached before the layout is complete, call `sizer->SetSizeHints(window)` afterwards.
- Register new GUI sources in `src/slic3r/CMakeLists.txt` (or the existing subsystem CMake file), and files with extracted user text in `localization/i18n/list.txt`.
- Reuse Orca widgets and Porca's design tokens. For supported Prepare toolbar styling, use the live override workflow in [the build guide](../../../docs/development-builds.md).
- Keep logic that can be verified without wx, GL or a printer independent of UI callbacks where practical. Preserve existing architecture; avoid unrelated extraction or file moves.
- Offline layout/math tests do not establish appearance, focus, event ordering or behavior on another OS. After a native UI change, follow the root save/build/restart workflow for visual review.
