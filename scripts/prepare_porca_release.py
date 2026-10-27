#!/usr/bin/env python3
"""Assemble Porca preview downloads from one successful Build all run."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import zipfile


EXPECTED = {"macOS_universal", "Windows_x64_installer", "Windows_x64_portable",
            "Windows_arm64_installer", "Windows_arm64_portable",
            "Linux_x86_64", "Linux_aarch64"}


def select_artifacts(source):
    selected = {}
    for folder in sorted(source.iterdir()):
        if folder.is_symlink():
            raise ValueError(f"Unexpected symbolic link: {folder.name}")
        if not folder.is_dir():
            continue
        name = folder.name
        if name.startswith("Porca_Mac_universal_"):
            key, pattern = "macOS_universal", "*.dmg"
        elif name.startswith("Porca_Windows_V"):
            match = re.fullmatch(r"Porca_Windows_V.+_(x64|arm64)(_portable)?", name)
            if not match:
                raise ValueError(f"Unrecognized Windows artifact: {name}")
            arch, portable = match.groups()
            key = f"Windows_{arch}_{'portable' if portable else 'installer'}"
            pattern = None if portable else f"Porca_Windows_Installer_*_{arch}.exe"
        elif name.startswith("Porca_Linux_ubuntu_"):
            key = "Linux_aarch64" if "_aarch64_" in name else "Linux_x86_64"
            pattern = "*.AppImage"
        else:
            continue
        if key in selected:
            raise ValueError(f"Duplicate release artifact for {key}")
        if any(path.is_symlink() for path in folder.rglob("*")):
            raise ValueError(f"Unexpected symbolic link in {name}")
        if pattern:
            files = list(folder.glob(pattern))
            if len(files) != 1 or not files[0].is_file():
                raise ValueError(f"Expected one {pattern} in {name}")
            selected[key] = files[0]
        else:
            if not (folder / "orca-slicer.exe").is_file():
                raise ValueError(f"Missing portable application in {name}")
            selected[key] = folder
    missing = EXPECTED - selected.keys()
    if missing:
        raise ValueError("Missing release artifacts: " + ", ".join(sorted(missing)))
    return selected


def assemble(source, destination, tag, commit, run_url):
    if not re.fullmatch(r"porca-[A-Za-z0-9][A-Za-z0-9._-]{0,63}", tag):
        raise ValueError("Use a Porca tag such as porca-preview-2026.10.26")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A complete source commit is required")
    if not re.fullmatch(r"https://github\.com/Strata-Print-Works/PorcaSlicer/actions/runs/[0-9]+", run_url):
        raise ValueError("A build run from the Porca repository is required")
    if destination.exists():
        raise ValueError("The output directory must not already exist")
    selected = select_artifacts(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="porca-release-", dir=destination.parent) as staging:
        staging = Path(staging)
        for key, path in sorted(selected.items()):
            suffix = ".zip" if path.is_dir() else path.suffix
            target = staging / f"Porca_{key}_{tag}{suffix}"
            if path.is_dir():
                with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
                    for item in sorted(path.rglob("*")):
                        if item.is_file():
                            archive.write(item, Path("Porca") / item.relative_to(path))
            else:
                shutil.copyfile(path, target)
                if suffix == ".AppImage":
                    target.chmod(0o755)
        manifest = {"product": "Porca", "tag": tag, "commit": commit, "build_run": run_url,
                    "distribution": "Developer preview; no Developer ID signing or notarization; Windows unsigned",
                    "platforms": sorted(selected)}
        (staging / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        checksums = []
        for path in sorted(staging.iterdir()):
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            checksums.append(f"{digest}  {path.name}\n")
        (staging / "SHA256SUMS").write_text("".join(checksums), encoding="utf-8")
        staging.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--run-url", required=True)
    args = parser.parse_args()
    try:
        assemble(args.artifacts, args.output, args.tag, args.commit, args.run_url)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
