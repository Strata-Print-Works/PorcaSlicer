#!/usr/bin/env python3
"""Build Release without overlapping writers to the same local Ninja cache.

Use this wrapper for local builds; direct cmake/ninja invocations bypass its lock.
--check-deps inspects the cache without invoking Ninja (even Ninja dry-runs can
truncate an invalid cache). --repair-deps backs up and removes only identical
duplicate path records; other damage requires separate investigation.
"""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time


def deps_log_version(data):
    if len(data) < 16 or not data.startswith(b"# ninjadeps\n"):
        raise ValueError("unsupported Ninja dependency log header")
    return struct.unpack_from("<I", data, 12)[0]


def inspect_deps(data):
    """Return redundant byte ranges only after validating the entire v4 log."""
    if deps_log_version(data) != 4:
        raise ValueError("unsupported Ninja dependency log header")
    paths = []
    duplicates = []
    offset = 16
    while offset < len(data):
        if offset + 4 > len(data):
            raise ValueError(f"incomplete record header at byte {offset}")
        record, = struct.unpack_from("<I", data, offset)
        size = record & 0x7fffffff
        start, end = offset + 4, offset + 4 + size
        if size < 4 or size > 512 * 1024 or size % 4 or end > len(data):
            raise ValueError(f"invalid or incomplete record at byte {offset}")
        if record & 0x80000000:
            if size < 12:
                raise ValueError(f"short dependency record at byte {offset}")
            values = struct.unpack_from(f"<{size // 4}I", data, start)
            if any(node >= len(paths) for node in (values[0], *values[3:])):
                raise ValueError(f"unknown dependency path at byte {offset}")
        else:
            checksum, = struct.unpack_from("<I", data, end - 4)
            node = ~checksum & 0xffffffff
            raw = data[start:end - 4]
            path = raw.rstrip(b"\0")
            if not path or b"\0" in path or len(raw) - len(path) > 3:
                raise ValueError(f"invalid path record at byte {offset}")
            if node == len(paths):
                paths.append(path)
            elif node < len(paths) and paths[node] == path:
                duplicates.append((offset, end))
            else:
                raise ValueError(f"inconsistent path ID at byte {offset}")
        offset = end
    return duplicates


@contextmanager
def build_lock(build_dir):
    """OS-owned lock: released on process exit, including an interrupted build."""
    lock_path = build_dir / ".porca-build.lock"
    with lock_path.open("a+b") as handle:
        if os.name == "nt":
            import msvcrt
            handle.seek(0)
            if not handle.read(1):
                handle.write(b"\0")
                handle.flush()
            handle.seek(0)
            try:
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                raise RuntimeError(f"another Porca build owns {lock_path}") from None
        else:
            import fcntl
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError(f"another Porca build owns {lock_path}") from None
        try:
            yield
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def check_or_repair(build_dir, repair=False):
    path = build_dir / ".ninja_deps"
    if not path.exists():
        print("No Ninja dependency cache present.")
        return
    initial = path.stat()
    data = path.read_bytes()
    version = deps_log_version(data)
    if version != 4:
        # Only the v4 record layout is understood; leave other versions to Ninja.
        print(f"Dependency cache validation skipped (unsupported deps log version {version}).")
        return
    duplicates = inspect_deps(data)
    if not duplicates:
        print("Ninja dependency cache is valid.")
        return
    if not repair:
        raise ValueError(f"{len(duplicates)} duplicate path record(s); run --repair-deps before Ninja")
    backup_dir = build_dir / ".porca-build-backups"
    backup_dir.mkdir(exist_ok=True)
    backup = backup_dir / f"ninja-deps-{time.time_ns()}.bin"
    shutil.copy2(path, backup)
    chunks, previous = [], 0
    for start, end in duplicates:
        chunks.append(data[previous:start])
        previous = end
    chunks.append(data[previous:])
    repaired = b"".join(chunks)
    if inspect_deps(repaired):
        raise ValueError("repaired cache still has duplicate records")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=build_dir, prefix=".ninja_deps-repair-", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(repaired)
            handle.flush()
            os.fsync(handle.fileno())
        current = path.stat()
        if (initial.st_mtime_ns, initial.st_size) != (current.st_mtime_ns, current.st_size):
            raise RuntimeError("cache changed during repair; stop other direct Ninja builds")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"Removed {len(duplicates)} duplicate path record(s); backup: {backup}")


def release_configuration(build_dir):
    """Return (multi-config, configured types) for a tree that can build Release.

    `cmake --build --config Release` selects a configuration only for
    multi-config generators; a single-config tree builds its CMAKE_BUILD_TYPE.
    """
    cache = {}
    text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if "=" in line and ":" in line.split("=", 1)[0] and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    configs = [name for name in cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";") if name]
    if configs:
        if "Release" not in configs:
            raise ValueError(f"configured tree has no Release configuration (found {';'.join(configs)})")
        return True, ";".join(configs)
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    if build_type != "Release":
        raise ValueError(f"configured tree must use CMAKE_BUILD_TYPE=Release (found {build_type or 'none'}); "
                         "reconfigure it as Release")
    return False, build_type


def run_build(command):
    child = None
    signals = [signal.SIGTERM]
    if hasattr(signal, "SIGHUP"):
        signals.append(signal.SIGHUP)
    original = {value: signal.getsignal(value) for value in [signal.SIGINT, *signals]}

    def interrupted(_number, _frame):
        raise KeyboardInterrupt

    for value in signals:
        signal.signal(value, interrupted)
    try:
        child = subprocess.Popen(command, start_new_session=os.name != "nt")
        code = child.wait()
        # A POSIX child stopped by a signal reports -N; exit with the shell's 128 + N.
        return 128 - code if code < 0 else code
    except KeyboardInterrupt:
        # Retain the cache lock until the owned build stops, including when the
        # invoking terminal/task terminates the wrapper instead of cmake.
        for value in [signal.SIGINT, *signals]:
            signal.signal(value, signal.SIG_IGN)
        if child is not None:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], check=False)
                child.wait()
            else:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
        return 130
    finally:
        for value, handler in original.items():
            signal.signal(value, handler)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("targets", nargs="*", default=["OrcaSlicer"])
    parser.add_argument("--build-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build/prototype")
    parser.add_argument("--jobs", type=int, default=6)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check-deps", action="store_true")
    action.add_argument("--repair-deps", action="store_true")
    action.add_argument("--dry-run", action="store_true", help="Ninja only; explain pending work without compiling")
    args = parser.parse_args(argv)
    build_dir = args.build_dir.resolve()
    if not (build_dir / "CMakeCache.txt").exists():
        parser.error(f"configure the build directory first: {build_dir}")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    started = time.monotonic()
    try:
        if not (args.check_deps or args.repair_deps):
            multi_config, build_types = release_configuration(build_dir)
            print(f"Configured build type: {build_types}" + (" (building Release)" if multi_config else ""),
                  flush=True)
        with build_lock(build_dir):
            check_or_repair(build_dir, args.repair_deps)
            if args.check_deps or args.repair_deps:
                return 0
            if args.dry_run and not (build_dir / "build.ninja").exists():
                parser.error("--dry-run requires a Ninja build directory")
            command = ["cmake", "--build", str(build_dir), "--config", "Release", "--target", *args.targets, f"-j{args.jobs}"]
            if args.dry_run:
                command.extend(["--", "-n", "-d", "explain"])
            return run_build(command)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Build stopped: {error}", file=sys.stderr)
        return 1
    finally:
        print(f"Elapsed: {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    sys.exit(main())
