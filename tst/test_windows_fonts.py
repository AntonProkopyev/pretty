# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsFontContractTests(unittest.TestCase):
    def test_font_file_uses_win32_mapping(self):
        source = (ROOT / "lib/shitty/font_face.cpp").read_text()

        for text in ("CreateFileW", "CreateFileMappingW", "MapViewOfFile"):
            with self.subTest(text=text):
                self.assertIn(text, source)

    def test_directwrite_resolver_is_registered(self):
        source = (ROOT / "lib/shitty/font_windows.cpp").read_text()
        composer = (ROOT / "lib/shitty/composer.cpp").read_text()

        self.assertIn("DWriteCreateFactory", source)
        self.assertIn("FindFamilyName", source)
        self.assertIn("createWindowsFontResolver", composer)

    def test_directwrite_files_stream_into_owned_faces(self):
        source = (ROOT / "lib/shitty/font_windows.cpp").read_text()

        self.assertIn("CreateStreamFromKey", source)
        self.assertIn("createOwnedFontFace", source)

    def test_windows_font_probe_is_in_build_graph(self):
        build = (ROOT / "windows_build.py").read_text()

        self.assertIn("font_test.cpp", build)
        self.assertIn("-ldwrite", build)
        self.assertIn('group("windows-font-test"', build)


if __name__ == "__main__":
    unittest.main()
