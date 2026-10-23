"""Runner regressions using fake processes/reports only; no real sockets."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build_porca import build_lock
import verify_offline as verify


def report(cases=2, failures=0, expected=0, skips=0):
    return (f'<Catch2TestRun><OverallResults successes="12" failures="{failures}" '
            f'expectedFailures="{expected}" skips="{skips}"/>'
            f'<OverallResultsCases successes="{cases}" failures="{failures}" '
            f'expectedFailures="{expected}" skips="{skips}"/></Catch2TestRun>')


def tags(group):
    return [tag for _, tag, _ in verify.GROUPS[group]]


class OfflineRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="porca-runner-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build with spaces"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        self.binary = self.build / "tests/libslic3r" / ("libslic3r_tests.exe" if os.name == "nt" else "libslic3r_tests")
        self.binary.parent.mkdir(parents=True)
        self.binary.write_text("not a real executable; all launches must be mocked\n")
        self.binary.chmod(0o755)
        self.reports = self.root / "reports"
        self.arguments = ["discovery", "--build-dir", str(self.build), "--report-dir", str(self.reports), "--seed", "17"]
        self.stdout = io.StringIO()
        for context in (redirect_stdout(self.stdout), redirect_stderr(self.stdout)):
            context.__enter__()
            self.addCleanup(context.__exit__, None, None, None)

    def fake_run(self, command, **kwargs):
        self.assertTrue(Path(command[0]).is_absolute())
        self.assertNotEqual(Path(kwargs["cwd"]), verify.ROOT)
        self.assertNotEqual(Path(kwargs["cwd"]), self.build)
        self.assertTrue(Path(kwargs["cwd"]).is_dir())
        self.assertEqual(command[command.index("--rng-seed") + 1], "17")
        # Prove the lock is held while executing, not just during preflight.
        if os.name != "nt":
            with self.assertRaises(RuntimeError):
                with build_lock(self.build):
                    self.fail("verification released the build lock too early")
        Path(command[command.index("--out") + 1]).write_text(report())
        return subprocess.CompletedProcess(command, 0)

    def summary(self):
        return json.loads(next(self.reports.glob("offline-*/summary.json")).read_text())

    def assert_reviewed_sources(self, suite, tag, sources, root=verify.ROOT):
        root = root.resolve()
        if suite == "porca_unit":
            # The small binary reuses only explicitly compiled sources, not the
            # full integration suites' directories (e.g. ledger 3MF fixtures).
            # Audit literal CMake paths without configuring or running CMake.
            cmake = root / "tests/porca_unit/CMakeLists.txt"
            text = re.sub(r"#[^\n]*", "", cmake.read_text(encoding="utf-8"))
            declarations = re.findall(r"(?:add_executable|target_sources)\s*\(([^()]*)\)", text)
            self.assertEqual(len(declarations), len(re.findall(r"(?:add_executable|target_sources)\s*\(", text)),
                             "Inspect unsupported porca_unit source declarations")
            paths = []
            for declaration in declarations:
                self.assertNotRegex(declaration, r"[$;]", "Inspect nonliteral porca_unit source declarations")
                paths.extend((cmake.parent / token).resolve() for token in shlex.split(declaration)
                             if token.endswith(".cpp"))
            self.assertTrue(paths, "Inspect porca_unit's compiled source declarations")
        else:
            # Keep the full suites' conservative directory-wide review boundary,
            # including tagged sources that have not been added to CMake yet.
            paths = [path for directory in {Path(source).parent for source in sources}
                     for path in (root / directory).rglob("*.cpp")]
        # Catch2 matches tags case-insensitively.
        matching = {path.relative_to(root).as_posix() for path in paths
                    if f"[{tag}]".lower() in path.read_text(encoding="utf-8").lower()}
        self.assertEqual(matching, set(sources), "Inspect changed tag coverage and update its sources")

    def test_reviewed_groups_run_in_isolation_and_record_reproducible_results(self):
        with patch.object(verify.subprocess, "run", side_effect=self.fake_run) as run:
            self.assertEqual(verify.main(self.arguments), 0)
        self.assertEqual([call.args[0][1] for call in run.call_args_list], [f"[{tag}]" for tag in tags("discovery")])
        for call in run.call_args_list:
            self.assertFalse(Path(call.kwargs["cwd"]).exists())
        summary = self.summary()
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["seed"], 17)
        self.assertEqual([check["cases"] for check in summary["checks"]], [2] * len(tags("discovery")))
        self.assertIn("binary_mtime_ns", summary["checks"][0])
        with build_lock(self.build):
            pass

    def test_inspection_modes_never_launch_processes_or_create_reports(self):
        with patch.object(verify.subprocess, "run") as run:
            self.assertEqual(verify.main(["--list"]), 0)
            self.assertEqual(verify.main(self.arguments + ["--dry-run"]), 0)
        run.assert_not_called()
        self.assertFalse(self.reports.exists())

    def test_tag_sources_do_not_silently_expand_beyond_the_reviewed_list(self):
        for checks in verify.GROUPS.values():
            for suite, tag, sources in checks:
                with self.subTest(suite=suite, tag=tag):
                    self.assert_reviewed_sources(suite, tag, sources)

    def test_unreviewed_tag_expansion_fails_in_quick_and_integration_sources(self):
        for directory in ("libslic3r", "slic3rutils", "porca_unit"):
            (self.root / "tests" / directory).mkdir(parents=True)
        ledger = "tests/libslic3r/test_production_ledger.cpp"
        integration = "tests/libslic3r/test_3mf.cpp"
        unreviewed = "tests/slic3rutils/test_unreviewed.cpp"
        for source in (ledger, integration, unreviewed):
            (self.root / source).write_text('TEST_CASE("example", "[ProductionLedger]") {}\n')
        cmake = self.root / "tests/porca_unit/CMakeLists.txt"
        cmake.write_text("add_executable(porca_unit_tests\n"
                         '    "../libslic3r/test_production_ledger.cpp"\n'
                         "    # ../libslic3r/test_3mf.cpp is integration-only\n)\n")
        self.assert_reviewed_sources("porca_unit", "ProductionLedger", [ledger], self.root)
        self.assert_reviewed_sources("libslic3r", "ProductionLedger", [ledger, integration], self.root)
        with self.assertRaisesRegex(AssertionError, "Inspect changed tag coverage"):
            self.assert_reviewed_sources("libslic3r", "ProductionLedger", [ledger], self.root)
        # A new compiled source outside the reviewed source's directory must
        # still fail for the small binary, even though 3MF fixtures are excluded.
        with cmake.open("a") as output:
            output.write("target_sources(porca_unit_tests PRIVATE ../slic3rutils/test_unreviewed.cpp)\n")
        with self.assertRaisesRegex(AssertionError, "Inspect changed tag coverage"):
            self.assert_reviewed_sources("porca_unit", "ProductionLedger", [ledger], self.root)

    def test_nonliteral_quick_source_lists_require_explicit_inspection(self):
        cmake = self.root / "tests/porca_unit/CMakeLists.txt"
        cmake.parent.mkdir(parents=True)
        cmake.write_text("add_executable(porca_unit_tests ${test_sources})\n")
        with self.assertRaisesRegex(AssertionError, "Inspect nonliteral porca_unit source declarations"):
            self.assert_reviewed_sources("porca_unit", "ProductionLedger", [], self.root)

    def test_shared_tags_keep_reports_from_both_executables(self):
        shared = next(tag for tag in tags("quick") if tag in tags("discovery"))
        small = self.build / "tests/porca_unit" / self.binary.name.replace("libslic3r", "porca_unit")
        small.parent.mkdir()
        small.write_bytes(self.binary.read_bytes())
        small.chmod(0o755)
        with patch.object(verify.subprocess, "run", side_effect=self.fake_run):
            self.assertEqual(verify.main(self.arguments + ["quick"]), 0)
        reports = next(self.reports.glob("offline-*"))
        self.assertTrue((reports / f"discovery-libslic3r_tests-{shared}.xml").is_file())
        self.assertTrue((reports / f"quick-porca_unit_tests-{shared}.xml").is_file())
        self.assertEqual(len(list(reports.glob("*.xml"))), len(verify.GROUPS["quick"]) + len(verify.GROUPS["discovery"]))

    def test_overlapping_groups_preserve_failed_report_and_log_evidence(self):
        shared = next(tag for tag in tags("discovery") if tag in tags("delivery"))
        calls = 0

        def first_fails(command, **kwargs):
            nonlocal calls
            result = self.fake_run(command, **kwargs)
            if command[1] == f"[{shared}]":
                calls += 1
                kwargs["stdout"].write(f"attempt {calls}\n")
                if calls == 1:
                    Path(command[command.index("--out") + 1]).write_text(report(failures=1))
                    return subprocess.CompletedProcess(command, 1)
            return result

        with patch.object(verify.subprocess, "run", side_effect=first_fails):
            self.assertEqual(verify.main(self.arguments + ["delivery"]), 1)
        checks = [check for check in self.summary()["checks"] if check["tag"] == shared]
        self.assertEqual([check["status"] for check in checks], ["failed", "passed"])
        paths = [Path(check["command"][-1]) for check in checks]
        self.assertNotEqual(*paths)
        self.assertIn('failures="1"', paths[0].read_text())
        self.assertIn('failures="0"', paths[1].read_text())
        self.assertEqual(paths[0].with_suffix(".log").read_text(), "attempt 1\n")
        self.assertEqual(paths[1].with_suffix(".log").read_text(), "attempt 2\n")

    def test_unknown_groups_and_invalid_limits_fail_before_execution(self):
        with patch.object(verify.subprocess, "run") as run:
            for args in [[], ["[Http]"], ["all", "typo"], ["discovery", "--timeout", "0"],
                         ["all", "--seed", "-1"], ["all", "--seed", str(2**32)]]:
                with self.subTest(args=args), self.assertRaises(SystemExit) as raised:
                    verify.main(args)
                self.assertEqual(raised.exception.code, 2)
        run.assert_not_called()

    def test_all_binaries_are_checked_before_any_test_runs(self):
        with patch.object(verify.subprocess, "run") as run:
            self.assertEqual(verify.main(self.arguments + ["preview"]), 1)
        run.assert_not_called()
        self.assertFalse(self.reports.exists())

    def test_debug_build_is_rejected_and_multiconfig_uses_only_release(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Debug\n")
        with self.assertRaises(ValueError):
            verify.release_binary(self.build, "libslic3r")
        cache.write_text("CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n")
        # Even with a root binary available, never fall back from missing Release.
        with self.assertRaises(ValueError):
            verify.release_binary(self.build, "libslic3r")
        release = self.binary.parent / "Release" / self.binary.name
        release.parent.mkdir()
        self.binary.rename(release)
        self.assertEqual(verify.release_binary(self.build, "libslic3r"), release.resolve())

    def test_undecodable_cache_bytes_do_not_stop_release_detection(self):
        (self.build / "CMakeCache.txt").write_bytes(b"//Path: \xff\xfe\nCMAKE_BUILD_TYPE:STRING=Release\n")
        self.assertEqual(verify.release_binary(self.build, "libslic3r"), self.binary.resolve())

    def test_empty_skipped_expected_failure_and_incomplete_reports_fail(self):
        path = self.root / "result.xml"
        for content in [report(cases=0), report(failures=1), report(expected=1), report(skips=1),
                        "<Catch2TestRun/>", "<broken", "<Catch2TestRun><OverallResultsCases/></Catch2TestRun>"]:
            with self.subTest(content=content):
                path.write_text(content)
                with self.assertRaises((ValueError, KeyError, verify.ET.ParseError)):
                    verify.read_result(path)

    def test_nonzero_exit_cannot_be_hidden_by_a_passing_report(self):
        def failed(command, **kwargs):
            self.fake_run(command, **kwargs)
            return subprocess.CompletedProcess(command, 9)

        with patch.object(verify.subprocess, "run", side_effect=failed):
            self.assertEqual(verify.main(self.arguments), 1)
        self.assertEqual(self.summary()["status"], "failed")
        self.assertEqual(len(self.summary()["checks"]), len(tags("discovery")))

    def test_one_missing_tag_fails_even_when_another_passes(self):
        absent = tags("discovery")[-1]

        def missing(command, **kwargs):
            result = self.fake_run(command, **kwargs)
            if command[1] == f"[{absent}]":
                Path(command[command.index("--out") + 1]).write_text(report(cases=0))
            return result

        with patch.object(verify.subprocess, "run", side_effect=missing):
            self.assertEqual(verify.main(self.arguments), 1)
        self.assertEqual([check["status"] for check in self.summary()["checks"]],
                         ["failed" if tag == absent else "passed" for tag in tags("discovery")])

    def test_timeout_is_reported_and_lock_is_released(self):
        with patch.object(verify.subprocess, "run", side_effect=subprocess.TimeoutExpired("fake", 1)):
            self.assertEqual(verify.main(self.arguments), 1)
        self.assertEqual(self.summary()["status"], "failed")
        with build_lock(self.build):
            pass

    def test_interruption_never_writes_a_passing_summary(self):
        with patch.object(verify.subprocess, "run", side_effect=KeyboardInterrupt):
            self.assertEqual(verify.main(self.arguments), 130)
        self.assertEqual(self.summary()["status"], "incomplete")
        with build_lock(self.build):
            pass

    @unittest.skipIf(os.name == "nt", "Windows locking needs native validation")
    def test_running_build_prevents_verification(self):
        with build_lock(self.build), patch.object(verify.subprocess, "run") as run:
            self.assertEqual(verify.main(self.arguments), 1)
        run.assert_not_called()
        self.assertFalse(self.reports.exists())


if __name__ == "__main__":
    unittest.main()
