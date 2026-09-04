#!/usr/bin/env python3

"""Fetch and verify the pinned llvm-mingw toolchain."""

import argparse
import hashlib
import shutil
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
import zipfile
from pathlib import Path


MARKER = ".llvm-mingw.sha256"


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            result.update(chunk)
    return result.hexdigest()


def download(url, destination):
    with urllib.request.urlopen(url, timeout=60) as source:
        with destination.open("wb") as target:
            shutil.copyfileobj(source, target)
    return destination


def safe_zip_members(archive, destination):
    root = destination.resolve()
    for member in archive.infolist():
        target = (destination / member.filename).resolve()
        if not target.is_relative_to(root):
            raise ValueError(f"archive path escapes destination: {member.filename}")
    return archive.infolist()


def extract(archive_path, destination):
    if zipfile.is_zipfile(archive_path):
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(destination, members=safe_zip_members(archive, destination))
    else:
        with tarfile.open(archive_path, "r:xz") as archive:
            archive.extractall(destination, filter="data")
    entries = list(destination.iterdir())
    if len(entries) != 1 or not entries[0].is_dir():
        raise ValueError("toolchain archive must contain one top-level directory")
    return entries[0]


def installed(output, expected):
    try:
        return (output / MARKER).read_text().strip() == expected
    except OSError:
        return False


def replace_owned(source, output, expected):
    if output.exists():
        marker = output / MARKER
        if not marker.is_file():
            raise ValueError(f"refusing to replace unowned output: {output}")
        shutil.rmtree(output)
    (source / MARKER).write_text(expected + "\n")
    shutil.move(source, output)
    return output


def fetch(lock_path, platform, output):
    lock = tomllib.loads(lock_path.read_text())
    if lock.get("version") != 1:
        raise ValueError("unsupported toolchain lock version")
    try:
        asset = lock["assets"][platform]
        url = asset["url"]
        expected = asset["sha256"]
    except KeyError as error:
        raise ValueError(f"unknown toolchain platform: {platform}") from error
    if installed(output, expected):
        return output
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="llvm-mingw-", dir=output.parent) as directory:
        temporary = Path(directory)
        archive = download(url, temporary / "toolchain.archive")
        actual = digest(archive)
        if actual != expected:
            raise ValueError(f"checksum mismatch: expected {expected}, got {actual}")
        extracted = extract(archive, temporary / "unpacked")
        return replace_owned(extracted, output, expected)


def main():
    options = arguments()
    try:
        fetch(options.lock, options.platform, options.output)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"toolchain: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
