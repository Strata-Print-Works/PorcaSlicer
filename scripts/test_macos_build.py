"""Offline checks for SDK selection and the cached TLS-library link guard."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MacSDKTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="porca-sdk-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.script = self.root / "build_release_macos.sh"
        shutil.copyfile(ROOT / self.script.name, self.script)
        (self.root / "deps").mkdir()
        tools = self.root / "tools"
        tools.mkdir()
        self.log = self.root / "configure.json"
        self.sdk = self.root / "Selected SDK.sdk"
        self.sdk.mkdir()
        self.env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
                    "PROBE_LOG": str(self.log), "PROBE_SDK": str(self.sdk)}
        for key in ("SDKROOT", "BUILD_ONLY", "BUILD_TARGET", "BUILD_TESTS", "ARCH"):
            self.env.pop(key, None)
        scripts = {
            "xcrun": """#!/bin/sh
test "$*" = '--sdk macosx --show-sdk-path' || exit 98
test -z "$PROBE_SDK_FAILURE" || exit 19
test -n "$PROBE_EMPTY_SDK" || printf '%s\\n' "$PROBE_SDK"
""",
            "cmake": """#!/usr/bin/env python3
import json, os, pathlib, sys
if sys.argv[1:] == ['--version']:
    print('cmake version 4.3.0')
    sys.exit(0)
pathlib.Path(os.environ['PROBE_LOG']).write_text(json.dumps({
    'args': sys.argv[1:], 'sdk': os.environ.get('SDKROOT')}))
# Stop at configure: never build, bootstrap, run tests or download anything.
sys.exit(97)
""",
        }
        for name, contents in scripts.items():
            path = tools / name
            path.write_text(contents)
            path.chmod(0o755)

    def run_script(self, mode):
        return subprocess.run(["bash", str(self.script), mode, "-x", "-a", "x86_64"],
                              env=self.env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=15)

    def test_selected_sdk_reaches_dependency_and_app_configure(self):
        for mode in ("-d", "-s"):
            with self.subTest(mode=mode):
                result = self.run_script(mode)
                self.assertEqual(result.returncode, 97, result.stdout)
                probe = json.loads(self.log.read_text())
                self.assertEqual(probe["sdk"], str(self.sdk))
                self.assertIn("-DCMAKE_OSX_SYSROOT=" + str(self.sdk), probe["args"])

    def test_explicit_sdk_is_preserved_without_lookup(self):
        self.env.update(SDKROOT="custom-macos-sdk", PROBE_SDK_FAILURE="1")
        result = self.run_script("-d")
        self.assertEqual(result.returncode, 97, result.stdout)
        probe = json.loads(self.log.read_text())
        self.assertEqual(probe["sdk"], "custom-macos-sdk")
        self.assertIn("-DCMAKE_OSX_SYSROOT=custom-macos-sdk", probe["args"])

    def test_sdk_lookup_failure_stops_before_configure(self):
        self.env["PROBE_SDK_FAILURE"] = "1"
        self.assertEqual(self.run_script("-d").returncode, 19)
        self.assertFalse(self.log.exists())

    def test_empty_sdk_stops_before_configure(self):
        self.env["PROBE_EMPTY_SDK"] = "1"
        self.assertNotEqual(self.run_script("-s").returncode, 0)
        self.assertFalse(self.log.exists())


class BundledTLSLinkTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="porca-tls-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "libraries"
        self.source.mkdir()
        include = self.source / "include"
        (include / "curl").mkdir(parents=True)
        (include / "rtc").mkdir()
        (include / "curl/curl.h").write_text(
            'constexpr int CURLVERSION_NOW = 0;\nextern "C" int* curl_version_info(int);\n')
        (include / "rtc/peerconnection.hpp").write_text(
            'namespace rtc { class PeerConnection { public: PeerConnection(); ~PeerConnection(); }; }\n')
        files = {
            "curl.cpp": 'extern "C" void SSL_get1_peer_certificate();\n'
                        'extern "C" int* curl_version_info(int) { SSL_get1_peer_certificate(); return nullptr; }\n',
            "datachannel.cpp": '#include <rtc/peerconnection.hpp>\n'
                        'extern "C" void EVP_PKEY_Q_keygen();\n'
                        'rtc::PeerConnection::PeerConnection() { EVP_PKEY_Q_keygen(); }\n'
                        'rtc::PeerConnection::~PeerConnection() = default;\n',
            "ssl.cpp": '#ifdef MATCHED_TLS\nextern "C" void SSL_get1_peer_certificate() {}\n#endif\n',
            "crypto.cpp": '#ifdef MATCHED_TLS\nextern "C" void EVP_PKEY_Q_keygen() {}\n#endif\n',
            "z.cpp": 'void unused_zlib_stub() {}\n',
        }
        for name, contents in files.items():
            (self.source / name).write_text(contents)
        (self.source / "CMakeLists.txt").write_text("""
cmake_minimum_required(VERSION 3.19)
project(TLSFixtures LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(name curl datachannel ssl crypto z)
    add_library(${name} STATIC ${name}.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    if(MATCHED_TLS)
        target_compile_definitions(${name} PRIVATE MATCHED_TLS)
    endif()
endforeach()
""")
        self.build = self.root / "built"
        self.check = self.root / "check"
        self.check.mkdir()
        imports = []
        for target, library in (("CURL::libcurl", "curl"), ("LibDataChannel::LibDataChannel", "datachannel"),
                                ("OpenSSL::SSL", "ssl"), ("OpenSSL::Crypto", "crypto"), ("ZLIB::ZLIB", "z")):
            imports.append(f'add_library({target} STATIC IMPORTED)\n'
                f'set_target_properties({target} PROPERTIES IMPORTED_LOCATION "{self.build}/lib/lib{library}.a" '
                f'INTERFACE_INCLUDE_DIRECTORIES "{include}")\n')
        (self.check / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.19)\nproject(CheckTLS LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\n' + "".join(imports) +
            f'include("{ROOT}/cmake/modules/CheckBundledTLS.cmake")\n')

    def command(self, *args):
        command = ["cmake", *map(str, args)]
        if sys.platform == "darwin" and "-S" in args:
            command.append("-DCMAKE_OSX_SYSROOT=macosx")
        return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=90)

    def build_libraries(self, matched):
        result = self.command("-S", self.source, "-B", self.build, "-G", "Unix Makefiles",
                              "-DMATCHED_TLS=" + ("ON" if matched else "OFF"))
        self.assertEqual(result.returncode, 0, result.stdout)
        result = self.command("--build", self.build, "--clean-first", "--parallel", "2")
        self.assertEqual(result.returncode, 0, result.stdout)

    def configure_check(self):
        return self.command("-S", self.check, "-B", self.root / "checked", "-G", "Unix Makefiles")

    def test_matching_tls_libraries_link(self):
        self.build_libraries(True)
        result = self.configure_check()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_mismatch_fails_early_and_repair_is_rechecked(self):
        self.build_libraries(False)
        result = self.configure_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Rebuild the macOS dependencies", result.stdout)
        self.build_libraries(True)
        result = self.configure_check()
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
