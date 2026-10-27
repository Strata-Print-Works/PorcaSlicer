# Snapmaker U1 print setup

Sending a sliced plate to a Moonraker host configured as `Snapmaker U1` opens a
native send dialog with one toolhead assignment for each filament actually used
by the slice. It also offers automatic flow calibration and a choice of the
assigned physical toolheads to calibrate. Upload alone stores the file; these
choices apply to Upload and Print. Editing the printer's loaded material records
remains in its vendor controls.

The dialog reads the U1's reported filament presence, material, nozzle diameter,
nozzle flow type and current calibration preferences. Its command list must
advertise `SDCARD_PRINT_FILE_WITH_PARAMETERS`; flow calibration is offered only
when `FLOW_CALIBRATE` is also reported. Missing nozzle or toolhead data is an
error. Each used filament needs a distinct loaded toolhead with a matching
material and nozzle diameter. A high-flow head can print a standard-flow job;
the reverse is rejected.

All used heads must have the same nozzle diameter: Snapmaker's current
parameterized-start implementation checks every used physical head against the
first supplied diameter. Mixed diameters are reported as unsupported rather than
omitting known nozzle metadata to bypass that firmware check.

`SnapmakerPrintTask` owns the selected plate's slice-time filament IDs,
temperatures, flow ratios, volumetric limits and usage, together with the chosen
assignments and calibration heads. The upload queue retains this snapshot;
changing the active plate or its presets cannot rewrite an already queued job.
It also retains the loaded colors shown when the assignments were confirmed;
a color change during the upload requires review instead of printing with an
unseen replacement. Initial assignments are selected automatically only when
the sliced and reported filament colors match.
Logical filament IDs are preserved even when the slice uses only sparse slots.
The task does not modify the project's filament map or another plate's setup.

The U1 receives plain G-code. Its upload explicitly requests `print=false`; only
a response confirming the stored path in `gcodes` with both `print_started` and
`print_queued` false permits the start step. The worker reads fresh toolhead
information and checks the saved assignments again after uploading. It then
issues one `SDCARD_PRINT_FILE_WITH_PARAMETERS` command through
`/printer/gcode/script`, including the map, selected calibration heads and sliced
filament parameters. Firmware validates and starts that file with those
parameters together. Generic Moonraker hosts keep their normal upload/start
behavior.

The protocol follows Snapmaker's public
[print-task configuration](https://github.com/Snapmaker/u1-klipper/blob/main/klippy/extras/print_task_config.py)
and [parameterized SD-card start](https://github.com/Snapmaker/u1-klipper/blob/main/klippy/extras/virtual_sdcard.py).
Lists retain Python-compatible quoting for Klipper's extended parameter parser.
Filenames containing command delimiters, quotes, control characters or parent
traversal are rejected. An unconfirmed start is never retried or followed by a
generic start request: the printer may already have accepted it.

The `[SnapmakerPrintTask]` cases run without wx, sockets or a printer in
`porca_unit_tests` (`python3 scripts/verify_offline.py u1`) and remain in
`slic3rutils_tests`. They cover sparse IDs,
snapshot ownership, changed or missing hardware/materials, selective calibration,
ambiguous upload replies and command injection. These checks do not establish
live firmware compatibility, calibration quality or physical print delivery.
