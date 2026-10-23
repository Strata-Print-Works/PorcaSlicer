"""Offline regressions for dependency-cache recovery and build serialization."""

from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import build_porca
from build_porca import build_lock, check_or_repair, inspect_deps


HEADER = b"# ninjadeps\n\x04\x00\x00\x00"


def path_record(node, path):
    raw = path.encode()
    raw += b"\0" * (-len(raw) % 4)
    return struct.pack("<I", len(raw) + 4) + raw + struct.pack("<I", ~node & 0xffffffff)


def deps_record(output, *dependencies):
    values = [output, 123, 456, *dependencies]
    return struct.pack("<I", 0x80000000 | len(values) * 4) + struct.pack(f"<{len(values)}I", *values)


class DependencyCacheTests(unittest.TestCase):
    def setUp(self):
        self.prefix = HEADER + path_record(0, "unit.o") + path_record(1, "unit.cpp")

    def test_valid_cache_including_later_dependency_updates(self):
        self.assertEqual(inspect_deps(self.prefix + deps_record(0, 1) * 2), [])

    def test_duplicate_recovery_preserves_latest_records_and_original(self):
        duplicate = path_record(0, "unit.o")
        original = self.prefix + duplicate + deps_record(0, 1)
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            path = build_dir / ".ninja_deps"
            path.write_bytes(original)
            with self.assertRaises(ValueError):
                check_or_repair(build_dir)
            self.assertEqual(path.read_bytes(), original)
            output = io.StringIO()
            with redirect_stdout(output):
                check_or_repair(build_dir, repair=True)
            self.assertIn("Removed 1 duplicate path record(s)", output.getvalue())
            self.assertEqual(path.read_bytes(), self.prefix + deps_record(0, 1))
            backups = list((build_dir / ".porca-build-backups").iterdir())
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_bytes(), original)

    def test_ambiguous_damage_is_rejected_without_mutation(self):
        cases = [
            self.prefix + path_record(0, "different.o"),
            self.prefix + path_record(9, "unit.o"),
            self.prefix + deps_record(0, 2),
            self.prefix + deps_record(0, 1)[:-1],
            self.prefix + b"\x04",
            b"# notninja\n\x04\x00\x00\x00",
            HEADER[:14],
        ]
        for original in cases:
            with self.subTest(original=original), tempfile.TemporaryDirectory() as directory:
                build_dir = Path(directory)
                path = build_dir / ".ninja_deps"
                path.write_bytes(original)
                with self.assertRaises(ValueError):
                    check_or_repair(build_dir, repair=True)
                self.assertEqual(path.read_bytes(), original)

    def test_other_log_versions_skip_validation_without_mutation(self):
        original = b"# ninjadeps\n\x05\x00\x00\x00" + b"\xff" * 7
        with self.assertRaises(ValueError):
            inspect_deps(original)
        for repair in (False, True):
            with self.subTest(repair=repair), tempfile.TemporaryDirectory() as directory:
                build_dir = Path(directory)
                path = build_dir / ".ninja_deps"
                path.write_bytes(original)
                output = io.StringIO()
                with redirect_stdout(output):
                    check_or_repair(build_dir, repair=repair)
                self.assertIn("unsupported deps log version 5", output.getvalue())
                self.assertEqual(path.read_bytes(), original)
                self.assertFalse((build_dir / ".porca-build-backups").exists())

    @unittest.skipIf(os.name == "nt", "Windows file-lock behavior requires native validation")
    def test_second_writer_rejected_and_lock_released(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            with build_lock(build_dir):
                with self.assertRaises(RuntimeError):
                    with build_lock(build_dir):
                        self.fail("overlapping writer acquired lock")
            with build_lock(build_dir):
                pass

    @unittest.skipIf(os.name == "nt", "Windows process-tree behavior requires native validation")
    def test_termination_stops_owned_child_before_unlocking(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            marker = build_dir / "child-pid"
            child_code = f"import os,time;from pathlib import Path;Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(60)"
            wrapper_code = (
                "from pathlib import Path;import sys;from build_porca import build_lock,run_build\n"
                f"with build_lock(Path({directory!r})):\n"
                f" result=run_build([sys.executable,'-c',{child_code!r}])\n"
                "sys.exit(result)"
            )
            wrapper = subprocess.Popen([sys.executable, "-c", wrapper_code], cwd=Path(__file__).parent)
            try:
                deadline = time.monotonic() + 3
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(marker.exists())
                child_pid = int(marker.read_text())
                wrapper.send_signal(signal.SIGTERM)
                self.assertEqual(wrapper.wait(timeout=3), 130)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
                with build_lock(build_dir):
                    pass
            finally:
                if wrapper.poll() is None:
                    wrapper.kill()
                    wrapper.wait()


class ReleaseConfigurationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build_dir = Path(temporary.name)

    def build(self, cache, *options):
        (self.build_dir / "CMakeCache.txt").write_text(cache)
        output = io.StringIO()
        # The build itself is faked; these cases never invoke CMake or Ninja.
        with patch.object(build_porca, "run_build", return_value=0) as run, \
                redirect_stdout(output), redirect_stderr(output):
            result = build_porca.main(["--build-dir", str(self.build_dir), *options, "libslic3r_tests"])
        return result, run, output.getvalue()

    def test_non_release_trees_stop_before_building(self):
        for cache, found in [("CMAKE_BUILD_TYPE:STRING=Debug\n", "CMAKE_BUILD_TYPE=Release (found Debug)"),
                             ("CMAKE_BUILD_TYPE:STRING=\n", "CMAKE_BUILD_TYPE=Release (found none)"),
                             ("CMAKE_CONFIGURATION_TYPES:STRING=Debug;RelWithDebInfo\n",
                              "no Release configuration (found Debug;RelWithDebInfo)")]:
            with self.subTest(cache=cache):
                result, run, output = self.build(cache)
                self.assertEqual(result, 1)
                run.assert_not_called()
                self.assertIn(found, output)
                self.assertNotIn("Configured build type", output)

    def test_release_trees_report_their_configuration_and_build_release(self):
        for cache, configured in [("CMAKE_BUILD_TYPE:STRING=Release\n", "Configured build type: Release\n"),
                                  ("CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n",
                                   "Configured build type: Debug;Release (building Release)\n")]:
            with self.subTest(cache=cache):
                result, run, output = self.build(cache)
                self.assertEqual(result, 0)
                self.assertIn(configured, output)
                command = run.call_args.args[0]
                self.assertEqual(command[command.index("--config") + 1], "Release")

    def test_dependency_inspection_does_not_require_release(self):
        result, run, output = self.build("CMAKE_BUILD_TYPE:STRING=Debug\n", "--check-deps")
        self.assertEqual(result, 0)
        run.assert_not_called()
        self.assertIn("No Ninja dependency cache present.", output)


if __name__ == "__main__":
    unittest.main()
