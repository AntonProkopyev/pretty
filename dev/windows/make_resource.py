# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import argparse
import shutil
from pathlib import Path

from png_to_ico import convert


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--png", type=Path, required=True)
    parser.add_argument("--icon-output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    arguments = parser.parse_args()
    convert(arguments.png, arguments.icon_output)
    shutil.copyfile(arguments.manifest, arguments.manifest_output)
    arguments.output.write_text(
        f'1 ICON "{arguments.icon_output.name}"\n'
        f'1 RT_MANIFEST "{arguments.manifest_output.name}"\n'
    )


if __name__ == "__main__":
    main()
