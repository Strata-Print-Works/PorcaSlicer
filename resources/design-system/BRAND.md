Porca is a reskin of Orca Slicer: the same layout, controls and workflow, rebuilt on one calm, warm system so the app reads as modern and as Porca. Coral is the brand; it marks the one thing to do next and what is selected. Everything else stays quiet so the model on the plate is the loudest thing on screen.

## Principles

- **Keep the interface, change the finish.** Same panels, same order, same Orca names for settings and presets. Change the chrome: fewer boxes, one radius scale, one control height, one accent.
- **Coral means "act" or "selected", nothing else.** One `primary` button per area; `accent` for the active tab, switch, checkbox and modified-setting dot. Coral never marks an error.
- **Dense but even.** The sidebar holds hundreds of settings. Every row is `row-height` (32px), every control `control-md` (28px), every value column 148px wide.
- **The viewport is the hero.** Chrome sits on `bg` and `surface`; the plate sits on `viewport`. No gradients, glows or drop shadows on chrome.

## Voice and copy

- Sentence case everywhere: "Slice plate", "Add mixed filament", "First layer height". Never Title Case, never ALL CAPS in source text (the `overline` style uppercases with CSS).
- Keep Orca's names for settings, presets and tabs so existing users and wiki pages still match. Shorten only chrome copy: "Printer · Plate 1" instead of "Printer for Plate 1".
- Buttons start with a verb. Menu actions end in an ellipsis when they open a dialog: "Create printer…".
- Copilot and notices speak plainly, lead with the finding, then the fix: "Supports are off. The model has overhangs past 55°." Use "you" and "your", never "the user". No emoji.
- Units are lowercase and spaced from the number in prose ("0.2 mm"), and live in the field's unit slot in controls.

## Color

- Two themes from one set of names: **Dark** (default, the slicer's home) and **Light** (Cream). Put new colors in both.
- Grounds, back to front: `bg` (top bar, gutters) → `surface` (sidebar, Copilot, dialogs) → `surface-raised` (menus, cards, the selected segment) → `field` (inputs). `surface-hover` is the only hover fill. The 3D plate sits on `viewport`.
- Text is `ink`, secondary text and units are `ink-muted`; both pass 4.5:1 on every ground above. `ink-disabled` is for disabled controls only.
- `primary` (Coral `#FF7B7B`) fills primary buttons with an `on-primary` (Charcoal) label, 7:1. **Never white on Coral** (2.4:1).
- `accent` is Coral in Dark and a deeper `#D9545A` in Light so marks keep 3:1 on Cream. Use `accent-ink` for Coral text or icons, `accent-soft` for a selected row or active nav tab.
- Status: `success` (teal), `warning` (amber), `danger` (vermilion), `info` (blue), each with a `-soft` ground for notices and badges. Danger is pushed toward orange so it never reads as brand Coral; every status also carries an icon and a word.
- `line` is decoration (dividers). Control borders use `line-control`, which holds 3:1 against the grounds.
- `axis-x`, `axis-y`, `axis-z` are the standard red, green, blue gizmo colors; use them only for axes. The plate is `plate` with `plate-grid` lines (50mm major, 10mm minor at lower opacity); the plate name and number are `accent-ink` in the display face.
- Filament colors are user data, not tokens. Put them on a `ToolChip` or swatch and let `readableOn` pick the number color.

## Type

- **Figtree** (`sans`) for all interface text, with tabular numerals on. **Nunito** (`display`) at 800–900 for brand moments only: the home screen, dialog titles, slice summaries. **JetBrains Mono** (`mono`) for G-code, Copilot's step log, tool tags and coordinates.
- 13px `body` is the base. `heading` (14/650) titles panel sections, `label` (12/500) sets tabs and segments, `caption` (11/500) sets units and hints, `overline` (11/700, +0.08em, uppercase) heads setting groups and menu sections.
- Don't use Nunito for settings or body copy; its round forms get mushy at 13px.

## Space, size and shape

- Spacing is a 2/4px grid: `space-2` … `space-32`. Panel padding `space-12` horizontal, `space-16` between sections and groups, `space-8` between controls.
- Heights: `control-sm` 24, `control-md` 28 (sidebar default), `control-lg` 34 (top bar, dialogs), `row-height` 32. Sidebar `sidebar-width` 360px; top bar `topbar-height` 44px.
- Radii nest: `radius-md` (7px) on buttons and fields, `radius-sm` (5px) on anything inside them (segments, menu items, badges), `radius-lg` (10px) on menus and cards, `radius-xl` (16px) on dialogs and the app icon, `radius-pill` for switches and dots.
- Elevation only for things that float: `shadow-popover` (menus, tooltips) and `shadow-dialog`. `shadow-raised` is a 1px lift for the selected segment and secondary buttons. Panels are separated by hairlines, not shadows.

## States and motion

- Hover: `surface-hover` fill, or the border steps from `line-control` to `ink-muted`. Pressed: the darker fill (`primary-pressed`).
- Focus: a 2px solid `focus` ring, 2px offset (fields draw it as border plus 1px shadow). It holds 3:1 on every ground. Never remove it.
- Selected: `accent-soft` ground with `accent-ink` text (menus, nav), an `accent` bar (tabs), or a raised segment (segmented controls).
- Modified from preset: `accent` dot before the label and `accent` border on the field, with a reset arrow. This replaces Orca's orange lock/unlock icons.
- Disabled: `ink-disabled` text, fill flattens to `surface-hover`, no border.
- Motion is fast and functional: 120ms ease for color, 140ms for switches and chevrons. Nothing bounces, nothing loops.

## What changes from Orca

- Bordered boxes inside bordered boxes become flat panel sections split by `line` hairlines, with one bordered control per value.
- The printer block becomes a 48px `Select size="lg"` with the printer thumbnail and nozzle/plate subtitle; the bed type gets its own select beside it.
- Filament slots keep the 2×2 grid; the colored number moves inside the select (`FilamentSlot`), so each slot is one clean row.
- The Process header's coral pill toggle becomes a `SegmentedControl` (This plate / Objects) and an Advanced `Switch`; T1–T4 SF tabs become a segmented control with filament-color dots.
- Coral leaves secondary places (wifi icon at rest, every tab, both top-bar buttons) and stays on the active state and the single primary action.
- The top bar becomes `Tabs variant="nav"`, the project name with an unsaved dot, and two `SplitButton`s. Before a slice exists, Slice plate is `primary` and Export is `secondary`; once sliced they swap, so Coral always sits on the next step.
- The viewport tool strips become floating `Toolbar`s: object tools centered on top, plate tools on the right, view controls with the view cube at bottom-left, and a small info strip (file, size, tool) at bottom-center.
- Copilot findings render as `Notice`s inside its reply; the step log is mono with success checks; the prompt is a `Composer` with Check my work beside Send.

## Iconography

- Lucide line icons, 1.75 stroke, 16px default (14 in small controls, 20 in the viewport toolbar and nav), color from `currentColor`. The bundle carries the slicer set; see the Icon card for names.
- Orca has glyphs Lucide lacks (spool, nozzle, heated bed, support painting, seam painting). `disc-3` and `flame` stand in for now; draw replacements on Lucide's 24px grid and stroke, never mix in filled or two-tone icons.
- No emoji in the interface.

## Logo

- The supplied pig is preserved at `resources/branding/porca-pig-source.svg`. Use `resources/images/porca_mark.svg` for the standalone mark and `porca_icon.svg` for the app icon. Do not redraw, stretch or outline the pig.
- Use `porca_logo_dark.svg` on charcoal and `porca_logo_light.svg` on cream or white. The logo uses only the name Porca, set in outlined Nunito 900. All runtime logos are vectors with no font dependency.
- Preserve the supplied mark's coral `#FE7674`. The established UI action coral remains `#FF7B7B`; semantic status and filament colors keep their own meanings.
- The layer stripes belong to the mark. Do not repeat them as a decorative background pattern.
- `porca_splash_dark.svg` and `porca_splash_light.svg` provide the 640 × 320 startup composition. Keep the footer below y=240 free for native version, loading status and progress.
- Regenerate SVG compositions with `python3 scripts/generate_brand_assets.py`. The current overview is `resources/branding/porca-brand-kit.svg`; `brand-board.svg` is historical source material.
