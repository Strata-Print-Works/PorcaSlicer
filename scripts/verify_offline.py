#!/usr/bin/env python3
"""Run explicitly listed offline Catch2 checks from an existing Release build.

No build, configure, dependency installation, app launch or arbitrary test filters.
The list is reviewed coverage, not an OS network sandbox. Reinspect changed tests
and their callees before running them; see docs/development-builds.md.
"""

import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

# `python3 -I` omits the script directory; resolve the sibling wrapper explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_porca import build_lock, release_configuration  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
# Each entry is (suite, tag, reviewed test sources). Keep filters narrow; do not
# add transport integration tests just because they use the same feature name.
GROUPS = {
    "accounts": [
        ("slic3rutils", "LocalAccounts", ["tests/slic3rutils/test_device_manager_integration.cpp"]),
        ("slic3rutils", "LocalPluginStorage", ["tests/slic3rutils/test_plugin_cloud_metadata.cpp"]),
    ],
    "u1": [("porca_unit", "SnapmakerPrintTask", ["tests/slic3rutils/test_snapmaker_print_task.cpp"])],
    "comparison": [
        ("porca_unit", "SliceComparison", ["tests/libslic3r/test_slice_comparison.cpp"]),
        ("libslic3r", "SliceComparisonSnapshot", ["tests/libslic3r/test_slice_comparison_snapshot.cpp"]),
        ("fff_print", "SliceComparisonPrint", ["tests/fff_print/test_slice_comparison.cpp"]),
    ],
    "library-formats": [("slic3rutils", "ModelLibraryFormats", ["tests/slic3rutils/test_model_library.cpp"])],
    "library": [
        ("porca_unit", "ModelLibrary", ["tests/slic3rutils/test_model_library.cpp"]),
    ],
    "devices": [
        ("slic3rutils", "ElegooLink", ["tests/slic3rutils/test_elegoo_link.cpp"]),
        ("slic3rutils", "BambuProcessSessions", ["tests/slic3rutils/test_bambu_process_sessions.cpp"]),
        ("porca_unit", "PluginAgentOwner", ["tests/slic3rutils/test_plugin_agent_owner.cpp"]),
        ("slic3rutils", "BambuDeviceSessions", ["tests/slic3rutils/test_bambu_device_sessions.cpp"]),
        ("porca_unit", "PrinterSessions", ["tests/slic3rutils/test_printer_sessions.cpp"]),
        ("slic3rutils", "NetworkDeviceSessions", ["tests/slic3rutils/test_network_device_sessions.cpp"]),
        ("slic3rutils", "DeviceOwnership", ["tests/slic3rutils/test_device_manager_integration.cpp"]),
        ("slic3rutils", "DeviceRequestFeedback", ["tests/slic3rutils/test_device_manager_integration.cpp"]),
        ("slic3rutils", "WebMediaController", ["tests/slic3rutils/test_web_media_controller.cpp"]),
        ("slic3rutils", "MoonrakerPrinterAgent", ["tests/slic3rutils/test_moonraker_printer_agent.cpp"]),
        ("slic3rutils", "Flashforge", ["tests/slic3rutils/test_printhost.cpp"]),
    ],
    "layout": [
        ("libslic3r", "PlateLayout", ["tests/libslic3r/test_plate_layout.cpp"]),
        ("libslic3r", "PlateMaterialTransfer", ["tests/libslic3r/test_plate_material_transfer.cpp",
                                                "tests/libslic3r/test_plate_print_setup.cpp"]),
        ("porca_unit", "SelectionAlignment", ["tests/libslic3r/test_selection_alignment.cpp"]),
    ],
    "quick": [
        ("porca_unit", "ProjectSliceQueue", ["tests/libslic3r/test_project_slice_queue.cpp"]),
        ("porca_unit", "ProjectBackupState", ["tests/libslic3r/test_project_backup_state.cpp"]),
        ("porca_unit", "PrinterConnectionStatus", ["tests/slic3rutils/test_printer_connection_status.cpp"]),
        ("porca_unit", "PreviewAllocation", ["tests/slic3rutils/test_preview_layout.cpp"]),
        ("porca_unit", "PreviewEstimate", ["tests/slic3rutils/test_preview_estimate.cpp"]),
        ("porca_unit", "FilamentUsage", ["tests/libslic3r/test_filament_usage.cpp"]),
        ("porca_unit", "PlateSliceBatch", ["tests/libslic3r/test_plate_slice_batch.cpp"]),
        ("porca_unit", "ProductionLedger", ["tests/libslic3r/test_production_ledger.cpp"]),
        ("porca_unit", "DiscoveryPolicy", ["tests/libslic3r/test_discovery_policy.cpp"]),
    ],
    "persistence": [
        ("libslic3r", "ProjectCompatibility", ["tests/libslic3r/test_3mf.cpp"]),
        ("libslic3r", "PlatePrintSetup", ["tests/libslic3r/test_plate_print_setup.cpp",
                                          "tests/libslic3r/test_model.cpp"]),
        ("libslic3r", "ProjectPlateImport", ["tests/libslic3r/test_project_plate_import.cpp",
                                             "tests/libslic3r/test_plate_print_setup.cpp",
                                             "tests/libslic3r/test_config.cpp"]),
    ],
    "production": [
        ("libslic3r", "ProductionLedger", ["tests/libslic3r/test_production_ledger.cpp",
                                           "tests/libslic3r/test_3mf.cpp"]),
    ],
    "workspace": [
        ("libslic3r", "WorkspaceStages", ["tests/libslic3r/test_workspace_stages.cpp",
                                          "tests/libslic3r/test_plate_print_setup.cpp"]),
        ("slic3rutils", "WorkspaceStyle", ["tests/slic3rutils/test_workspace_style.cpp"]),
        ("slic3rutils", "PlateTransfer", ["tests/slic3rutils/test_workspace_scene.cpp"]),
        ("slic3rutils", "WorkspaceScene", ["tests/slic3rutils/test_workspace_scene.cpp"]),
        ("slic3rutils", "UndoRedo", ["tests/slic3rutils/test_undo_redo.cpp"]),
        ("slic3rutils", "Camera", ["tests/slic3rutils/test_camera.cpp"]),
        ("slic3rutils", "Shortcuts", ["tests/slic3rutils/test_shortcuts.cpp"]),
    ],
    "preview": [
        ("slic3rutils", "PreviewLayout", ["tests/slic3rutils/test_camera.cpp",
                                         "tests/slic3rutils/test_preview_layout.cpp"]),
        ("slic3rutils", "ImGuiInput", ["tests/slic3rutils/test_imgui_input.cpp"]),
        ("slic3rutils", "PreviewEstimate", ["tests/slic3rutils/test_preview_estimate.cpp"]),
        ("libslic3r", "FilamentUsage", ["tests/libslic3r/test_filament_usage.cpp"]),
    ],
    "slicing": [
        ("libslic3r", "PlateSliceBatch", ["tests/libslic3r/test_plate_slice_batch.cpp"]),
    ],
    "discovery": [
        ("libslic3r", "ElegooDiscovery", ["tests/libslic3r/test_elegoo_discovery.cpp"]),
        ("libslic3r", "DiscoveryPolicy", ["tests/libslic3r/test_discovery_policy.cpp"]),
        ("libslic3r", "DiscoveredPrinter", ["tests/libslic3r/test_discovered_printer.cpp"]),
    ],
    "delivery": [
        ("libslic3r", "PrintDelivery", ["tests/libslic3r/test_print_delivery_validation.cpp"]),
        ("libslic3r", "DiscoveredPrinter", ["tests/libslic3r/test_discovered_printer.cpp"]),
    ],
}


def release_binary(build_dir, suite):
    multi_config, _ = release_configuration(build_dir)
    directory = build_dir / "tests" / suite
    if multi_config:
        directory /= "Release"
    binary = directory / (suite + "_tests" + (".exe" if os.name == "nt" else ""))
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"missing executable: {binary}; build {suite}_tests with scripts/build_porca.py first")
    return binary.resolve()


def read_result(path):
    root = ET.parse(path).getroot()
    cases = root.find("OverallResultsCases")
    assertions = root.find("OverallResults")
    if root.tag != "Catch2TestRun" or cases is None or assertions is None:
        raise ValueError("missing Catch2 completion totals")
    totals = {name: int(cases.attrib[name]) for name in ("successes", "failures", "expectedFailures", "skips")}
    count = sum(totals.values())
    if count == 0:
        raise ValueError("filter matched no test cases; rebuild or fix the reviewed group")
    if any(totals[name] for name in ("failures", "expectedFailures", "skips")):
        raise ValueError(f"incomplete or failing coverage: {totals}")
    return count, int(assertions.attrib["successes"])


def run_check(binary, tag, reports, seed, timeout, group):
    report = reports / f"{group}-{binary.stem}-{tag}.xml"
    log = reports / f"{group}-{binary.stem}-{tag}.log"
    command = [str(binary), f"[{tag}]", "--order", "rand", "--rng-seed", str(seed),
               "--reporter", "xml", "--out", str(report)]
    stat = binary.stat()
    result = {"tag": tag, "binary": str(binary), "binary_mtime_ns": stat.st_mtime_ns,
              "binary_size": stat.st_size, "command": command, "status": "failed"}
    started = time.monotonic()
    try:
        # Preset/3MF fixtures write relative to cwd. Never use the checkout or a
        # user's profile directory, including when invoked from outside the repo.
        with tempfile.TemporaryDirectory(prefix="porca-offline-") as working, log.open("w") as output:
            completed = subprocess.run(command, cwd=working, stdout=output, stderr=subprocess.STDOUT,
                                       timeout=timeout, check=False)
        result["exit_code"] = completed.returncode
        if completed.returncode != 0:
            raise ValueError(f"test process exited {completed.returncode}")
        result["cases"], result["assertions"] = read_result(report)
        result["status"] = "passed"
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
    result["seconds"] = round(time.monotonic() - started, 3)
    detail = f"{result['cases']} cases, {result['assertions']} assertions" if result["status"] == "passed" else result["error"]
    print(f"{result['status'].upper()} {binary.stem} [{tag}]: {detail} ({result['seconds']:.2f}s)", flush=True)
    if result["status"] != "passed":
        print(f"  Details: {report} and {log}", flush=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("groups", nargs="*", help="one or more named groups, or all")
    parser.add_argument("--list", action="store_true", help="show targets, tags and inspected sources; execute nothing")
    parser.add_argument("--dry-run", action="store_true", help="show the selection and build targets; execute nothing")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/prototype")
    parser.add_argument("--report-dir", type=Path, default=ROOT / "artifacts/verification",
                        help="parent for a unique report directory")
    parser.add_argument("--seed", type=int, help="reproduce Catch2's randomized order (unsigned 32-bit integer)")
    parser.add_argument("--timeout", type=int, default=300, help="seconds per tag (default: 300)")
    args = parser.parse_intermixed_args(argv)
    if args.timeout <= 0 or (args.seed is not None and not 0 <= args.seed <= 0xffffffff):
        parser.error("timeout must be positive and seed must be an unsigned 32-bit integer")
    if any(group not in GROUPS and group != "all" for group in args.groups):
        parser.error("unknown group; use --list to see reviewed groups")
    if not args.groups and not args.list:
        parser.error("choose a group or all; use --list to inspect coverage")
    groups = list(GROUPS) if args.list or "all" in args.groups else list(dict.fromkeys(args.groups))
    checks = [(group, suite, tag, sources) for group in groups for suite, tag, sources in GROUPS[group]]
    if args.list or args.dry_run:
        for group, suite, tag, sources in checks:
            print(f"{group}: {suite}_tests [{tag}]\n  " + ", ".join(sources))
        print("Build targets: " + " ".join(dict.fromkeys(suite + "_tests" for _, suite, _, _ in checks)))
        return 0

    seed = args.seed if args.seed is not None else secrets.randbits(32)
    build_dir = args.build_dir.resolve()
    summary = {"build_dir": str(build_dir), "seed": seed, "groups": groups,
               "status": "incomplete", "checks": []}
    reports = None
    previous_handler = signal.getsignal(signal.SIGTERM)

    def interrupted(_number, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        # Use the build wrapper's lock so a cooperating rebuild cannot replace a
        # binary during the run. No CMake or Ninja invocation is needed here.
        with build_lock(build_dir):
            binaries = {suite: release_binary(build_dir, suite) for _, suite, _, _ in checks}
            args.report_dir.mkdir(parents=True, exist_ok=True)
            reports = Path(tempfile.mkdtemp(prefix="offline-", dir=args.report_dir.resolve()))
            print(f"Testing existing Release binaries; rebuild affected targets first. Seed: {seed}", flush=True)
            print(f"Reports: {reports}", flush=True)
            for group, suite, tag, sources in checks:
                result = run_check(binaries[suite], tag, reports, seed, args.timeout, group)
                result.update(group=group, sources=sources)
                summary["checks"].append(result)
            failed = sum(check["status"] != "passed" for check in summary["checks"])
            summary["status"] = "failed" if failed else "passed"
            print(f"{len(checks) - failed}/{len(checks)} checks passed. Offline coverage only.", flush=True)
            return 1 if failed else 0
    except KeyboardInterrupt:
        print("Verification interrupted.", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Verification stopped: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous_handler)
        if reports is not None:
            (reports / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
