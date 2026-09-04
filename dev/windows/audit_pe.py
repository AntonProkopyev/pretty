# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import json
import re
import subprocess
from pathlib import Path


FORBIDDEN = {"msys-2.0.dll", "cygwin1.dll"}
SYSTEM = {
    "advapi32.dll",
    "cfgmgr32.dll",
    "comdlg32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "d3dcompiler_47.dll",
    "dwrite.dll",
    "dxgi.dll",
    "gdi32.dll",
    "imm32.dll",
    "kernel32.dll",
    "msvcrt.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "propsys.dll",
    "shell32.dll",
    "shlwapi.dll",
    "user32.dll",
    "ucrtbase.dll",
    "version.dll",
    "winmm.dll",
    "ws2_32.dll",
    "usp10.dll",
}


def imports(objdump, image):
    result = subprocess.run(
        [objdump, "-p", image],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"objdump failed for {image}")
    names = re.findall(r"DLL Name:\s*([^\r\n]+)", result.stdout)
    return result.stdout, [name.strip() for name in names]


def system(name):
    lowered = name.lower()
    return lowered in SYSTEM or lowered.startswith(("api-ms-win-", "ext-ms-win-"))


def recursive_closure(objdump, root, entries):
    # A case-folded index matches the Windows loader without assuming the
    # package was assembled on a case-insensitive filesystem.
    packaged = {
        path.name.lower(): path
        for path in root.rglob("*")
        if path.is_file()
    }
    pending = [root / entry for entry in entries]
    visited = {}
    missing = set()
    forbidden = set()
    while pending:
        image = pending.pop()
        key = image.relative_to(root).as_posix().lower()
        if key in visited:
            continue
        if not image.is_file():
            missing.add(image.name)
            continue
        metadata, names = imports(objdump, image)
        visited[key] = {
            "path": image.relative_to(root).as_posix(),
            "coff_x86_64": "file format coff-x86-64" in metadata,
            "imports": names,
        }
        for name in names:
            lowered = name.lower()
            if lowered in FORBIDDEN:
                forbidden.add(lowered)
            if system(name):
                continue
            dependency = packaged.get(lowered)
            if dependency is None:
                missing.add(name)
            else:
                pending.append(dependency)
    return {
        "recursive": True,
        "entries": entries,
        "images": list(visited.values()),
        "missing": sorted(missing, key=str.lower),
        "forbidden": sorted(forbidden),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("entries", nargs="+")
    arguments = parser.parse_args()

    receipt = recursive_closure(
        arguments.objdump,
        arguments.root,
        arguments.entries,
    )
    print(json.dumps(receipt, indent=2, sort_keys=True))
    machines = all(image["coff_x86_64"] for image in receipt["images"])
    return 0 if machines and not receipt["missing"] and not receipt["forbidden"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
