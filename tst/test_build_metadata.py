# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import concurrent.futures
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


class BuildMetadataTests(unittest.TestCase):
    def run_build(self, build_file, *arguments):
        return subprocess.run(
            [
                sys.executable,
                ROOT / "build",
                "--build-file",
                build_file,
                *arguments,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def run_build_with_descr(self, descr):
        with tempfile.TemporaryDirectory() as directory:
            build_file = Path(directory) / "build.py"
            build_file.write_text(
                "target = command(\n"
                "    outputs=['$(B)/result'],\n"
                "    cmd=['true'],\n"
                f"    descr={descr!r},\n"
                ")\n"
                "install(target)\n"
            )
            return self.run_build(build_file, "--list")

    def test_build_accepts_exactly_two_ascii_letters_in_descr(self):
        result = self.run_build_with_descr("OK")

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_build_rejects_every_other_descr_shape(self):
        for descr in ("A", "ABC", "A1", "A ", "ÄB"):
            with self.subTest(descr=descr):
                result = self.run_build_with_descr(descr)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "descr must be exactly two ASCII letters",
                    result.stderr,
                )

    def test_groups_are_additive_cli_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_file = root / "build.py"
            build_file.write_text(
                "one = command(\n"
                "    outputs=['$(B)/one'],\n"
                "    cmd=['python3', '-c', "
                "\"from pathlib import Path; Path(r'$(B)/one').touch()\"],\n"
                ")\n"
                "two = command(\n"
                "    outputs=['$(B)/two'],\n"
                "    cmd=['python3', '-c', "
                "\"from pathlib import Path; Path(r'$(B)/two').touch()\"],\n"
                ")\n"
                "group('batch', one)\n"
                "group('batch', two)\n"
                "group('install', one)\n"
            )

            listed = self.run_build(build_file, "--list")
            self.assertEqual(listed.returncode, 0, listed.stderr)
            self.assertEqual(
                listed.stdout.splitlines(),
                ["batch", "install", "one", "two"],
            )

            result = self.run_build(build_file, "-B", ".out", "batch")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / ".out" / "one").exists())
            self.assertTrue((root / ".out" / "two").exists())
            self.assertFalse((root / "one").exists())
            self.assertFalse((root / "two").exists())

    def test_install_group_is_the_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_file = root / "build.py"
            build_file.write_text(
                "target = command(\n"
                "    outputs=['$(B)/result'],\n"
                "    cmd=['python3', '-c', "
                "\"from pathlib import Path; Path(r'$(B)/result').touch()\"],\n"
                ")\n"
                "group('install', target)\n"
            )

            result = self.run_build(build_file, "-B", ".out")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / ".out" / "result").exists())

    def test_group_name_cannot_conflict_with_target_name(self):
        with tempfile.TemporaryDirectory() as directory:
            build_file = Path(directory) / "build.py"
            build_file.write_text(
                "same = command(\n"
                "    outputs=['$(B)/result'],\n"
                "    cmd=['/usr/bin/touch', '$(B)/result'],\n"
                ")\n"
                "group('same', same)\n"
            )

            result = self.run_build(build_file, "--list")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "group name conflicts with target name: same",
                result.stderr,
            )

    def test_libstd_is_bundled_as_source(self):
        self.assertFalse((ROOT / ".gitmodules").exists())
        self.assertTrue((ROOT / "ext/libstd/build.py").is_file())
        self.assertTrue((ROOT / "ext/libstd/std/lib/buffer.cpp").is_file())

    def test_readme_builds_without_submodule_setup(self):
        readme = (ROOT / "README.md").read_text()
        self.assertNotIn("git submodule", readme)
        self.assertIn("ext/libstd", readme)

    def test_graph_python_commands_use_current_interpreter(self):
        result = self.run_build(ROOT / "build.py", "--graph")
        self.assertEqual(result.returncode, 0, result.stderr)
        graph = json.loads(result.stdout)
        commands = [
            command
            for node in graph["nodes"]
            for command in node.get("cmd", [])
            if command
        ]

        hardcoded = [command for command in commands if command[0] == "python3"]
        self.assertEqual(len(hardcoded), 0)
        self.assertTrue(any(command[0] == sys.executable for command in commands))

    def test_windows_native_target_uses_mingw_triple(self):
        loader = SourceFileLoader("shitty_build_windows_target", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with mock.patch.object(runner.os, "name", "nt"), mock.patch.object(
            runner.sysconfig,
            "get_config_var",
            return_value=None,
        ), mock.patch.object(runner.platform, "machine", return_value="AMD64"):
            self.assertEqual(runner.native_target(), "x86_64-w64-windows-gnu")

    def test_windows_programs_have_exe_suffix(self):
        loader = SourceFileLoader("shitty_build_windows_program", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.c").write_text("int main(void) { return 0; }\n")
            context = runner.BuildContext(
                root,
                root / ".out",
                target="x86_64-w64-windows-gnu",
            )
            target = context.program(
                name="demo",
                output="$(B)/demo",
                srcs=["$(S)/main.c"],
            )
            with mock.patch.object(
                context,
                "_compiler_command",
                return_value=["target-cc"],
            ):
                context._emit_target(target, set())

            self.assertEqual(target.root.outputs, ["$(B)/demo.exe"])

    def test_windows_resources_use_llvm_rc(self):
        loader = SourceFileLoader("shitty_build_windows_resource", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.c").write_text("int main(void) { return 0; }\n")
            (root / "app.rc").write_text("1 VERSIONINFO\n")
            context = runner.BuildContext(
                root,
                root / ".out",
                target="x86_64-w64-windows-gnu",
            )
            context.rc = "llvm-rc"
            target = context.program(
                name="demo",
                srcs=["$(S)/main.c", "$(S)/app.rc"],
            )
            with mock.patch.object(
                context,
                "_compiler_command",
                return_value=["target-cc"],
            ):
                context._emit_target(target, set())

            resource = next(
                node for node in target.nodes
                if node.inputs == ["$(S)/app.rc"]
            )
            self.assertEqual(resource.commands[0][0:2], ["llvm-rc", "/fo"])
            self.assertTrue(resource.outputs[0].endswith(".res"))
            self.assertEqual(resource.commands[0][-1], "$(S)/app.rc")

    def test_windows_supervisor_runs_main_without_posix_process_group(self):
        loader = SourceFileLoader("shitty_build_windows_supervisor", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with mock.patch.object(runner.os, "name", "nt"), mock.patch.object(
            runner,
            "main",
            return_value=7,
        ) as main, mock.patch.object(
            runner.signal,
            "pthread_sigmask",
            side_effect=AssertionError("POSIX supervisor used on Windows"),
            create=True,
        ):
            self.assertEqual(runner.supervised_main(["--list"]), 7)

        main.assert_called_once_with(["--list"])

    def test_windows_build_runner_uses_native_file_lock(self):
        loader = SourceFileLoader("shitty_build_windows_lock", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        msvcrt = mock.Mock(LK_LOCK=1, LK_UNLCK=2)

        with mock.patch.object(os, "name", "nt"), mock.patch.dict(
            sys.modules,
            {"fcntl": None, "msvcrt": msvcrt},
        ):
            loader.exec_module(runner)

        with tempfile.TemporaryFile() as lock_file:
            with runner.FileLock(lock_file.fileno()):
                pass

            self.assertEqual(
                msvcrt.locking.call_args_list,
                [
                    mock.call(lock_file.fileno(), msvcrt.LK_LOCK, 1),
                    mock.call(lock_file.fileno(), msvcrt.LK_UNLCK, 1),
                ],
            )

    def test_cache_store_copies_when_hard_links_are_unavailable(self):
        loader = SourceFileLoader("shitty_build_cache_store_copy", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = runner.BuildContext(root, root / ".out")
            executor = runner.Executor(context, 1, False, False)
            output = root / "output"
            output.write_text("cached")
            manifest = {}

            with mock.patch.object(
                runner.os,
                "link",
                side_effect=PermissionError("hard links unavailable"),
            ):
                executor._store_path(output, "$(B)/output", manifest)

            cached = executor._cas_path(manifest["$(B)/output"]["cas"])
            self.assertEqual(cached.read_text(), "cached")

    def test_cache_restore_copies_when_links_are_unavailable(self):
        loader = SourceFileLoader("shitty_build_cache_restore_copy", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = runner.BuildContext(root, root / ".out")
            executor = runner.Executor(context, 1, False, False)
            source = root / "source"
            source.write_text("restored")
            stored = {}
            executor._store_path(source, "$(B)/output", stored)
            uid = "a" * 64

            for symlink in (False, True):
                with self.subTest(symlink=symlink):
                    manifest = executor._manifest_path(uid)
                    manifest.parent.mkdir(parents=True, exist_ok=True)
                    manifest.write_text(json.dumps(stored))
                    destination = root / ("link" if symlink else "file")
                    with mock.patch.object(
                        runner.os,
                        "link",
                        side_effect=PermissionError("hard links unavailable"),
                    ), mock.patch.object(
                        runner.os,
                        "symlink",
                        side_effect=PermissionError("symbolic links unavailable"),
                    ):
                        executor._restore(uid, destination, symlink)

                    self.assertEqual((destination / "output").read_text(), "restored")

    def test_publish_refreshes_managed_copy_without_symlink_privilege(self):
        loader = SourceFileLoader("shitty_build_publish_copy", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = runner.BuildContext(root, root / ".out")
            output = context.build_root / "demo"
            output.parent.mkdir(parents=True)
            output.write_text("first")
            target = runner.Target("program", name="demo")
            target.root = runner.Node([], ["$(B)/demo"], [])

            with mock.patch.object(
                runner.os,
                "link",
                side_effect=PermissionError("hard links unavailable"),
            ), mock.patch.object(
                runner.os,
                "symlink",
                side_effect=PermissionError("symbolic links unavailable"),
            ):
                context.publish([target])
                self.assertEqual((root / "demo").read_text(), "first")
                output.write_text("second")
                context.publish([target])
                self.assertEqual((root / "demo").read_text(), "second")

                (root / "demo").write_text("user-owned")
                output.write_text("third")
                context.publish([target])

            self.assertEqual((root / "demo").read_text(), "user-owned")

    def test_header_probe_uses_target_compiler_and_current_flags(self):
        loader = SourceFileLoader("shitty_build_header_probe", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = runner.BuildContext(
                root,
                root / ".out",
                target="aarch64-unknown-linux-gnu",
            )
            context.cflags = ["-target-c"]
            context.cxxflags = ["-target-cxx"]
            context.cppflags = ["-target-cpp"]
            compiler = ["target-c++", "--target=aarch64-unknown-linux-gnu"]
            completed = subprocess.CompletedProcess(compiler, 0)
            with mock.patch.object(
                context,
                "_compiler_command",
                return_value=compiler,
            ), mock.patch.object(
                runner.subprocess,
                "run",
                return_value=completed,
            ) as run:
                self.assertTrue(context.have_header("optional/header.h"))

            command = run.call_args.args[0]
            self.assertEqual(command[:2], compiler)
            self.assertIn("-target-c", command)
            self.assertIn("-target-cxx", command)
            self.assertIn("-target-cpp", command)
            self.assertEqual(
                run.call_args.kwargs["input"],
                "#include <optional/header.h>\n",
            )

    def test_cxx_standard_probe_uses_target_compiler(self):
        loader = SourceFileLoader("shitty_build_cxx_standard_probe", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / (
                "target-clang++.exe" if os.name == "nt" else "target-clang++"
            )
            compiler.touch(mode=0o755)
            context = runner.BuildContext(
                ROOT,
                root / ".out",
                target="x86_64-w64-windows-gnu",
            )
            failed = subprocess.CompletedProcess([], 1)
            with mock.patch.dict(
                os.environ,
                {"CXX": str(compiler)},
            ), mock.patch.object(
                runner.subprocess,
                "run",
                return_value=failed,
            ) as run:
                with self.assertRaises(RuntimeError):
                    context.load(ROOT / "build.py")

            command = run.call_args.args[0]
            self.assertEqual(command[0], str(compiler))
            self.assertIn("--target=x86_64-w64-windows-gnu", command)

    def test_headless_plt_accepts_windows_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "clang"
            compiler.touch(mode=0o755)
            environment = os.environ.copy()
            environment.update({
                "CC": str(compiler),
                "CXX": str(compiler),
                "CPPFLAGS": "-Dplatforms=headless",
            })
            result = subprocess.run(
                [
                    sys.executable,
                    ROOT / "build",
                    "--build-file", ROOT / "ext" / "plt" / "build.py",
                    "--target", "x86_64-w64-windows-gnu",
                    "--graph",
                ],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            graph = json.loads(result.stdout)
            sources = {
                source
                for node in graph["nodes"]
                for source in node["inputs"]
            }
            self.assertIn("$(S)/platform_headless.cpp", sources)
            self.assertNotIn("$(S)/platform_wayland.cpp", sources)
            self.assertNotIn("$(S)/platform_cocoa.mm", sources)
            platform = next(
                node for node in graph["nodes"]
                if "$(S)/platform.cpp" in node["inputs"]
            )
            self.assertIn("-DPLT_HEADLESS=1", platform["cmd"][0])

    def test_plt_selects_win32_backend_for_windows_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "clang"
            compiler.touch(mode=0o755)
            environment = os.environ.copy()
            environment.update({
                "CC": str(compiler),
                "CXX": str(compiler),
            })
            result = subprocess.run(
                [
                    sys.executable,
                    ROOT / "build",
                    "--build-file", ROOT / "ext" / "plt" / "build.py",
                    "--target", "x86_64-w64-windows-gnu",
                    "--graph",
                ],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            graph = json.loads(result.stdout)
            sources = {
                source
                for node in graph["nodes"]
                for source in node["inputs"]
            }
            self.assertTrue("$(S)/platform_win32.cpp" in sources)
            self.assertTrue("$(S)/platform_wayland.cpp" not in sources)
            self.assertTrue("$(S)/platform_cocoa.mm" not in sources)
            self.assertTrue("$(S)/platform_headless.cpp" in sources)

    def test_windows_libstd_graph_excludes_posix_and_test_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "clang"
            compiler.touch(mode=0o755)
            environment = os.environ.copy()
            environment.update({
                "CC": str(compiler),
                "CXX": str(compiler),
            })
            result = subprocess.run(
                [
                    sys.executable,
                    ROOT / "build",
                    "--build-file", ROOT / "ext" / "libstd" / "build.py",
                    "--target", "x86_64-w64-windows-gnu",
                    "--graph",
                ],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            graph = json.loads(result.stdout)
            sources = {
                source
                for node in graph["nodes"]
                for source in node["inputs"]
                if source.endswith((".cpp", ".c"))
            }
            forbidden = (
                "$(S)/std/dns/",
                "$(S)/std/net/",
                "$(S)/std/ios/stream_tcp",
                "$(S)/std/sys/event_fd.cpp",
                "$(S)/std/sys/mem_fd.cpp",
                "$(S)/std/thr/io_classic.cpp",
                "$(S)/std/thr/io_uring.cpp",
                "$(S)/std/thr/poll_fd.cpp",
                "$(S)/std/thr/reactor_poll.cpp",
                "$(S)/tst/",
            )

            self.assertTrue("$(S)/std/ios/output.cpp" in sources)
            self.assertTrue("$(S)/std/ios/in_fd.cpp" in sources)
            self.assertTrue("$(S)/std/ios/out_fd.cpp" in sources)
            self.assertTrue(all(not source.endswith("_ut.cpp") for source in sources))
            for prefix in forbidden:
                with self.subTest(prefix=prefix):
                    self.assertTrue(all(not source.startswith(prefix) for source in sources))

    def test_tool_resolution_preserves_the_path_selected_argv_zero(self):
        loader = SourceFileLoader("shitty_build_tool_path", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            implementation = root / "multicall"
            implementation.touch()
            alias = root / "sh"
            alias.symlink_to(implementation.name)
            context = runner.BuildContext(root, root / ".out")
            with mock.patch.object(
                runner.shutil,
                "which",
                return_value=str(alias),
            ):
                resolved = context.resolve_tool("sh")

            self.assertEqual(resolved, str(alias))
            self.assertNotEqual(resolved, str(implementation))

    def test_imported_program_gets_injected_dependency_link_flags(self):
        loader = SourceFileLoader("shitty_build_import_ldflags", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dependency = root / "dependency"
            application = root / "application"
            dependency.mkdir()
            application.mkdir()
            (dependency / "support.c").write_text("int support(void) { return 0; }\n")
            (dependency / "build.py").write_text(
                "support = library(srcs=['$(S)/support.c'])\n"
            )
            (application / "main.c").write_text("int main(void) { return 0; }\n")
            (application / "build.py").write_text(
                "app = program(srcs=['$(S)/main.c'])\n"
            )
            build_file = root / "build.py"
            build_file.write_text(
                "support = import_build('dependency/build.py', 'libsupport.a')\n"
                "support.ldflags += ['-lsupport-runtime']\n"
                "app = import_build(\n"
                "    'application/build.py', 'app', deps=[support],\n"
                ")\n"
            )

            context = runner.BuildContext(root, root / ".out")
            context.load(build_file)
            context.build_graph()

            support = context.target_names["support"]
            app = context.target_names["app"]
            command = app.root.commands[-1]
            archive = command.index(support.output)
            self.assertEqual(command[archive + 1], "-lsupport-runtime")

    def test_strace_parser_counts_stat_and_file_backed_mmap(self):
        loader = SourceFileLoader("shitty_build_strace", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stat_input = root / "from-stat"
            mmap_input = root / "from-mmap"
            declared = root / "declared"
            generated = root / "generated"
            build_root = root / ".out"
            build_output = build_root / "generated"
            for path in (stat_input, mmap_input, declared, generated, build_output):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            payload = {
                "source_root": str(root),
                "build_root": str(build_root),
                "cwd": str(root),
                "inputs": [str(declared)],
            }
            trace = (
                f'stat("{stat_input}", {{st_mode=S_IFREG|0644}}) = 0\n'
                f'mmap(NULL, 1, PROT_READ, MAP_PRIVATE, '
                f'3<{mmap_input}>, 0) = 0x1234\n'
                f'open("{declared}", O_RDONLY) = 4<{declared}>\n'
                f'42 open("{generated}", O_RDWR|O_CREAT|O_EXCL, 0600 '
                f'<unfinished ...>\n'
                f'42 <... open resumed>) = 5<{generated}>\n'
                f'stat("{generated}", {{st_mode=S_IFREG|0600}}) = 0\n'
                f'mmap(NULL, 1, PROT_READ, MAP_PRIVATE, '
                f'5<{generated}>, 0) = 0x5678\n'
                f'newfstatat(7<{root}/vanished (deleted)>, "from-dirfd", '
                f'{{st_mode=S_IFREG|0644}}, 0) = 0\n'
                f'newfstatat(AT_FDCWD<{build_root}>, "generated", '
                f'{{st_mode=S_IFREG|0644}}, 0) = 0\n'
            )

            self.assertEqual(
                runner._strace_missing_inputs(payload, trace),
                [
                    "$(S)/from-mmap",
                    "$(S)/from-stat",
                    "$(S)/vanished/from-dirfd",
                ],
            )

    @unittest.skipUnless(
        sys.platform.startswith("linux") and shutil.which("strace"),
        "strace is only available on Linux",
    )
    @unittest.skipIf(
        os.environ.get("BUILD_STRACE_ACTIVE"),
        "ptrace cannot be nested inside a strace audit",
    )
    def test_strace_rejects_undeclared_read_on_a_cached_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            hidden = root / "hidden.txt"
            hidden.write_text("hidden")
            build_file = root / "build.py"
            build_file.write_text(
                "target = command(\n"
                "    outputs=['$(B)/result'],\n"
                "    cmd=['python3', '-c', \"import os; from pathlib import Path; "
                "os.stat(r'$(S)/hidden.txt'); Path(r'$(B)/result').touch()\"],\n"
                ")\n"
                "install(target)\n"
            )

            cached = self.run_build(build_file, "-B", ".out", "install")
            self.assertEqual(cached.returncode, 0, cached.stderr)

            traced = self.run_build(
                build_file,
                "-B", ".out",
                "--strace",
                "install",
            )
            self.assertNotEqual(traced.returncode, 0)
            self.assertIn("undeclared source input(s)", traced.stderr)
            self.assertIn("$(S)/hidden.txt", traced.stderr)

    @unittest.skipUnless(
        sys.platform.startswith("linux") and shutil.which("strace"),
        "strace is only available on Linux",
    )
    @unittest.skipIf(
        os.environ.get("BUILD_STRACE_ACTIVE"),
        "ptrace cannot be nested inside a strace audit",
    )
    def test_strace_accepts_inputs_from_the_dependency_closure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "dependency.txt").write_text("dependency")
            (root / "direct.txt").write_text("direct")
            build_file = root / "build.py"
            build_file.write_text(
                "dependency = command(\n"
                "    inputs=['$(S)/dependency.txt'],\n"
                "    outputs=['$(B)/dependency'],\n"
                "    cmd=['python3', '-c', \"from pathlib import Path; "
                "Path(r'$(S)/dependency.txt').read_text(); "
                "Path(r'$(B)/dependency').touch()\"],\n"
                ")\n"
                "target = command(\n"
                "    inputs=['$(S)/direct.txt'],\n"
                "    deps=[dependency],\n"
                "    outputs=['$(B)/result'],\n"
                "    cmd=['python3', '-c', \"from pathlib import Path; "
                "Path(r'$(S)/direct.txt').read_text(); "
                "Path(r'$(S)/dependency.txt').read_text(); "
                "Path(r'$(B)/result').touch()\"],\n"
                ")\n"
                "install(target)\n"
            )

            result = self.run_build(
                build_file,
                "-B", ".out",
                "--strace",
                "install",
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_parallel_execution_with_the_same_uid_keeps_a_stable_lock(self):
        loader = SourceFileLoader("shitty_build_parallel_uid", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = runner.BuildContext(root, root / ".out")
            executor = runner.Executor(
                context,
                jobs=2,
                verbose=False,
                keep_going=False,
                strace=True,
            )
            node = runner.Node(inputs=[], outputs=[], commands=[])
            node.uid = "same-uid"
            discarded = threading.Event()
            acquisition_lock = threading.Lock()
            acquisitions = 0
            real_flock = runner.fcntl.flock
            real_discard = executor._discard_contents

            def flock(fd, operation):
                nonlocal acquisitions
                real_flock(fd, operation)
                with acquisition_lock:
                    acquisitions += 1
                    acquisition = acquisitions
                if acquisition == 2:
                    self.assertTrue(discarded.wait(timeout=5))

            def discard(path):
                real_discard(path)
                discarded.set()

            executor._discard_contents = discard
            with mock.patch.object(runner.fcntl, "flock", side_effect=flock):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [
                        pool.submit(executor._execute, node, True, False)
                        for _ in range(2)
                    ]
                    for future in futures:
                        future.result(timeout=10)

            self.assertEqual(acquisitions, 2)

    def test_unit_test_suites_have_twenty_shard_nodes(self):
        result = self.run_build(ROOT / "build.py", "--list")
        self.assertEqual(result.returncode, 0, result.stderr)

        targets = result.stdout.splitlines()
        self.assertIn("test_suite", targets)
        self.assertIn("test_suite_prod_parser", targets)
        for prefix in (
            "unit_tests_group_",
            "test_suite_group_",
            "test_suite_prod_parser_group_",
        ):
            with self.subTest(prefix=prefix):
                self.assertEqual(
                    [target for target in targets if target.startswith(prefix)],
                    [f"{prefix}{group:02}" for group in range(20)],
                )

    def test_test_partitions_are_deterministic_complete_and_disjoint(self):
        loader = SourceFileLoader("shitty_build_runner", str(ROOT / "build"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.assertIsNotNone(spec)
        runner = importlib.util.module_from_spec(spec)
        sys.modules[loader.name] = runner
        loader.exec_module(runner)

        with tempfile.TemporaryDirectory() as directory:
            def test_ids(values, suffix):
                context = runner.BuildContext(
                    ROOT,
                    Path(directory) / suffix,
                    runner.Flags(values),
                )
                context.load(ROOT / "build.py")
                return {
                    target.name or target.output or "\0".join(target.outputs)
                    for target in context.groups["test"]
                }

            full = test_ids({}, "full")
            partitions = [
                test_ids(
                    {"group": str(group), "group_count": "5"},
                    f"group-{group}",
                )
                for group in range(5)
            ]

            self.assertEqual(set().union(*partitions), full)
            self.assertEqual(sum(map(len, partitions)), len(full))
            self.assertEqual(
                test_ids({"group": "2", "group_count": "5"}, "repeat"),
                partitions[2],
            )


if __name__ == "__main__":
    unittest.main()
