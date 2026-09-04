# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import json
import os
import platform
import random
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


COLUMNS = 80
ROWS = 24
FONT = "Cascadia Mono"
FONT_SIZE = 12
SCROLLBACK_LINES = 500
ASCII_PAYLOAD_BYTES = 1_000_000_000
RANDOM_PAYLOAD_BYTES = 100_000_000
SEED = 0x5117
MIB = 1 << 20


@dataclass(frozen=True)
class AsciiPayload:
    columns: int
    seed: int

    def chunk(self):
        rng = random.Random(self.seed)
        printable = bytes(range(0x20, 0x7F))
        return bytes(rng.choice(printable) for _ in range(self.columns)) + b"\n"

    def write(self, path, size):
        line = self.chunk()
        block = line * max(1, MIB // len(line))
        with path.open("wb") as output:
            written = 0
            while written < size:
                chunk = block[:size - written]
                output.write(chunk)
                written += len(chunk)
        return path


@dataclass(frozen=True)
class RandomPayload:
    seed: int

    def generator(self):
        rng = random.Random(self.seed)
        printable = bytes(range(0x20, 0x7F))
        tuple(rng.choice(printable) for _ in range(COLUMNS))
        return rng

    def chunk(self, size):
        return self.generator().randbytes(size)

    def write(self, path, size):
        rng = self.generator()
        with path.open("wb") as output:
            written = 0
            while written < size:
                chunk = rng.randbytes(min(MIB, size - written))
                output.write(chunk)
                written += len(chunk)
        return path


@dataclass(frozen=True)
class Payloads:
    directory: Path
    ascii_size: int
    random_size: int

    def ensure(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        ascii_path = self.directory / "ascii.bin"
        random_path = self.directory / "random.bin"
        if not ascii_path.is_file() or ascii_path.stat().st_size != self.ascii_size:
            AsciiPayload(COLUMNS, SEED).write(ascii_path, self.ascii_size)
        if not random_path.is_file() or random_path.stat().st_size != self.random_size:
            RandomPayload(SEED).write(random_path, self.random_size)
        return (
            Workload("ascii", ascii_path, self.ascii_size),
            Workload("random", random_path, self.random_size),
        )


@dataclass(frozen=True)
class ShittyTerminal:
    executable: Path
    config: Path
    font: str
    font_size: int
    scrollback_lines: int
    column_delta: int
    row_delta: int

    def label(self):
        return "shitty"

    def command(self, child, width, height):
        return (
            str(self.executable),
            "-config",
            str(self.config),
            "-font",
            self.font,
            "-fontsize",
            str(self.font_size),
            "-geometry",
            f"{max(1, width + self.column_delta)}x{max(1, height + self.row_delta)}",
            "-saveLines",
            str(self.scrollback_lines),
            "-e",
            *child,
        )


@dataclass(frozen=True)
class WindowsTerminal:
    executable: Path
    profile: str
    column_delta: int

    def label(self):
        return "windows-terminal"

    def command(self, child, width, height):
        return (
            str(self.executable),
            "-w",
            "new",
            "--size",
            f"{max(1, width + self.column_delta)},{height}",
            "new-tab",
            "--profile",
            self.profile,
            "--title",
            "terminal-perf",
            *child,
        )


@dataclass(frozen=True)
class Workload:
    name: str
    payload: Path
    size: int


@dataclass(frozen=True)
class ProcessTimes:
    process: object

    def cpu_ns(self):
        if os.name != "nt":
            return 0
        import ctypes
        from ctypes import wintypes

        created = wintypes.FILETIME()
        exited = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not ctypes.windll.kernel32.GetProcessTimes(
                int(self.process._handle),
                ctypes.byref(created),
                ctypes.byref(exited),
                ctypes.byref(kernel),
                ctypes.byref(user)):
            raise ctypes.WinError()
        ticks = (
            (kernel.dwHighDateTime << 32) | kernel.dwLowDateTime
        ) + ((user.dwHighDateTime << 32) | user.dwLowDateTime)
        return ticks * 100


@dataclass(frozen=True)
class Writer:
    payload: Path
    expected_size: int
    result: Path

    def run(self):
        if self.payload.stat().st_size != self.expected_size:
            raise RuntimeError(f"unexpected payload size: {self.payload}")
        terminal = os.get_terminal_size(sys.stdout.fileno())
        output = sys.stdout.fileno()
        started = time.perf_counter_ns()
        cpu_started = time.process_time_ns()
        written = 0
        with self.payload.open("rb") as source:
            while chunk := source.read(MIB):
                offset = 0
                while offset != len(chunk):
                    offset += os.write(output, chunk[offset:])
                written += len(chunk)
        elapsed_ns = time.perf_counter_ns() - started
        cpu_ns = time.process_time_ns() - cpu_started
        receipt = {
            "bytes": written,
            "columns": terminal.columns,
            "cpu_ns": cpu_ns,
            "elapsed_ns": elapsed_ns,
            "lines": terminal.lines,
        }
        temporary = self.result.with_suffix(self.result.suffix + ".tmp")
        temporary.write_text(json.dumps(receipt, sort_keys=True) + "\n")
        os.replace(temporary, self.result)
        return 0


@dataclass(frozen=True)
class Attempt:
    terminal: object
    workload: Workload
    python: Path
    script: Path
    directory: Path
    timeout_seconds: float

    def run(self, result, iteration):
        child = (
            str(self.python),
            str(self.script),
            "write",
            "--payload",
            str(self.workload.payload),
            "--expected-size",
            str(self.workload.size),
            "--result",
            str(result),
        )
        command = self.terminal.command(child, COLUMNS, ROWS)
        started = time.perf_counter_ns()
        process = subprocess.Popen(command, cwd=self.directory)
        deadline = time.monotonic() + self.timeout_seconds
        while not result.is_file() and time.monotonic() < deadline:
            time.sleep(0.01)
        if not result.is_file():
            process.terminate()
            process.wait(timeout=10)
            raise TimeoutError(
                f"{self.terminal.label()} {self.workload.name} timed out"
            )
        wall_ns = time.perf_counter_ns() - started
        launcher_exit = process.wait(timeout=15)
        terminal_cpu_ns = ProcessTimes(process).cpu_ns()
        receipt = json.loads(result.read_text())
        if (receipt["columns"], receipt["lines"]) != (COLUMNS, ROWS):
            raise RuntimeError(
                f"{self.terminal.label()} grid is "
                f"{receipt['columns']}x{receipt['lines']}, expected {COLUMNS}x{ROWS}"
            )
        receipt.update({
            "case": self.workload.name,
            "iteration": iteration,
            "launcher_exit": launcher_exit,
            "terminal": self.terminal.label(),
            "terminal_cpu_ns": terminal_cpu_ns,
            "wall_ns": wall_ns,
        })
        time.sleep(0.35)
        return receipt


@dataclass(frozen=True)
class Report:
    samples: tuple
    runs: int
    font: str
    font_size: int
    scrollback_lines: int
    windows_terminal_version: str
    windows_terminal_profile: str
    windows_terminal_column_delta: int
    shitty_column_delta: int
    shitty_row_delta: int
    shitty_sha256: str
    os_build: str

    def best(self, name, terminal):
        choices = [
            sample for sample in self.samples
            if sample["case"] == name and sample["terminal"] == terminal
        ]
        sample = min(choices, key=lambda item: item["wall_ns"])
        wall = sample["wall_ns"] / 1_000_000_000
        return {
            "best_wall_s": wall,
            "throughput_mib_s": sample["bytes"] / MIB / wall,
            "writer_elapsed_s": sample["elapsed_ns"] / 1_000_000_000,
        }

    def content(self):
        cases = {}
        for name in ("ascii", "random"):
            shitty = self.best(name, "shitty")
            windows = self.best(name, "windows-terminal")
            cases[name] = {
                "shitty": shitty,
                "windows-terminal": windows,
                "throughput_ratio_shitty_over_windows_terminal": (
                    shitty["throughput_mib_s"] / windows["throughput_mib_s"]
                ),
            }
        return {
            "environment": {
                "font": self.font,
                "font_size": self.font_size,
                "grid": f"{COLUMNS}x{ROWS}",
                "host": platform.platform(),
                "os_build": self.os_build,
                "python": sys.version,
                "scrollback_lines_requested": self.scrollback_lines,
                "shitty_sha256": self.shitty_sha256,
                "windows_terminal_version": self.windows_terminal_version,
                "windows_terminal_profile": self.windows_terminal_profile,
                "windows_terminal_column_delta": self.windows_terminal_column_delta,
                "shitty_column_delta": self.shitty_column_delta,
                "shitty_row_delta": self.shitty_row_delta,
            },
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "method": "README cat workload, best wall time",
            "runs": self.runs,
            "samples": list(self.samples),
            "schema": 1,
            "summary": cases,
        }


@dataclass(frozen=True)
class Comparison:
    shitty: ShittyTerminal
    windows: WindowsTerminal
    workloads: tuple
    python: Path
    script: Path
    output: Path
    runs: int
    timeout_seconds: float
    windows_terminal_version: str
    shitty_sha256: str
    os_build: str

    def run(self):
        receipts = self.output.parent / (self.output.stem + "-samples")
        receipts.mkdir(parents=True, exist_ok=True)
        samples = []
        terminals = (self.shitty, self.windows)
        for workload in self.workloads:
            for iteration in range(self.runs):
                order = terminals if iteration % 2 == 0 else tuple(reversed(terminals))
                for terminal in order:
                    result = receipts / (
                        f"{workload.name}-{terminal.label()}-{iteration}.json"
                    )
                    result.unlink(missing_ok=True)
                    sample = Attempt(
                        terminal,
                        workload,
                        self.python,
                        self.script,
                        self.output.parent,
                        self.timeout_seconds,
                    ).run(result, iteration)
                    samples.append(sample)
                    wall = sample["wall_ns"] / 1_000_000_000
                    rate = sample["bytes"] / MIB / wall
                    print(
                        f"{workload.name} {terminal.label()} "
                        f"wall={wall:.3f}s throughput={rate:.1f} MiB/s"
                    )
        report = Report(
            tuple(samples),
            self.runs,
            self.shitty.font,
            self.shitty.font_size,
            self.shitty.scrollback_lines,
            self.windows_terminal_version,
            self.windows.profile,
            self.windows.column_delta,
            self.shitty.column_delta,
            self.shitty.row_delta,
            self.shitty_sha256,
            self.os_build,
        ).content()
        self.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(json.dumps(report["summary"], indent=2, sort_keys=True))
        return 0


def parser():
    result = argparse.ArgumentParser()
    modes = result.add_subparsers(dest="mode", required=True)
    writer = modes.add_parser("write")
    writer.add_argument("--payload", type=Path, required=True)
    writer.add_argument("--expected-size", type=int, required=True)
    writer.add_argument("--result", type=Path, required=True)
    compare = modes.add_parser("compare")
    compare.add_argument("--shitty", type=Path, required=True)
    compare.add_argument("--config", type=Path, required=True)
    compare.add_argument("--windows-terminal", type=Path, required=True)
    compare.add_argument("--windows-terminal-profile", required=True)
    compare.add_argument("--windows-terminal-column-delta", type=int, default=0)
    compare.add_argument("--work", type=Path, required=True)
    compare.add_argument("--output", type=Path, required=True)
    compare.add_argument("--python", type=Path, default=Path(sys.executable))
    compare.add_argument("--runs", type=int, default=3)
    compare.add_argument("--ascii-bytes", type=int, default=ASCII_PAYLOAD_BYTES)
    compare.add_argument("--random-bytes", type=int, default=RANDOM_PAYLOAD_BYTES)
    compare.add_argument("--font", default=FONT)
    compare.add_argument("--font-size", type=int, default=FONT_SIZE)
    compare.add_argument("--scrollback-lines", type=int, default=SCROLLBACK_LINES)
    compare.add_argument("--shitty-column-delta", type=int, default=0)
    compare.add_argument("--shitty-row-delta", type=int, default=0)
    compare.add_argument("--timeout", type=float, default=300.0)
    compare.add_argument("--windows-terminal-version", default="unknown")
    compare.add_argument("--shitty-sha256", default="unknown")
    compare.add_argument("--os-build", default="unknown")
    return result


def write(arguments):
    return Writer(arguments.payload, arguments.expected_size, arguments.result).run()


def compare(arguments):
    workloads = Payloads(
        arguments.work,
        arguments.ascii_bytes,
        arguments.random_bytes,
    ).ensure()
    return Comparison(
        ShittyTerminal(
            arguments.shitty,
            arguments.config,
            arguments.font,
            arguments.font_size,
            arguments.scrollback_lines,
            arguments.shitty_column_delta,
            arguments.shitty_row_delta,
        ),
        WindowsTerminal(
            arguments.windows_terminal,
            arguments.windows_terminal_profile,
            arguments.windows_terminal_column_delta,
        ),
        workloads,
        arguments.python,
        Path(__file__).resolve(),
        arguments.output,
        arguments.runs,
        arguments.timeout,
        arguments.windows_terminal_version,
        arguments.shitty_sha256,
        arguments.os_build,
    ).run()


def main():
    arguments = parser().parse_args()
    return {"compare": compare, "write": write}[arguments.mode](arguments)


if __name__ == "__main__":
    raise SystemExit(main())
