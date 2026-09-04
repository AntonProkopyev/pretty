# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsPackageContractTests(unittest.TestCase):
    def test_packager_is_deterministic_and_audits_closure(self):
        source = (ROOT / "dev/windows/package.py").read_text()

        self.assertIn("recursive_closure", source)
        self.assertIn("ZipInfo", source)
        self.assertIn("SHA256SUMS", source)
        self.assertIn("SOURCE_DATE_EPOCH", source)

    def test_packager_includes_configs_and_licenses(self):
        source = (ROOT / "dev/windows/package.py").read_text()

        for text in (
            "shitty.toml",
            "pretty.toml",
            "LICENSE.MIT",
            "LICENSE.GPL3",
            "LICENSE.NotoColorEmoji",
            "llvm-mingw.txt",
        ):
            with self.subTest(text=text):
                self.assertIn(text, source)

    def test_readme_documents_native_windows_runtime(self):
        readme = (ROOT / "README.md").read_text()

        self.assertIn("Windows 11 x64", readme)
        self.assertIn("%LOCALAPPDATA%", readme)
        self.assertIn("Vulkan", readme)
        self.assertIn("ConPTY", readme)

    def test_release_workflow_publishes_tested_windows_zip(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()

        self.assertIn("build-windows:", workflow)
        self.assertIn("test-windows:", workflow)
        self.assertIn("shitty-windows-x86_64.zip", workflow)
        self.assertIn("--extra-artifact", workflow)


if __name__ == "__main__":
    unittest.main()
