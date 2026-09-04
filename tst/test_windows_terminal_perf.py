# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import importlib.util
import sys
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def benchmark_module():
    loader = SourceFileLoader(
        "shitty_windows_terminal_perf",
        str(ROOT / "dev/windows/terminal_perf.py"),
    )
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[loader.name] = module
    loader.exec_module(module)
    return module


class WindowsTerminalPerfTests(unittest.TestCase):
    def test_windows_pty_prefers_bundled_conpty(self):
        source = (ROOT / "lib/shitty/pty_windows.cpp").read_text()

        self.assertIn("GetModuleFileNameW", source)
        self.assertIn("LoadLibraryExW", source)
        self.assertIn("LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR", source)
        self.assertIn('GetProcAddress(module, "ConptyCreatePseudoConsole")', source)
        self.assertIn('GetProcAddress(module, "ConptyReleasePseudoConsole")', source)
        self.assertIn("CreatePseudoConsole", source)
        self.assertIn("ConptyApi(const ConptyApi&) = delete", source)
        self.assertIn("PseudoConsole(const PseudoConsole&) = delete", source)
        self.assertIn("block->used = block->storage.size()", source)

    def test_win32_frames_are_coalesced_before_paint(self):
        source = (ROOT / "ext/plt/platform_win32.cpp").read_text()

        self.assertIn("frameDelayMilliseconds = 8", source)
        self.assertIn("SetTimer(handle, frameTimerId", source)
        self.assertIn("case WM_TIMER:", source)

    def test_win32_attention_is_rate_limited(self):
        source = (ROOT / "ext/plt/platform_win32.cpp").read_text()

        self.assertIn("attentionIntervalMilliseconds = 1000", source)
        self.assertIn("GetTickCount64", source)
        self.assertIn("GetForegroundWindow", source)

    def test_workloads_match_readme_sizes_and_payloads(self):
        module = benchmark_module()

        ascii_chunk = module.AsciiPayload(80, 0x5117).chunk()
        random_chunk = module.RandomPayload(0x5117).chunk(4096)

        self.assertEqual(module.ASCII_PAYLOAD_BYTES, 1_000_000_000)
        self.assertEqual(module.RANDOM_PAYLOAD_BYTES, 100_000_000)
        self.assertTrue(all(byte == 10 or 0x20 <= byte < 0x7F for byte in ascii_chunk))
        self.assertEqual(random_chunk, module.RandomPayload(0x5117).chunk(4096))

    def test_writer_handles_partial_pipe_writes(self):
        source = (ROOT / "dev/windows/terminal_perf.py").read_text()

        self.assertIn("while offset != len(chunk):", source)
        self.assertIn("offset += os.write(output, chunk[offset:])", source)

    def test_terminals_launch_the_same_child(self):
        module = benchmark_module()
        child = ("python.exe", "terminal_perf.py", "write")

        shitty = module.ShittyTerminal(
            Path("st.exe"),
            Path("shitty.toml"),
            "Cascadia Mono",
            12,
            500,
            23,
            6,
        )
        windows = module.WindowsTerminal(Path("wt.exe"), "Shitty Benchmark", -1)

        self.assertEqual(shitty.command(child, 80, 24)[-3:], child)
        self.assertEqual(windows.command(child, 80, 24)[-3:], child)
        self.assertIn("103x30", shitty.command(child, 80, 24))
        self.assertIn("79,24", windows.command(child, 80, 24))
        self.assertIn("Shitty Benchmark", windows.command(child, 80, 24))
        self.assertIn("500", shitty.command(child, 80, 24))


if __name__ == "__main__":
    unittest.main()
