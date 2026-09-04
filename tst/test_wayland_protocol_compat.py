# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WaylandProtocolCompatibilityTests(unittest.TestCase):
    def test_text_input_listener_uses_standard_v3_callbacks(self):
        source = (ROOT / "ext/plt/platform_wayland.cpp").read_text()

        unsupported = (
            ".action = [](void*, struct zwp_text_input_v3*",
            ".language = [](void*, struct zwp_text_input_v3*",
            ".preedit_hint = [](void*, struct zwp_text_input_v3*",
        )
        for callback in unsupported:
            with self.subTest(callback=callback):
                self.assertNotIn(callback, source)

    def test_fake_server_uses_installed_protocol_interfaces(self):
        source = (ROOT / "ext/plt/tests/test.cpp").read_text()

        unsupported = (
            ".get_release = nullptr",
            ".set_available_actions =",
            ".show_input_panel =",
            ".hide_input_panel =",
        )
        for callback in unsupported:
            with self.subTest(callback=callback):
                self.assertNotIn(callback, source)
        for implementation in (
            "compositorImplementation",
            "dataManagerImplementation",
        ):
            start = source.index(implementation)
            end = source.index("\n    };", start)
            with self.subTest(implementation=implementation):
                self.assertNotIn(".release =", source[start:end])


if __name__ == "__main__":
    unittest.main()
