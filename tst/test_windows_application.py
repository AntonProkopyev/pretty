# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsApplicationContractTests(unittest.TestCase):
    def test_release_uses_gui_subsystem_and_unicode_entry(self):
        build = (ROOT / "build.py").read_text()
        main = (ROOT / "lib/shitty/main.cpp").read_text()
        shitty = (ROOT / "bin/st/main.cpp").read_text()
        pretty = (ROOT / "bin/pt/main.cpp").read_text()

        self.assertIn("-mwindows", build)
        self.assertIn("-municode", build)
        self.assertIn("CommandLineToArgvW", main)
        self.assertIn("wWinMain", shitty)
        self.assertIn("wWinMain", pretty)

    def test_windows_config_uses_known_folder(self):
        options = (ROOT / "lib/shitty/options.cpp").read_text()

        self.assertIn("SHGetKnownFolderPath", options)
        self.assertIn("FOLDERID_LocalAppData", options)

    def test_windows_shutdown_returns_typed_child_exit(self):
        application = (ROOT / "lib/shitty/application.cpp").read_text()
        session = (ROOT / "lib/shitty/session.cpp").read_text()

        self.assertIn("lastExit()", application)
        self.assertIn("exitResult()", session)
        self.assertIn("composer.platform->stop()", application)

    def test_windows_poller_supports_multiple_native_handles(self):
        poller = (ROOT / "ext/plt/poller_loop.cpp").read_text()

        self.assertIn("nativeHandles", poller)
        self.assertIn("MAXIMUM_WAIT_OBJECTS", poller)


if __name__ == "__main__":
    unittest.main()
