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
        self.assertIn("AttachConsole", main)
        self.assertIn("startup.log", main)
        self.assertIn("wWinMain", shitty)
        self.assertIn("wWinMain", pretty)

    def test_windows_config_uses_known_folder(self):
        options = (ROOT / "lib/shitty/options.cpp").read_text()

        self.assertIn("SHGetKnownFolderPath", options)
        self.assertIn("FOLDERID_LocalAppData", options)
        self.assertIn('getenv("COMSPEC")', options)

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

    def test_release_embeds_manifest_and_brand_icons(self):
        build = (ROOT / "build.py").read_text()
        manifest = (ROOT / "dev/windows/app.manifest").read_text()
        resource = (ROOT / "dev/windows/make_resource.py").read_text()

        self.assertIn("shitty_windows_resource", build)
        self.assertIn("pretty_windows_resource", build)
        self.assertIn("PerMonitorV2", manifest)
        self.assertIn("supportedOS", manifest)
        self.assertIn("RT_MANIFEST", resource)
        self.assertIn(" ICON ", resource)
        self.assertIn("MAKEINTRESOURCEW(1)", (ROOT / "ext/plt/platform_win32.cpp").read_text())
        self.assertIn("description.hIconSm", (ROOT / "ext/plt/platform_win32.cpp").read_text())

    def test_windows_config_file_boundary_uses_utf16(self):
        source = (ROOT / "ext/libstd/std/ios/fs_utils.cpp").read_text()

        self.assertIn("MultiByteToWideChar", source)
        self.assertIn("_wopen", source)

    def test_windows_config_reload_uses_input_action(self):
        bindings = (ROOT / "lib/shitty/input_bindings.h").read_text()
        application = (ROOT / "lib/shitty/application.cpp").read_text()

        self.assertIn("ReloadConfig", bindings)
        self.assertIn("InputActions::ReloadConfig", application)
        self.assertIn("composer.config->reload()", application)

    def test_windows_decorated_frame_is_client_drawn(self):
        platform = (ROOT / "ext/plt/platform_win32.cpp").read_text()
        build = (ROOT / "build.py").read_text()
        windows_build = (ROOT / "windows_build.py").read_text()

        self.assertIn("WS_CHILD | WS_VISIBLE", platform)
        self.assertIn("paintChrome", platform)
        self.assertIn("requestTabs", platform)
        self.assertIn("case WM_NCCALCSIZE:", platform)
        self.assertIn("case WM_NCHITTEST:", platform)
        self.assertIn("HTCAPTION", platform)
        self.assertIn("HTTOP", platform)
        self.assertIn('"-lgdi32"', build)
        self.assertIn('"-lgdi32"', windows_build)

    def test_windows_global_hotkey_toggles_the_window(self):
        options = (ROOT / "lib/shitty/options.cpp").read_text()
        application = (ROOT / "lib/shitty/application.cpp").read_text()
        platform = (ROOT / "ext/plt/platform_win32.cpp").read_text()

        self.assertIn('{"globalHotkey", OptionKind::SepArg', options)
        self.assertIn(".globalToggleHotkey", application)
        self.assertIn("RegisterHotKey", platform)
        self.assertIn("case WM_HOTKEY:", platform)
        self.assertIn("UnregisterHotKey", platform)
        self.assertIn("VK_OEM_3", platform)


if __name__ == "__main__":
    unittest.main()
