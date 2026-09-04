# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import unittest
import zipfile
from importlib.machinery import SourceFileLoader
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FETCH = ROOT / "dev" / "windows" / "fetch_toolchain.py"
LOCK = ROOT / "dev" / "windows" / "toolchain.lock"
PROBE = ROOT / "dev" / "windows" / "probe_toolchain.py"
PROBE_SOURCE = ROOT / "dev" / "windows" / "probe.cpp"
VCPKG = ROOT / "dev" / "windows" / "vcpkg.json"
CI = ROOT / ".github" / "workflows" / "ci.yml"


class WindowsToolchainTests(unittest.TestCase):
    def run_fetch(self, lock, platform, output):
        return subprocess.run(
            [
                sys.executable,
                FETCH,
                "--lock", lock,
                "--platform", platform,
                "--output", output,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def write_lock(self, path, platform, archive, digest):
        path.write_text(
            "version = 1\n"
            "vcpkg_commit = \"abb6dda5cc32914d2e64d7d72b974dc301d1fc8a\"\n"
            f"[assets.{platform}]\n"
            f"url = \"{archive.as_uri()}\"\n"
            f"sha256 = \"{digest}\"\n"
        )

    def test_lock_pins_official_toolchains_and_vcpkg(self):
        lock = tomllib.loads(LOCK.read_text())

        self.assertEqual(lock["version"], 1)
        self.assertEqual(
            lock["vcpkg_commit"],
            "abb6dda5cc32914d2e64d7d72b974dc301d1fc8a",
        )
        self.assertEqual(
            lock["assets"]["linux_x86_64"],
            {
                "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz",
                "sha256": "cee8d2ce3da5145ce4dc882e70d0b0719a783d53a99752c60948fc0659975a65",
            },
        )
        self.assertEqual(
            lock["assets"]["windows_x86_64"],
            {
                "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-x86_64.zip",
                "sha256": "ae601f4e0f72bbdf441ad2df8bb16f037e2e9251559ea6b37b4057aef39c06c3",
            },
        )

    def test_vcpkg_manifest_uses_locked_baseline_and_dependencies(self):
        lock = tomllib.loads(LOCK.read_text())
        manifest = json.loads(VCPKG.read_text())

        self.assertEqual(manifest["builtin-baseline"], lock["vcpkg_commit"])
        self.assertEqual(
            manifest["dependencies"],
            [
                "brotli",
                "freetype",
                "harfbuzz",
                "simdutf",
                "vulkan-headers",
                "vulkan-loader",
            ],
        )

    def test_fetch_extracts_zip_and_tar_archives(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = b"clang"
            archives = {
                "windows_x86_64": root / "toolchain.zip",
                "linux_x86_64": root / "toolchain.tar.xz",
            }
            with zipfile.ZipFile(archives["windows_x86_64"], "w") as archive:
                archive.writestr("llvm-mingw/bin/clang", payload)
            source = root / "clang"
            source.write_bytes(payload)
            with tarfile.open(archives["linux_x86_64"], "w:xz") as archive:
                archive.add(source, arcname="llvm-mingw/bin/clang")

            for platform, archive in archives.items():
                with self.subTest(platform=platform):
                    lock = root / f"{platform}.lock"
                    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                    self.write_lock(lock, platform, archive, digest)
                    output = root / f"out-{platform}"

                    result = self.run_fetch(lock, platform, output)

                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((output / "bin" / "clang").read_bytes(), payload)
                    repeated = self.run_fetch(lock, platform, output)
                    self.assertEqual(repeated.returncode, 0, repeated.stderr)

    def test_fetch_rejects_wrong_checksum_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "toolchain.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("llvm-mingw/bin/clang", b"clang")
            lock = root / "toolchain.lock"
            self.write_lock(lock, "windows_x86_64", archive, "0" * 64)
            output = root / "out"

            result = self.run_fetch(lock, "windows_x86_64", output)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checksum", result.stderr.lower())
            self.assertFalse(output.exists())

    def test_fetch_rejects_archive_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "toolchain.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("../escape", b"escape")
            lock = root / "toolchain.lock"
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.write_lock(lock, "windows_x86_64", archive, digest)
            output = root / "out"

            result = self.run_fetch(lock, "windows_x86_64", output)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("escapes destination", result.stderr)
            self.assertFalse((root / "escape").exists())

    def test_probe_command_pins_windows_target_flags(self):
        loader = SourceFileLoader("shitty_windows_toolchain_probe", str(PROBE))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        probe = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = probe
        loader.exec_module(probe)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "bin" / "x86_64-w64-mingw32-clang++"
            compiler.parent.mkdir()
            compiler.touch()
            output = root / "probe.exe"

            command = probe.compile_command(root, PROBE_SOURCE, output)

            self.assertEqual(
                command,
                [
                    str(compiler),
                    "-std=c++26",
                    "-fuse-ld=lld",
                    "-static",
                    "-Wl,--subsystem,console",
                    str(PROBE_SOURCE),
                    "-o",
                    str(output),
                ],
            )

    def test_probe_parses_pe32_plus_amd64_console(self):
        loader = SourceFileLoader("shitty_windows_toolchain_pe", str(PROBE))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        probe = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = probe
        loader.exec_module(probe)

        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "probe.exe"
            content = bytearray(0x200)
            content[0:2] = b"MZ"
            struct.pack_into("<I", content, 0x3C, 0x80)
            content[0x80:0x84] = b"PE\0\0"
            struct.pack_into("<H", content, 0x84, 0x8664)
            struct.pack_into("<H", content, 0x94, 0xF0)
            struct.pack_into("<H", content, 0x98, 0x20B)
            struct.pack_into("<H", content, 0x98 + 68, 3)
            image.write_bytes(content)

            contract = probe.read_contract(image)

            self.assertEqual(contract.machine, 0x8664)
            self.assertEqual(contract.magic, 0x20B)
            self.assertEqual(contract.subsystem, 3)

    def test_ci_runs_pinned_toolchain_probe_on_windows_server(self):
        workflow = CI.read_text()

        required = [
            "windows-toolchain:",
            "runs-on: windows-2025",
            "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
            "--platform windows_x86_64",
            "dev/windows/probe_toolchain.py",
            "& $probe",
        ]
        for text in required:
            with self.subTest(text=text):
                self.assertTrue(text in workflow)


if __name__ == "__main__":
    unittest.main()
