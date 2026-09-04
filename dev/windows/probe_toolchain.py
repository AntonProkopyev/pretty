#!/usr/bin/env python3

"""Compile and inspect a minimal Windows x64 executable."""

import argparse
import dataclasses
import re
import struct
import subprocess
import sys
from pathlib import Path


@dataclasses.dataclass(frozen=True)
class PeContract:
    machine: int
    magic: int
    subsystem: int


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runner", default="")
    return parser.parse_args()


def tool(toolchain, name):
    plain = toolchain / "bin" / name
    executable = plain.with_suffix(plain.suffix + ".exe")
    return plain if plain.exists() else executable


def compile_command(toolchain, source, output):
    return [
        str(tool(toolchain, "x86_64-w64-mingw32-clang++")),
        "-std=c++26",
        "-fuse-ld=lld",
        "-static",
        "-Wl,--subsystem,console",
        str(source),
        "-o",
        str(output),
    ]


def read_contract(image):
    content = image.read_bytes()
    if len(content) < 0x40 or content[0:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe = struct.unpack_from("<I", content, 0x3C)[0]
    if pe + 24 + 70 > len(content) or content[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE header")
    machine = struct.unpack_from("<H", content, pe + 4)[0]
    magic = struct.unpack_from("<H", content, pe + 24)[0]
    subsystem = struct.unpack_from("<H", content, pe + 24 + 68)[0]
    return PeContract(machine, magic, subsystem)


def validate(contract):
    expected = PeContract(machine=0x8664, magic=0x20B, subsystem=3)
    if contract != expected:
        raise ValueError(f"unexpected PE contract: {contract}")
    return contract


def imports(toolchain, image):
    result = subprocess.run(
        [str(tool(toolchain, "llvm-objdump")), "-p", str(image)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return tuple(re.findall(r"DLL Name: ([^\r\n]+)", result.stdout))


def validate_imports(names):
    allowed = {
        "advapi32.dll",
        "kernel32.dll",
        "ntdll.dll",
        "ucrtbase.dll",
        "user32.dll",
    }
    unexpected = [
        name
        for name in names
        if name.lower() not in allowed
        and not name.lower().startswith("api-ms-win-crt-")
    ]
    if unexpected:
        raise ValueError(f"unexpected DLL imports: {', '.join(unexpected)}")
    return names


def probe(toolchain, output, runner):
    source = Path(__file__).with_name("probe.cpp")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(compile_command(toolchain, source, output), check=True)
    contract = validate(read_contract(output))
    names = validate_imports(imports(toolchain, output))
    if runner:
        subprocess.run([runner, str(output)], check=True, timeout=30)
    print(
        f"PE32+ AMD64 subsystem={contract.subsystem} "
        f"imports={','.join(names) or '(none)'}"
    )
    return output


def main():
    options = arguments()
    try:
        probe(options.toolchain, options.output, options.runner)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"toolchain probe: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
