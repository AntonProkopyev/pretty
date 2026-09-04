# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsConPtyContractTests(unittest.TestCase):
    def test_probe_covers_conpty_lifecycle(self):
        source = (ROOT / "dev/windows/conpty_test.cpp").read_text()

        required = [
            "CreatePseudoConsole",
            "PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE",
            "CreateProcessW",
            "ResizePseudoConsole",
            "GetExitCodeProcess",
            "ClosePseudoConsole",
            "std::thread",
            "quoteArgument",
        ]
        for text in required:
            with self.subTest(text=text):
                self.assertIn(text, source)

    def test_probe_is_in_windows_build_graph(self):
        build = (ROOT / "windows_build.py").read_text()

        self.assertIn("conpty_test.cpp", build)
        self.assertIn('group("windows-conpty-test"', build)

    def test_production_pty_has_windows_backend_and_typed_exit(self):
        build = (ROOT / "build.py").read_text()
        contract = (ROOT / "lib/vterm/pty.h").read_text()
        backend = (ROOT / "lib/shitty/pty_windows.cpp").read_text()

        self.assertIn("pty_windows.cpp", build)
        self.assertIn("PtyExitResult", contract)
        self.assertIn("GetExitCodeProcess", backend)


if __name__ == "__main__":
    unittest.main()
