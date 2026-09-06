# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import hashlib
import json
import os
import subprocess
import time
import zipfile
from pathlib import Path

from audit_pe import recursive_closure


VCPKG_LICENSES = (
    "brotli",
    "bzip2",
    "freetype",
    "harfbuzz",
    "libpng",
    "simdutf",
    "vulkan-headers",
    "vulkan-loader",
    "zlib",
)
FONT_LICENSES = (
    "LICENSE.Amiri",
    "LICENSE.NotoColorEmoji",
    "LICENSE.cozette",
    "LICENSE.spleen",
    "OFL.txt",
    "SOURCE.md",
)
ARCHIVE_ROOT = "shitty-windows-x86_64"


def content(path):
    if not path.is_file():
        raise RuntimeError(f"required package input is missing: {path}")
    return path.read_bytes()


def timestamp(value):
    instant = time.gmtime(value)
    return (max(1980, instant.tm_year), instant.tm_mon, instant.tm_mday,
            instant.tm_hour, instant.tm_min, instant.tm_sec)


def zip_info(name, date_time):
    info = zipfile.ZipInfo(f"{ARCHIVE_ROOT}/{name}", date_time=date_time)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def verify_entry(objdump, readobj, image):
    metadata = subprocess.run(
        [objdump, "-p", image],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout
    resources = subprocess.run(
        [readobj, "--coff-resources", image],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout
    required = ("Type: RT_MANIFEST", "Type: ICON", "Type: GROUP_ICON")
    missing = [item for item in required if item not in resources]
    gui = "Subsystem               00000002" in metadata
    if missing or not gui:
        raise RuntimeError(
            f"invalid release PE resources for {image.name}: "
            f"gui={gui} missing={missing}"
        )
    return {"gui_subsystem": gui, "resources": list(required)}


def package(arguments):
    closure = recursive_closure(
        arguments.objdump,
        arguments.runtime_dir,
        ["st.exe", "pt.exe", "conpty.dll", "x64/OpenConsole.exe"],
    )
    if closure["missing"] or closure["forbidden"]:
        raise RuntimeError(
            f"invalid recursive DLL closure: missing={closure['missing']} "
            f"forbidden={closure['forbidden']}"
        )
    if not all(image["coff_x86_64"] for image in closure["images"]):
        raise RuntimeError("Windows package contains a non-AMD64 PE image")
    closure["entry_resources"] = {
        entry: verify_entry(
            arguments.objdump,
            arguments.readobj,
            arguments.runtime_dir / entry,
        )
        for entry in ("st.exe", "pt.exe")
    }

    files = {
        image["path"]: content(arguments.runtime_dir / image["path"])
        for image in closure["images"]
    }
    files.update({
        "config/shitty.toml": content(arguments.project_root / "bin/st/shitty.toml"),
        "config/pretty.toml": content(arguments.project_root / "bin/pt/pretty.toml"),
        "install.ps1": content(arguments.project_root / "dev/windows/install.ps1"),
        "shell-integration.zsh": content(arguments.project_root / "dev/windows/shell-integration.zsh"),
        "licenses/LICENSE": content(arguments.project_root / "LICENSE"),
        "licenses/LICENSE.MIT": content(arguments.project_root / "LICENSE.MIT"),
        "licenses/LICENSE.GPL3": content(arguments.project_root / "LICENSE.GPL3"),
        "licenses/llvm-mingw.txt": content(arguments.llvm_license),
        "licenses/gcc-mingw.txt": content(arguments.gcc_copyright),
        "licenses/Microsoft.Windows.Console.ConPTY.LICENSE.txt": content(
            arguments.project_root
            / "dev/windows/Microsoft.Windows.Console.ConPTY.LICENSE.txt"
        ),
        "README.md": content(arguments.project_root / "README.md"),
        "pe-audit.json": (json.dumps(closure, indent=2, sort_keys=True) + "\n").encode(),
    })
    for name in FONT_LICENSES:
        files[f"licenses/fonts/{name}"] = content(
            arguments.project_root / "ext/fonts" / name
        )
    for name in VCPKG_LICENSES:
        files[f"licenses/vcpkg/{name}.txt"] = content(
            arguments.vcpkg_share / name / "copyright"
        )

    hashes = "".join(
        f"{hashlib.sha256(files[name]).hexdigest()}  {name}\n"
        for name in sorted(files, key=str.lower)
    )
    files["SHA256SUMS"] = hashes.encode()
    date_time = timestamp(arguments.timestamp)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        arguments.output,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for name in sorted(files, key=str.lower):
            archive.writestr(zip_info(name, date_time), files[name])
    return {
        "output": os.fspath(arguments.output),
        "sha256": hashlib.sha256(arguments.output.read_bytes()).hexdigest(),
        "files": len(files),
        "runtime_images": len(closure["images"]),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--vcpkg-share", type=Path, required=True)
    parser.add_argument("--llvm-license", type=Path, required=True)
    parser.add_argument("--gcc-copyright", type=Path, required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--readobj", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--timestamp",
        type=int,
        default=int(os.environ.get("SOURCE_DATE_EPOCH", "315532800")),
        help="SOURCE_DATE_EPOCH-compatible ZIP timestamp",
    )
    arguments = parser.parse_args()
    print(json.dumps(package(arguments), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
