import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from prepare_porca_release import assemble


class PorcaReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "artifacts"
        self.source.mkdir()
        self.output = self.root / "upload"
        self.tag = "porca-preview-test"
        self.commit = "a" * 40
        self.run = "https://github.com/Strata-Print-Works/PorcaSlicer/actions/runs/123"
        self.put("Porca_Mac_universal_V2.5.0-dev", "app.dmg", b"macOS payload")
        for arch in ("x64", "arm64"):
            self.put(f"Porca_Windows_V2.5.0-dev_{arch}", f"Porca_Windows_Installer_V2.5.0-dev_{arch}.exe", b"installer " + arch.encode())
            self.put(f"Porca_Windows_V2.5.0-dev_{arch}_portable", "orca-slicer.exe", b"application " + arch.encode())
            self.put(f"Porca_Windows_V2.5.0-dev_{arch}_portable", "resources/LICENSE.txt", b"license")
        self.put("Porca_Linux_ubuntu_2404_V2.5.0-dev", "app.AppImage", b"x86_64 payload")
        self.put("Porca_Linux_ubuntu_2404_aarch64_V2.5.0-dev", "app.AppImage", b"aarch64 payload")

    def put(self, artifact, name, data):
        path = self.source / artifact / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def build(self):
        assemble(self.source, self.output, self.tag, self.commit, self.run)

    def test_complete_release_preserves_payloads_and_checksums(self):
        self.build()
        self.assertEqual(len(list(self.output.iterdir())), 9)
        self.assertEqual((self.output / f"Porca_macOS_universal_{self.tag}.dmg").read_bytes(), b"macOS payload")
        for arch in ("x64", "arm64"):
            with zipfile.ZipFile(self.output / f"Porca_Windows_{arch}_portable_{self.tag}.zip") as archive:
                self.assertEqual(archive.read("Porca/orca-slicer.exe"), b"application " + arch.encode())
                self.assertEqual(archive.read("Porca/resources/LICENSE.txt"), b"license")
        for line in (self.output / "SHA256SUMS").read_text().splitlines():
            checksum, name = line.split("  ")
            self.assertEqual(checksum, hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        self.assertEqual(json.loads((self.output / "build-manifest.json").read_text())["commit"], self.commit)

    def test_missing_platform_does_not_leave_partial_release(self):
        (self.source / "Porca_Mac_universal_V2.5.0-dev" / "app.dmg").unlink()
        with self.assertRaises(ValueError):
            self.build()
        self.assertFalse(self.output.exists())

    def test_duplicate_platform_is_rejected(self):
        self.put("Porca_Mac_universal_other", "other.dmg", b"other build")
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.build()

    def test_tag_cannot_write_outside_release_directory(self):
        for tag in ("../outside", "porca-../../outside", "porca-test/other", "v2.5.0"):
            with self.subTest(tag=tag):
                self.tag = tag
                with self.assertRaises(ValueError):
                    self.build()

    def test_existing_output_and_unrelated_build_urls_are_rejected(self):
        self.output.mkdir()
        with self.assertRaises(ValueError):
            self.build()
        self.output.rmdir()
        self.run = "https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/123"
        with self.assertRaises(ValueError):
            self.build()


if __name__ == "__main__":
    unittest.main()
