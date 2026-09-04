# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsCiContractTests(unittest.TestCase):
    def test_windows_build_has_aggregate_test_group(self):
        build = (ROOT / "windows_build.py").read_text()

        self.assertIn('group("windows-test"', build)

    def test_ci_separates_cross_build_and_server_runtime(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()

        self.assertIn("windows-cross:", workflow)
        self.assertIn("windows-runtime:", workflow)
        self.assertIn("needs: windows-cross", workflow)
        self.assertIn("proxy evidence", workflow)

    def test_pe_audit_is_recursive_and_rejects_posix_runtimes(self):
        source = (ROOT / "dev/windows/audit_pe.py").read_text()

        self.assertIn("msys-2.0.dll", source)
        self.assertIn("cygwin1.dll", source)
        self.assertIn("missing", source)
        self.assertIn("recursive", source)


if __name__ == "__main__":
    unittest.main()
