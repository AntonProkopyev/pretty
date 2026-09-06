# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from urllib.parse import quote


@unittest.skipUnless(shutil.which("zsh"), "zsh is not installed")
class ShellDirectoryTests(unittest.TestCase):
    def test_directory_is_encoded_and_hook_is_scoped(self):
        hook = Path(__file__).resolve().parents[1] / "dev/windows/shell-integration.zsh"
        environment = dict(os.environ)
        environment.pop("SHITTY_VERSION", None)
        environment.pop("PRETTY_VERSION", None)
        inactive = subprocess.run(
            ["zsh", "-dfc", 'source "$1"; (( ! $+functions[_shitty_directory] ))', "test", str(hook)],
            env=environment, capture_output=True, check=True,
        )
        self.assertEqual(inactive.stdout, b"")
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "space Ω % ; $()"
            directory.mkdir()
            environment["SHITTY_VERSION"] = "test"
            active = subprocess.run(
                ["zsh", "-dfc", 'source "$1"; _shitty_directory', "test", str(hook)],
                cwd=directory, env=environment, capture_output=True, check=True,
            )
            expected = b"\x1b]7;file://localhost" + quote(str(directory), safe="/").encode() + b"\x1b\\"
            self.assertEqual(active.stdout, expected)


if __name__ == "__main__":
    unittest.main()
