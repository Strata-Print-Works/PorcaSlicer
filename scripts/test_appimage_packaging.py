"""Exercise the generated packaging scripts with fake downloads and tools only."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "src/dev-utils/platform/unix"
MOCK_TOOL = """#!/usr/bin/env python3
import os, pathlib, sys
args = sys.argv[1:]
assert '--runtime-file' in args, 'packaging must not download its own runtime'
runtime = pathlib.Path(args[args.index('--runtime-file') + 1])
assert runtime.read_bytes() == b'runtime fixture'
assert pathlib.Path('usr/share/metainfo/com.orcaslicer.OrcaSlicer.metainfo.xml').read_text() == 'metadata fixture'
pathlib.Path('Porca-' + os.environ['FAKE_ARCH'] + '.AppImage').write_bytes(b'package fixture')
"""


class AppImagePackagingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="porca packaging ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        # Neither directory has the depth/layout the old relative lookup assumed.
        self.source = self.root / "source tree"
        self.build = self.root / "external build"
        self.build.mkdir()
        metadata = self.source / "scripts/flatpak/com.orcaslicer.OrcaSlicer.metainfo.xml"
        metadata.parent.mkdir(parents=True)
        metadata.write_text("metadata fixture")
        self.bin = self.root / "fake tools"
        self.bin.mkdir()
        self.write_tool("uname", '#!/bin/sh\nprintf "%s\\n" "$FAKE_ARCH"\n')
        self.write_tool("curl", "#!/bin/sh\necho 'Real downloads are prohibited in this test.' >&2\nexit 97\n")
        self.write_tool("wget", """#!/usr/bin/env python3
import os, pathlib, sys
args = sys.argv[1:]
assert '--tries=6' in args and '--timeout=60' in args
assert '--retry-on-http-error=429,500,502,503,504' in args
with open(os.environ['DOWNLOAD_LOG'], 'a') as log:
    log.write(args[-1] + '\\n')
if os.environ.get('FAIL_DOWNLOAD') or (os.environ.get('FAIL_FIRST_URL') and '?' not in args[-1]):
    pathlib.Path(args[args.index('-O') + 1]).write_bytes(b'partial transfer')
    sys.exit(18)
output = pathlib.Path(args[args.index('-O') + 1])
if 'appimagetool-' in args[-1]:
    output.write_text(""" + repr(MOCK_TOOL) + """ )
else:
    output.write_bytes(b'runtime fixture')
""")
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                    "FAKE_ARCH": "x86_64", "DOWNLOAD_LOG": str(self.root / "downloads")}
        self.env.pop("container", None)
        self.env.pop("FAIL_DOWNLOAD", None)
        self.script = self.build / "build_appimage.sh"
        self.script.write_text(self.render("build_appimage.sh.in"))

    def write_tool(self, name, content):
        tool = self.bin / name
        tool.write_text(content)
        tool.chmod(0o755)

    def render(self, name):
        text = (TEMPLATES / name).read_text()
        for key, value in {"CMAKE_SOURCE_DIR": str(self.source), "SLIC3R_APP_KEY": "OrcaSlicer",
                           "SLIC3R_APP_CMD": "orca-slicer", "SLIC3R_APP_DISPLAY_NAME": "Porca", "SoftFever_VERSION": "test"}.items():
            text = text.replace("@" + key + "@", value)
        return text

    def package(self, name, env):
        appdir = self.build / name
        image = appdir / "resources/images/OrcaSlicer_192px.png"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"icon fixture")
        (appdir / "orca-slicer").write_bytes(b"application fixture")
        result = subprocess.run(["bash", str(self.script)], cwd=appdir, env=env, capture_output=True, text=True)
        return result, appdir

    def test_each_architecture_reuses_downloads_and_finds_source_metadata(self):
        build_root = self.build
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                self.build = build_root / arch
                self.build.mkdir()
                self.script = self.build / "build_appimage.sh"
                self.script.write_text(self.render("build_appimage.sh.in"))
                env = {**self.env, "FAKE_ARCH": arch}
                for iteration in range(2):
                    result, appdir = self.package(arch + str(iteration), env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((appdir / "Porca_Linux_Vtest.AppImage").read_bytes(), b"package fixture")
                    desktop = (appdir / "com.orcaslicer.OrcaSlicer.desktop").read_text()
                    self.assertIn("Name=Porca\n", desktop)
                urls = (self.root / "downloads").read_text().splitlines()
                self.assertEqual(sum(url.endswith("runtime-" + arch) for url in urls), 1)
                self.assertEqual(sum(url.endswith("appimagetool-" + arch + ".AppImage") for url in urls), 1)

    def test_failed_transfer_cannot_publish_a_partial_tool_or_image(self):
        result, appdir = self.package("failed", {**self.env, "FAIL_DOWNLOAD": "1"})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.build / "appimagetool.AppImage").exists())
        self.assertFalse((appdir / "Porca_Linux_Vtest.AppImage").exists())

    def test_failed_release_redirect_gets_a_fresh_url(self):
        result, appdir = self.package("fresh redirect", {**self.env, "FAIL_FIRST_URL": "1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((appdir / "Porca_Linux_Vtest.AppImage").is_file())
        urls = (self.root / "downloads").read_text().splitlines()
        self.assertEqual(len(urls), 4)
        self.assertEqual(sum("?porca_download=" in url for url in urls), 2)

    def test_outer_packager_propagates_appimage_failure(self):
        (self.build / "package").mkdir()
        self.script.write_text("#!/bin/sh\nexit 19\n")
        block = self.render("build_linux_image.sh.in").rsplit('if [[ -n "$BUILD_IMAGE" ]]', 1)[1]
        result = subprocess.run(["bash", "-c", 'set -e\nBUILD_IMAGE=1\nif [[ -n "$BUILD_IMAGE" ]]' + block],
                                cwd=self.build, env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
