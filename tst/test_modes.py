# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class ModeTest(unittest.TestCase):
    def test_smooth_scroll_mode_coalesces_into_one_update(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.write(b"\x1b[?4h" + b"x\r\n" * 100)
            refreshes = terminal.snapshot().refresh_count - before

        self.assertEqual(refreshes, 1)

    def test_smooth_scroll_mode_survives_alternate_screen_reentry(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.write(
                b"\x1b[?4h\x1b[?1049h"
                b"\x1b[S\x1b[S"
                b"\x1b[?1049h"
                + b"a\xcc\x81" * 7
            )
            snapshot = terminal.model_snapshot()

        self.assertEqual(snapshot.lines[0][:7], "a" * 7)
        self.assertEqual(
            [snapshot.cell(column, 0).grapheme for column in range(7)],
            [(ord("a"), 0x301)] * 7,
        )

    def test_new_line_mode_applies_to_lf_vt_and_ff(self):
        for control in (b"\n", b"\v", b"\f"):
            with self.subTest(control=control), Shitty(columns=8, rows=3) as terminal:
                terminal.write(b"\x1b[20h\x1b[1;4H" + control + b"X")
                snapshot = terminal.snapshot()
                self.assertEqual(snapshot.cell(0, 1).char, "X")
                self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (1, 1))

    def test_blinking_text_drives_periodic_refresh(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[2 q\x1b[5mX")
            before = terminal.snapshot().refresh_count
            terminal.blink_tick()
            self.assertGreater(terminal.snapshot().refresh_count, before)

    def test_autowrap_can_be_disabled_and_restored(self):
        with Shitty(columns=4, rows=2) as terminal:
            terminal.write(b"\x1b[?7labcde")
            self.assertEqual(terminal.snapshot().lines, ["abce", "    "])
            terminal.write(b"\x1b[?7h\r12345")
            self.assertEqual(terminal.snapshot().lines, ["1234", "5   "])

    def test_cursor_visibility_and_shape(self):
        with Shitty(columns=4, rows=2) as terminal:
            terminal.write(b"\x1b[?25l")
            self.assertEqual(terminal.snapshot().cursor_style, 0)
            terminal.focus(True)
            terminal.write(b"\x1b[5 q\x1b[?25h")
            self.assertEqual(terminal.snapshot().cursor_style, 4)

    def test_configured_bar_cursor_is_initial_shape(self):
        with Shitty(
            columns=4,
            rows=2,
            extra_arguments=("-cursorShape", "bar"),
        ) as terminal:
            terminal.focus(True)
            self.assertEqual(terminal.snapshot().cursor_style, 4)

    def test_alternate_screen_restores_primary_contents_and_cursor(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"primary\x1b[?1049halt\x1b[?1049l")
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.lines[0], "primary ")
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (7, 0))

    def test_mouse_tracking_modes_are_reported(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1003h\x1b[?1006h\x1b[?1004h")
            self.assertEqual(terminal.state()[:3], (4, 2, 1))
            terminal.write(b"\x1b[?1003l\x1b[?1006l\x1b[?1004l")
            self.assertEqual(terminal.state()[:3], (0, 0, 0))

    def test_focus_reporting_protocol(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.focus(True)
            self.assertEqual(terminal.read_input(), b"")
            terminal.write(b"\x1b[?1004h")
            terminal.focus(False)
            terminal.focus(True)
            self.assertEqual(terminal.read_input(), b"\x1b[O\x1b[I")

    def test_private_modes_can_be_saved_and_restored(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(
                b"\x1b[?1003;1006;1004h"
                b"\x1b[?1003;1006;1004s"
                b"\x1b[?1003;1006;1004l"
            )
            self.assertEqual(terminal.state()[:3], (0, 0, 0))
            terminal.write(b"\x1b[?1003;1006;1004r")
            self.assertEqual(terminal.state()[:3], (4, 2, 1))

    def test_alternate_scroll_mode_sends_cursor_keys(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1007h\x1b[?1049h")
            terminal.page_up()
            terminal.page_down()
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[A\x1b[A\x1b[B\x1b[B",
            )

    def test_synchronized_output_defers_refresh(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.write(b"\x1b[?2026hhidden")
            during = terminal.snapshot()
            self.assertEqual(during.refresh_count, before)
            terminal.write(b"\x1b[?2026l")
            after = terminal.snapshot()
            self.assertGreater(after.refresh_count, before)
            self.assertEqual(after.lines[0], "hidden  ")

    def test_synchronized_output_watchdog_releases_stuck_frame(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.write(b"\x1b[?2026hstuck")
            self.assertEqual(terminal.snapshot().refresh_count, before)
            terminal.sync_timeout()
            after = terminal.snapshot()
            self.assertGreater(after.refresh_count, before)
            self.assertEqual(after.lines[0], "stuck   ")

    def test_synchronized_output_timeout_is_single_shot_and_reusable(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.sync_timeout()
            before = terminal.snapshot().refresh_count
            terminal.write(b"\x1b[?2026htimed")
            terminal.sync_timeout()
            released = terminal.snapshot()
            self.assertEqual(released.lines[0], "timed   ")
            self.assertEqual(released.refresh_count, before + 1)

            terminal.sync_timeout()
            self.assertEqual(terminal.snapshot().refresh_count, before + 1)
            terminal.write(b"\x1b[?2026h-again\x1b[?2026l")
            after = terminal.snapshot()
            self.assertEqual(after.lines, ["timed-ag", "ain     "])
            self.assertEqual(after.refresh_count, before + 2)


if __name__ == "__main__":
    unittest.main()
