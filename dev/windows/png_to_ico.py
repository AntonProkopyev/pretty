# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import struct
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def convert(source, destination):
    image = source.read_bytes()
    if not image.startswith(PNG_SIGNATURE) or image[12:16] != b"IHDR":
        raise ValueError(f"not a PNG image: {source}")
    width, height = struct.unpack(">II", image[16:24])
    if not 1 <= width <= 256 or not 1 <= height <= 256:
        raise ValueError(f"ICO PNG dimensions must be 1..256: {width}x{height}")
    directory = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack(
        "<BBBBHHII",
        0 if width == 256 else width,
        0 if height == 256 else height,
        0,
        0,
        1,
        32,
        len(image),
        22,
    )
    destination.write_bytes(directory + entry + image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    arguments = parser.parse_args()
    convert(arguments.source, arguments.destination)


if __name__ == "__main__":
    main()
