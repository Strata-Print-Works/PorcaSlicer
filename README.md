# Porca Slicer

Porca is a desktop slicer for preparing work across multiple printers in one shared workspace. Each plate is an editable job with its own printer setup, materials, print settings, and slice result. Beds stay at their physical scale; moving a plate keeps its models in place on that bed.

The project develops the existing C++/wxWidgets application and retains its mature slicing and printer tools. It is a development fork, and its builds and release notes must be distinguished from upstream releases.

![Porca icon](resources/images/Porca_256px.png)

## Work on this repository

Start with [agent instructions](AGENTS.md), the [feature map](docs/feature-map.md), and [local build and offline verification instructions](docs/development-builds.md). The [independent-plate design](docs/HLSD/independent-plate-setups.md) describes project compatibility and current workflow limits.

The current local app is built from this checkout with:

```sh
python3 scripts/build_porca.py OrcaSlicer libslic3r_tests slic3rutils_tests porca_unit_tests
```

The build target (`OrcaSlicer`) and the macOS app directory (`OrcaSlicer.app`) keep their inherited names; the app's displayed name, installer package name and icon are Porca. The user-facing release notes are in [resources/porca-release-notes.txt](resources/porca-release-notes.txt). This repository does not advertise upstream binaries as Porca downloads.

## Project files

Porca saves normal `.3mf` projects. Its optional metadata carries independent plate setups and workspace positions. Other slicers may open the geometry without retaining that Porca metadata. Keep a Porca copy when moving a project through another slicer.

## Provenance and license

Porca is based on [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer), which builds on [Bambu Studio](https://github.com/bambulab/BambuStudio), [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer), and [Slic3r](https://github.com/Slic3r/Slic3r). The inherited source and resources retain their copyright notices and license information. Porca is licensed under the [GNU Affero General Public License, version 3](LICENSE.txt).

The inherited pressure advance calibration pattern includes work adapted from Andrew Ellis' generator and Sineos' Marlin generator under GPLv3. The optional Bambu networking plugin uses non-free Bambu Lab libraries; its availability and terms are separate from Porca's source license.
