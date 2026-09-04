import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import date
from pathlib import Path

import build


std_build = os.path.join("ext", "libstd", "build.py")
plt_build = os.path.join("ext", "plt", "build.py")
shitty_version = date.today().strftime("%Y.%m.%d")

build.flags.allow({
    "group": {
        "descr": "zero-based test partition to include",
        "default": "",
    },
    "group_count": {
        "descr": "total number of test partitions",
        "default": "",
    },
})


def parse_test_partition():
    group_value = build.flags.group
    group_count_value = build.flags.group_count
    if bool(group_value) != bool(group_count_value):
        raise RuntimeError("-Dgroup and -Dgroup_count must be specified together")
    if not group_value:
        return None
    try:
        group_index = int(group_value)
        group_count = int(group_count_value)
    except ValueError as error:
        raise RuntimeError("-Dgroup and -Dgroup_count must be integers") from error
    if group_count <= 0 or group_index < 0 or group_index >= group_count:
        raise RuntimeError(
            "test partition requires 0 <= group < group_count and group_count > 0"
        )
    return group_index, group_count


test_partition = parse_test_partition()
test_ids = set()


def add_test(*targets, instrumented=True):
    for target in targets:
        test_id = target.name or target.output or "\0".join(target.outputs)
        if not test_id:
            raise RuntimeError("test target has no deterministic identifier")
        if test_id in test_ids:
            raise RuntimeError(f"test target added twice: {test_id}")
        test_ids.add(test_id)
        if test_partition is not None:
            group_index, group_count = test_partition
            digest = hashlib.sha256(test_id.encode()).digest()
            if int.from_bytes(digest[:8], "big") % group_count != group_index:
                continue
        group("test", target)
        if instrumented:
            group("instrumented-test", target)


# $(S) serves the full-path form cross-library includes use:
# lib/shitty reaches the VT core as <lib/vterm/...>.
build.includes += ["$(B)", "$(S)", "$(S)/lib/shitty", "$(S)/ext"]
build.cppflags += [f'-DSHITTY_VERSION="{shitty_version}"']
# libstd needs -std=c++26, which the Apple command-line-tools clang does
# not know; fail here with directions instead of deep inside the graph.
cxx = os.environ.get("CXX", "c++")  # the runner's own compiler default
if subprocess.run(
    [
        *build.compiler_command(cxx),
        "-std=c++26",
        "-fsyntax-only",
        "-x",
        "c++",
        os.devnull,
    ],
    capture_output=True,
).returncode != 0:
    raise RuntimeError(
        f"{cxx} does not accept -std=c++26 (Apple clang from the "
        "command line tools is too old). Install a current LLVM and point "
        "the build at it:\n"
        "    brew install llvm\n"
        '    export CC="$(brew --prefix llvm)/bin/clang"\n'
        '    export CXX="$(brew --prefix llvm)/bin/clang++"\n'
        "    ./build"
    )

build.cxxflags += [
    "-std=c++26",
    "-Og" if "-DDEBUG" in build.cppflags else "-O2",
]
production_path_flags = [
    "-ffile-prefix-map=$(S)/lib/vterm=lib/vterm",
    "-ffile-prefix-map=$(S)/lib/embed=lib/embed",
    "-ffile-prefix-map=$(S)/lib/shitty=lib",
    "-ffile-prefix-map=$(S)/bin=bin",
    "-ffile-prefix-map=$(S)/ext=ext",
    "-ffile-prefix-map=$(S)/tst=tst",
    "-ffile-prefix-map=$(B)=.",
]


darwin = "apple-darwin" in build.target
linux = "linux" in build.target


untimed_command = command

def command(**kwargs):
    # Hard per-invocation timeout so one hung test cannot wedge the whole CI
    # run. Only test invocations are wrapped: build steps (ragel, shaders,
    # helper binaries) run untimed.
    test_timeout_seconds = kwargs.pop("test_timeout_seconds", 60)

    def is_test(argv):
        if argv[0] == "$(B)/unit_tests":
            return True
        return argv[0] == sys.executable and len(argv) > 1 and (
            argv[1].startswith("tst/") or argv[1] == "-m"
        )

    cmd = kwargs.get("cmd")
    if cmd:
        nested = cmd if isinstance(cmd[0], list) else [cmd]
        if any(is_test(argv) for argv in nested):
            kwargs["cmd"] = [
                [
                    sys.executable,
                    "$(S)/tst/run_timed.py",
                    str(test_timeout_seconds),
                    *argv,
                ]
                if is_test(argv) else argv
                for argv in nested
            ]
            kwargs["inputs"] = [*kwargs.get("inputs", []), "$(S)/tst/run_timed.py"]
    return untimed_command(**kwargs)

freetype = pkg_config("freetype2", required=False)
fontconfig = pkg_config("fontconfig", required=False)
harfbuzz = pkg_config("harfbuzz", required=False)
brotli_common = pkg_config("libbrotlicommon", required=False)
simdutf = pkg_config("simdutf >= 6.5.0", required=False)

have_freetype_backend = bool(freetype and harfbuzz)
if have_freetype_backend:
    build.cppflags += ["-DHAVE_FREETYPE=1", "-DHAVE_HARFBUZZ=1"]
    if fontconfig:
        build.cppflags += ["-DHAVE_FONTCONFIG=1"]
else:
    freetype = dependency()
    fontconfig = dependency()
    harfbuzz = dependency()
    brotli_common = dependency()

if darwin:
    darwin_frameworks = os.path.join(os.environ["OSX_SDK"], "System", "Library", "Frameworks") if "OSX_SDK" in os.environ else None
    darwin_backend = dependency(ldflags=[
        *([f"-F{darwin_frameworks}"] if darwin_frameworks else []),
        "-Wl,-ObjC",
        "-Wl,-framework,AppKit",
        "-Wl,-framework,Carbon",
        "-Wl,-framework,CoreFoundation",
        "-Wl,-framework,CoreGraphics",
        "-Wl,-framework,CoreText",
        "-Wl,-framework,Foundation",
        "-Wl,-framework,IOSurface",
        "-Wl,-framework,Metal",
        "-Wl,-framework,QuartzCore",
    ])
    if darwin_frameworks:
        build.cppflags += [f"-F{darwin_frameworks}"]
    build.cppflags += ["-DHAVE_CORETEXT=1", "-DHAVE_METAL_RENDERER=1"]
else:
    darwin_backend = dependency()

threads = dependency(ldflags=["-pthread"])

vulkan = dependency()
wayland_backend = dependency()
if linux:
    vulkan = pkg_config("vulkan")
    # Nothing injects these into LDFLAGS outside the Nix shell; the
    # Linux backend has to ask for them itself (issue 66).
    wayland_backend = pkg_config("wayland-client", "xkbcommon")
    wayland_backend.ldflags += ["-lrt"]
    build.cppflags += ["-DHAVE_VULKAN_WAYLAND=1"]


embedded_path_flags = [
    "-Wno-error",
    "-ffile-prefix-map=$(S)=.",
    "-ffile-prefix-map=$(B)=.",
]


# libstd picks its backends with __has_include, so which libraries the
# archive needs at link time depends on the headers this target has. An
# imported graph exports outputs, not flags, so nothing carries them over
# the boundary: the importer has to run the same probe and ask for them
# itself, the way the Linux backend does above.
libstd_backends = []
# std/thr/wait_queue.cpp uses the generic 16-byte __atomic builtins on x86-64.
# GCC may lower them to libatomic calls even with -mcx16 (notably on musl).
# Keep the runtime after the imported static archive through its usage flags.
if linux and build.target.startswith("x86_64"):
    libstd_backends.append("-latomic")
# std/thr/io_uring.cpp, pulled in by the reactor every binary starts.
if have_header("liburing.h"):
    libstd_backends.append("-luring")
# std/str/hash.cpp prefers rapidhash, which is header-only, and falls
# back to xxhash before its own FNV-1a. Mirror that order exactly:
# probing for xxhash alone would link a library nothing calls.
if not have_header("rapidhash.h") and have_header("xxhash.h"):
    libstd_backends.append("-lxxhash")
# The TLS and DNS backends are detected the same way but stay out of this
# list: nothing here references them, so the linker never pulls their
# archive members in and their libraries would be dead weight.

libstd = import_build(std_build, "libstd.a", extra_cflags=embedded_path_flags)
libstd.ldflags += libstd_backends
libstd_external_clock = import_build(
    std_build,
    "libstd_external_clock.a",
    extra_cflags=embedded_path_flags,
    extra_cppflags=["-DSTL_EXTERNAL_MONOTONIC_NOW_US=1"],
)
libstd_external_clock.ldflags += libstd_backends


if "-lplt" in build.ldflags:
    plt = dependency(ldflags=["-lplt"])
elif os.path.isfile(os.path.join(os.path.dirname(__file__), plt_build)):
    plt = import_build(
        plt_build,
        "libplt.a",
        extra_cflags=embedded_path_flags,
        extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
    )
else:
    plt = dependency(ldflags=["-lplt"])


# plt ships its own test suite (unit tests plus fake-compositor integration
# scenarios); nothing else runs it, so import its test programs into this
# graph and stamp them into the test group. They compile against our bundled
# libstd and link the archive built by this graph.
if build.target == build.host and os.path.isfile(os.path.join(os.path.dirname(__file__), plt_build)):
    plt_test_programs = [
        import_build(
            plt_build,
            "plt_unit_tests",
            extra_cflags=embedded_path_flags,
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ),
    ]
    if linux:
        plt_test_programs.append(import_build(
            plt_build,
            "plt_wayland_integration_tests",
            extra_cflags=embedded_path_flags,
            extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd"],
            deps=[libstd],
        ))
    plt_tests = untimed_command(
        name="plt_tests",
        inputs=["$(S)/ext/plt/tests/run_timed.py"],
        outputs=["$(B)/plt-tests.stamp"],
        deps=plt_test_programs,
        cmd=[
            # The same hard per-invocation timeout the nested suite uses.
            *[
                [sys.executable, "$(S)/ext/plt/tests/run_timed.py", "120", program.output]
                for program in plt_test_programs
            ],
            [
                sys.executable, "-c",
                "from pathlib import Path; Path(r'$(B)/plt-tests.stamp').touch()",
            ],
        ],
        descr="PT",
        color="green",
    )
else:
    plt_tests = None


render_shader_names = [
    "rgba8_unorm",
    "bgra8_unorm",
    "a8b8g8r8_unorm",
    "rgba8_srgb",
    "bgra8_srgb",
    "a8b8g8r8_srgb",
    "a2b10g10r10_unorm",
    "a2r10g10b10_unorm",
    "rgba16_unorm",
    "rgba16_sfloat_linear",
    "r5g6b5_unorm",
    "b5g6r5_unorm",
    "r4g4b4a4_unorm",
    "b4g4r4a4_unorm",
    "r5g5b5a1_unorm",
    "b5g5r5a1_unorm",
    "a1r5g5b5_unorm",
]
render_shader_outputs = []
render_shader_targets = []
for render_shader_name in render_shader_names:
    render_shader_output = f"$(B)/render_shader_{render_shader_name}.inc"
    render_shader_outputs.append(render_shader_output)
    render_shader_targets.append(command(
        name=f"render_shader_{render_shader_name}",
        inputs=["$(S)/lib/shitty/render.comp", "$(S)/lib/shitty/generate_render_shaders.py"],
        outputs=[render_shader_output],
        cmd=[
            sys.executable,
            "$(S)/lib/shitty/generate_render_shaders.py",
            "compile",
            "$(S)/lib/shitty/render.comp",
            render_shader_name,
            render_shader_output,
            "glslangValidator",
        ],
        descr="SH",
        color="magenta",
    ))

render_spv = command(
    name="render_spv",
    inputs=["$(S)/lib/shitty/generate_render_shaders.py", *render_shader_outputs],
    outputs=["$(B)/render_spv.h"],
    deps=render_shader_targets,
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_render_shaders.py",
        "combine",
        "$(B)/render_spv.h",
        *render_shader_outputs,
    ],
    descr="SH",
    color="magenta",
)

if darwin:
    render_msl = command(
        name="render_msl",
        inputs=["$(S)/lib/shitty/render.comp", "$(S)/lib/shitty/generate_render_shaders.py"],
        outputs=["$(B)/render_msl.h"],
        cmd=[
            sys.executable,
            "$(S)/lib/shitty/generate_render_shaders.py",
            "metal",
            "$(S)/lib/shitty/render.comp",
            "$(B)/render_msl.h",
            "glslangValidator",
            "spirv-cross",
        ],
        descr="SH",
        color="magenta",
    )

# Ragel 6 and 7 are both accepted. Ragel 7 is language-aware and
# dropped -C, renumbered the code styles (its -G1 is a switch-driven
# generator that miscompiles scanners in 7.0.4, so production maps to
# -G0 there), and lost -x, which the parser totality checker needs -
# under ragel 7 that check simply does not run.
def ragel_major() -> int:
    text = subprocess.check_output(["ragel", "--version"], text=True)
    for token in text.split():
        if token[0].isdigit():
            return int(token.split(".")[0])
    raise RuntimeError("cannot parse ragel --version output")

ragel_is_6 = ragel_major() < 7
ragel_prod_flags = ["-C", "-G1", "-L"] if ragel_is_6 else ["-G0", "-L"]
ragel_test_flags = ["-C", "-T1", "-L"] if ragel_is_6 else ["-T1", "-L"]

# Coverage instrumentation with runtime counter relocation chokes on the
# goto-driven automata: the generated VT parser goes from a 54-second
# object to an 800-second one. The coverage tier measures lines of the
# .rl sources, not the codegen style, so it builds the production
# automata table-driven like the test parser already is.
if "-fcoverage-mapping" in os.environ.get("CXXFLAGS", ""):
    ragel_prod_flags = ragel_test_flags

totality_deps = []
if ragel_is_6:
    parser_totality = command(
        name="parser_totality",
        inputs=["$(S)/lib/vterm/parser.rl", "$(S)/lib/vterm/check_parser_totality.py"],
        outputs=["$(B)/parser.rl.total"],
        cmd=[
            sys.executable,
            "$(S)/lib/vterm/check_parser_totality.py",
            "$(S)/lib/vterm/parser.rl",
            "$(B)/parser.rl.total",
        ],
        descr="RG",
        color="magenta",
    )
    totality_deps = [parser_totality]

parser_prod = command(
    name="parser_prod",
    inputs=["$(S)/lib/vterm/parser.rl"],
    outputs=["$(B)/parser.rl.h"],
    deps=totality_deps,
    cmd=[
        "ragel",
        *ragel_prod_flags,
        "-o",
        "$(B)/parser.rl.h",
        "$(S)/lib/vterm/parser.rl",
    ],
    descr="RG",
    color="magenta",
)

unicode_data_inputs = [
    "$(S)/lib/vterm/unicode_data.py",
    "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
    "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-8.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-15.0.0.txt",
    "$(S)/ext/unicode/EastAsianWidth-17.0.0.txt",
    "$(S)/ext/unicode/GraphemeBreakProperty-17.0.0.txt",
    "$(S)/ext/unicode/IndicSyllabicCategory-17.0.0.txt",
    "$(S)/ext/unicode/emoji-data-17.0.0.txt",
    "$(S)/ext/unicode/emoji-variation-sequences-17.0.0.txt",
]
unicode_data = command(
    name="unicode_data",
    inputs=unicode_data_inputs,
    outputs=["$(B)/unicode_data.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/vterm/unicode_data.py",
        "$(S)/ext/unicode",
        "$(B)/unicode_data.h",
    ],
    descr="UD",
    color="magenta",
)


# No totality check here: unlike the VT stream, the config parser is allowed
# to reject input, so unhandled bytes are ordinary syntax errors.
toml_prod = command(
    name="toml_prod",
    inputs=["$(S)/lib/shitty/toml.rl"],
    outputs=["$(B)/toml.rl.h"],
    cmd=[
        "ragel",
        *ragel_prod_flags,
        "-o",
        "$(B)/toml.rl.h",
        "$(S)/lib/shitty/toml.rl",
    ],
    descr="RG",
    color="magenta",
)

parser_test = command(
    name="parser_test",
    inputs=["$(S)/lib/vterm/parser.rl"],
    outputs=["$(B)/parser_test.rl.h"],
    deps=totality_deps,
    cmd=[
        "ragel",
        *ragel_test_flags,
        "-o",
        "$(B)/parser_test.rl.h",
        "$(S)/lib/vterm/parser.rl",
    ],
    descr="RG",
    color="magenta",
)


input_keys = command(
    name="input_keys",
    inputs=["$(S)/lib/shitty/generate_input_keys.py", "$(S)/ext/plt/input.h"],
    outputs=["$(B)/input_keys.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_input_keys.py",
        "$(S)/ext/plt/input.h",
        "$(B)/input_keys.h",
    ],
    descr="DF",
    color="magenta",
)


def icon_png(name, svg, png):
    # The rasterizer comes from the ambient environment: rsvg-convert
    # when present, otherwise svg2png, which has no output flag and
    # drops <input name>.png into its working directory.
    if shutil.which("rsvg-convert") is not None or shutil.which("svg2png") is None:
        cmd = ["rsvg-convert", "-w", "1024", "-h", "1024", svg, "-o", png]
    else:
        produced = "$(B)/" + Path(svg).name + ".png"
        cmd = [
            ["svg2png", svg, "1024x1024"],
            [
                sys.executable,
                "-c",
                f"import os; os.replace(r'{produced}', r'{png}')",
            ],
        ]
    return command(
        name=name,
        inputs=[svg],
        outputs=[png],
        cmd=cmd,
        descr="SV",
        color="magenta",
    )


shitty_icon_png = icon_png("shitty_icon_png", "$(S)/bin/st/shitty.svg", "$(B)/shitty.png")


shitty_icon_data = command(
    name="shitty_icon_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shitty.png",
    ],
    deps=[shitty_icon_png],
    outputs=["$(B)/shitty_icon_data.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/shitty_icon_data.h",
        "shittyIcon=$(B)/shitty.png",
    ],
    descr="IC",
    color="magenta",
)


pretty_icon_png = icon_png("pretty_icon_png", "$(S)/bin/pt/pretty.svg", "$(B)/pretty.png")


pretty_icon_data = command(
    name="pretty_icon_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/pretty.png",
    ],
    deps=[pretty_icon_png],
    outputs=["$(B)/pretty_icon_data.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/pretty_icon_data.h",
        "prettyIcon=$(B)/pretty.png",
    ],
    descr="IC",
    color="magenta",
)


font_coverage = command(
    name="font_coverage",
    inputs=[
        "$(S)/lib/shitty/generate_font_coverage.py",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    outputs=["$(B)/font_coverage.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_font_coverage.py",
        "$(B)/font_coverage.h",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    descr="DF",
    color="magenta",
)


font_data = command(
    name="font_data",
    inputs=[
        "$(S)/lib/shitty/generate_font_data.py",
        "$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "$(S)/ext/fonts/NotoColorEmoji.ttf",
        "$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    outputs=["$(B)/font_data.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/generate_font_data.py",
        "$(B)/font_data.h",
        "embeddedFontMono=$(S)/ext/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "embeddedFontEmoji=$(S)/ext/fonts/NotoColorEmoji.ttf",
        "embeddedFontEmojiText=$(S)/ext/fonts/NotoEmoji-Regular.ttf",
    ],
    descr="FD",
    color="magenta",
)


terminal_colors_data = command(
    inputs=[
        "$(S)/lib/shitty/terminal_colors.json",
        "$(S)/lib/shitty/terminal_colors.py",
    ],
    outputs=["$(B)/terminal_colors.json.h"],
    cmd=[
        sys.executable,
        "$(S)/lib/shitty/terminal_colors.py",
        "generate",
        "$(B)/terminal_colors.json.h",
    ],
    descr="TC",
    color="magenta",
)


main_source = "$(S)/lib/shitty/main.cpp"
shitty_main_source = "$(S)/bin/st/main.cpp"
pretty_main_source = "$(S)/bin/pt/main.cpp"
fuzz_source = "$(S)/bin/main_fuzz/main.cpp"
heap_profile_source = "$(S)/lib/shitty/heap_profile.cpp"
parser_source = "$(S)/lib/vterm/parser.cpp"
toml_source = "$(S)/lib/shitty/toml.cpp"
toml_dump_source = "$(S)/bin/toml_dump/main.cpp"
parser_perf_source = "$(S)/bin/parser_perf/main.cpp"
unit_sources = sorted(build.glob("$(S)/lib/shitty/*_ut.cpp") + build.glob("$(S)/lib/vterm/*_ut.cpp"))
platform_font_sources = {
    "$(S)/lib/shitty/font_freetype.cpp",
}
platform_renderer_sources = {
    "$(S)/lib/shitty/render_vk.cpp",
}
enabled_font_sources = set()
if have_freetype_backend:
    enabled_font_sources.add("$(S)/lib/shitty/font_freetype.cpp")
enabled_renderer_sources = set()
if linux:
    enabled_renderer_sources.add("$(S)/lib/shitty/render_vk.cpp")
all_libshitty_sources = [
    source for source in build.glob("$(S)/lib/shitty/*.cpp") + build.glob("$(S)/lib/vterm/*.cpp")
    if source not in (heap_profile_source, *unit_sources)
    and (source not in platform_font_sources or source in enabled_font_sources)
    and (source not in platform_renderer_sources or source in enabled_renderer_sources)
]
if darwin:
    all_libshitty_sources.append({
        "src": "$(S)/lib/shitty/render_metal.mm",
        "inputs": ["$(B)/render_msl.h"],
    })
    all_libshitty_sources.append("$(S)/lib/shitty/ui_csd_tabs.mm")
vterm_source = "$(S)/lib/vterm/vterm.cpp"
font_embedded_source = "$(S)/lib/shitty/font_embedded.cpp"
application_source = "$(S)/lib/shitty/application.cpp"
terminal_colors_source = "$(S)/lib/shitty/terminal_colors.cpp"
grapheme_source = "$(S)/lib/shitty/grapheme.cpp"
unicode_source = "$(S)/lib/vterm/unicode.cpp"
libshitty_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/toml.rl.h"],
    } if source == toml_source else {
        "src": source,
        "inputs": ["$(B)/font_data.h", "$(B)/font_coverage.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/terminal_colors.json.h"],
    } if source == terminal_colors_source else {
        "src": source,
        "inputs": ["$(B)/unicode_data.h"],
    } if source == unicode_source else source
    for source in all_libshitty_sources
]
libshitty_test_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser_test.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/toml.rl.h"],
    } if source == toml_source else {
        "src": source,
        "inputs": ["$(B)/font_data.h", "$(B)/font_coverage.h"],
    } if source == font_embedded_source else {
        "src": source,
        "inputs": ["$(B)/terminal_colors.json.h"],
    } if source == terminal_colors_source else {
        "src": source,
        "inputs": ["$(B)/unicode_data.h"],
    } if source == unicode_source else source
    for source in all_libshitty_sources
]
libshitty_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd,
    brotli_common, simdutf,
]
libshitty_test_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd,
    brotli_common, simdutf,
]
libshitty_fuzz_deps = [
    freetype, fontconfig, harfbuzz, darwin_backend, plt, vulkan, wayland_backend, threads, libstd_external_clock,
    brotli_common, simdutf,
]


libshitty = library(
    srcs=libshitty_sources,
    cxxflags=production_path_flags,
    deps=libshitty_deps,
    output="$(B)/libshitty_prod.a",
)


st = program(
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h"],
    }],
    cxxflags=production_path_flags,
    deps=[libshitty],
)


pt = program(
    name="pt",
    output="$(B)/pt",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h"],
    }],
    cxxflags=production_path_flags,
    deps=[libshitty],
)


heap_profile_cxxflags = [
    "-g",
    "-fno-omit-frame-pointer",
    "-mno-omit-leaf-frame-pointer",
]


libshitty_memprofile = library(
    name="libshitty_memprofile",
    srcs=libshitty_sources,
    cxxflags=heap_profile_cxxflags,
    deps=libshitty_deps,
    output="$(B)/libshitty_memprofile.a",
)


st_memprofile = program(
    name="st_memprofile",
    output="$(B)/st_memprofile",
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h"],
    }, heap_profile_source],
    cxxflags=heap_profile_cxxflags,
    cppflags=["-DSHITTY_HEAP_PROFILE=1"],
    deps=[libshitty_memprofile],
)


# The control protocol is compiled into both binaries. SHITTY_FOR_TESTS only
# opens its application entry point and exposes Vterm::testApi().
libshitty_test = library(
    name="libshitty_test",
    srcs=libshitty_test_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1", "-DSHITTY_COMPACT_PARSER=1"],
    deps=libshitty_test_deps,
    output="$(B)/libshitty_test.a",
)


# The fuzz target owns monotonicNowUs() so it can advance time exactly one
# record at a time. Its libstd variant deliberately omits the default clock.
libshitty_fuzz = library(
    name="libshitty_fuzz",
    srcs=libshitty_test_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1", "-DSHITTY_COMPACT_PARSER=1"],
    deps=libshitty_fuzz_deps,
    output="$(B)/libshitty_fuzz.a",
)


st_test = program(
    name="st_test",
    output="$(B)/st_test",
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test],
)


pt_test = program(
    name="pt_test",
    output="$(B)/pt_test",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test],
)


main_fuzz = program(
    srcs=[fuzz_source],
    deps=[libshitty_fuzz],
)


# Same test build against the production ragel backend (-G1): the two
# backends share the C++ semantics but not the generated code, and only
# this variant executes what ships in st.
libshitty_test_prod_parser = library(
    name="libshitty_test_prod_parser",
    srcs=libshitty_sources,
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=libshitty_test_deps,
    output="$(B)/libshitty_test_prod_parser.a",
)


st_test_prod_parser = program(
    name="st_test_prod_parser",
    output="$(B)/st_test_prod_parser",
    srcs=[{
        "src": shitty_main_source,
        "inputs": ["$(B)/shitty_icon_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test_prod_parser],
)


pt_test_prod_parser = program(
    name="pt_test_prod_parser",
    output="$(B)/pt_test_prod_parser",
    srcs=[{
        "src": pretty_main_source,
        "inputs": ["$(B)/pretty_icon_data.h"],
    }],
    cppflags=["-DSHITTY_FOR_TESTS=1"],
    deps=[libshitty_test_prod_parser],
)


pty_test_helper = program(
    name="pty_test_helper",
    output="$(B)/pty_test_helper",
    srcs=["$(S)/tst/pty_test_helper.c"],
)


# The kernel-truth probe behind the Darwin-gated pty unit tests; built
# on demand, run by hand on the host being characterized.
pty_probe = program(
    name="pty_probe",
    output="$(B)/pty_probe",
    srcs=["$(S)/tst/pty_probe.c"],
)


unit_tests = program(
    name="unit_tests",
    output="$(B)/unit_tests",
    srcs=["$(S)/ext/libstd/tst/test.cpp", *unit_sources],
    deps=[libshitty_test, libstd],
)


toml_dump = program(
    name="toml_dump",
    output="$(B)/toml_dump",
    srcs=[toml_dump_source],
    deps=[libshitty_test, libstd],
)


# The production parser alone, driven with no-op callbacks; a perf
# probe that isolates the FSM from vterm and the screen.
parser_perf = program(
    name="parser_perf",
    output="$(B)/parser_perf",
    srcs=[parser_perf_source],
    deps=[libshitty, libstd],
)


# The whole VT core - parser, vterm and screen - driven headlessly over
# a corpus file; the throughput an embedder would actually see.
core_perf = program(
    name="core_perf",
    output="$(B)/core_perf",
    srcs=["$(S)/bin/core_perf/main.cpp"],
    deps=[libshitty, libstd],
)


# The C embedding facade over the VT core: shitty_vt_* in lib/embed,
# linked with libstd and a headless-only libplt. `./build a` bundles
# the three into one static archive, `./build so` links the shared
# library with everything but the facade hidden by the version script.
# Everything embed-side is compiled -fPIC: `build so` needs it, and a
# position-independent static archive is what a consumer linking the
# facade into their own shared object wants anyway.
plt_headless = import_build(
    plt_build,
    "libplt_headless.a",
    extra_cflags=[*embedded_path_flags, "-fPIC"],
    extra_cxxflags=["-fPIC"],
    extra_cppflags=["-Dno_vendored_std", "-I$(S)/../libstd", "-Dplatforms=headless"],
)
libstd_pic = import_build(
    std_build,
    "libstd_pic.a",
    extra_cflags=[*embedded_path_flags, "-fPIC"],
    extra_cxxflags=["-fPIC"],
)
libstd_pic.ldflags += libstd_backends

embed_sources = [
    {
        "src": source,
        "inputs": ["$(B)/parser.rl.h"],
    } if source == parser_source else {
        "src": source,
        "inputs": ["$(B)/unicode_data.h"],
    } if source == unicode_source else source
    for source in sorted(build.glob("$(S)/lib/vterm/*.cpp"))
    if source not in unit_sources
] + ["$(S)/lib/embed/shitty_vt.cpp"]

libshitty_vt_core = library(
    name="libshitty_vt_core",
    srcs=embed_sources,
    cflags=["-fPIC"],
    cxxflags=[*production_path_flags, "-fPIC"],
    deps=[plt_headless, libstd_pic, simdutf],
    output="$(B)/libshitty_vt_core.a",
)

example = program(
    name="example",
    output="$(B)/example",
    srcs=["$(S)/bin/example/main.c"],
    deps=[libshitty_vt_core, plt_headless, libstd_pic, simdutf],
)

if linux:
    shitty_vt_a = command(
        name="shitty_vt_a",
        inputs=[
            "$(S)/lib/embed/merge_archives.py",
            libshitty_vt_core.output,
            plt_headless.output,
            libstd_pic.output,
        ],
        outputs=["$(B)/libshitty_vt.a"],
        deps=[libshitty_vt_core, libstd_pic, plt_headless],
        cmd=[[
            sys.executable,
            "$(S)/lib/embed/merge_archives.py",
            "$(B)/libshitty_vt.a",
            libshitty_vt_core.output,
            plt_headless.output,
            libstd_pic.output,
        ]],
        descr="AR",
        color="magenta",
    )
    group("a", shitty_vt_a)

    shitty_vt_so = command(
        name="shitty_vt_so",
        inputs=[
            "$(S)/lib/embed/link_shared.py",
            "$(S)/lib/embed/shitty_vt.map",
            libshitty_vt_core.output,
            plt_headless.output,
            libstd_pic.output,
        ],
        outputs=["$(B)/libshitty_vt.so"],
        deps=[libshitty_vt_core, libstd_pic, plt_headless],
        cmd=[[
            sys.executable,
            "$(S)/lib/embed/link_shared.py",
            "$(B)/libshitty_vt.so",
            "$(S)/lib/embed/shitty_vt.map",
            libshitty_vt_core.output,
            plt_headless.output,
            libstd_pic.output,
            *simdutf.ldflags,
            # libstd's hash, atomic and io_uring backends are probed, so the
            # shared link needs whatever the probe chose; --no-undefined
            # rejects the library outright without them.
            *libstd_backends,
        ]],
        descr="SO",
        color="magenta",
    )
    group("so", shitty_vt_so)

    # The release tarball: both libraries, the header, and a pkg-config
    # file carrying the link flags this build probed - the discovery
    # story of issue 102. Relocatable; point PKG_CONFIG_PATH at its
    # lib/pkgconfig after unpacking.
    shitty_vt_tgz = command(
        name="shitty_vt_tgz",
        inputs=[
            "$(S)/lib/embed/make_release.py",
            "$(S)/lib/embed/shitty_vt.h",
            "$(B)/libshitty_vt.a",
            "$(B)/libshitty_vt.so",
        ],
        outputs=["$(B)/shitty_vt.tgz"],
        deps=[shitty_vt_a, shitty_vt_so],
        cmd=[[
            sys.executable,
            "$(S)/lib/embed/make_release.py",
            "$(B)/shitty_vt.tgz",
            shitty_version,
            "$(S)/lib/embed/shitty_vt.h",
            "$(B)/libshitty_vt.a",
            "$(B)/libshitty_vt.so",
            "--",
            *simdutf.ldflags,
            *libstd_backends,
            "-lpthread",
            "-lm",
        ]],
        descr="TZ",
        color="magenta",
    )
    group("tgz", shitty_vt_tgz)


# Each shard is an independent graph node with its own hard timeout.
test_group_count = 20
python_test_inputs = [
    "$(S)/build",
    "$(S)/build.py",
    "$(S)/README.md",
    *build.glob("$(S)/LICENSE.*"),
    "$(S)/dev/ci_report.py",
    "$(S)/lib/shitty/heap_profile.cpp",
    "$(S)/bin/main_fuzz/main.cpp",
    "$(S)/bin/parser_perf/main.cpp",
    "$(S)/bin/core_perf/main.cpp",
    *build.glob("$(S)/lib/shitty/*_ut.cpp"),
    *build.glob("$(S)/lib/vterm/*_ut.cpp"),
    *build.glob("$(S)/tst/*.py"),
    *build.glob("$(S)/tst/*.md"),
    "$(S)/tst/pty_test_helper.c",
    *build.glob("$(S)/ext/fonts/*"),
    # The color-scheme suite reads the imported theme licenses, the
    # embed differential replays the recorded fuzz corpus.
    *build.glob("$(S)/ext/LICENSE.*"),
    *build.glob("$(S)/tst/corpus/*"),
    *build.glob("$(S)/tst/**/*file_names.txt"),
    *build.glob("$(S)/tst/**/xfail.txt"),
    *build.glob("$(S)/tst/contour/vttest/*"),
    "$(S)/tst/termless/cases.json",
    "$(S)/tst/termless/upstream/LICENSE",
    *build.glob("$(S)/tst/termless/upstream/**/*.ts"),
    "$(S)/tst/tmux/upstream/input-fuzzer.dict",
    *[
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((Path(__file__).parent / "tst" / "toml").rglob("*"))
        if path.is_file()
    ],
    *build.glob("$(S)/tst/ucs_detect/*.txt"),
    *build.glob("$(S)/tst/vte/upstream/parser-*.hh"),
    *build.glob("$(S)/tst/vtebench/benchmarks/*/*"),
    "$(S)/tst/wezterm/catalog.py",
    "$(S)/tst/windows_terminal/upstream/KittyKeyboardProtocol.cpp",
    *build.glob("$(S)/tst/windows_terminal/upstream/*Test*.cpp"),
    "$(S)/tst/wraptest/cases.json",
    "$(S)/tst/wraptest/wraptest.c",
    *build.glob("$(S)/tst/xterm_vttests/upstream/*"),
    "$(S)/ext/libstd/build.py",
    *build.glob("$(S)/ext/libstd/**/*_ut.cpp"),
    *build.glob("$(S)/ext/libstd/tst/*.cpp"),
    "$(S)/ext/plt/build.py",
    *build.glob("$(S)/ext/plt/*_ut.cpp"),
    *build.glob("$(S)/ext/plt/tests/*"),
    "$(S)/lib/shitty/application.cpp",
    "$(S)/bin/pt/pretty.desktop",
    "$(S)/bin/pt/pretty.toml",
    "$(S)/bin/st/shitty.desktop",
    "$(S)/bin/st/shitty.toml",
    "$(S)/lib/shitty/terminal_colors.json",
    "$(S)/lib/shitty/terminal_colors.py",
    *unicode_data_inputs,
    "$(S)/ext/unicode/GraphemeBreakTest-17.0.0.txt",
]


def touch_stamp(path):
    return [
        sys.executable,
        "-c",
        f"from pathlib import Path; Path(r'{path}').touch()",
    ]


unit_test_groups = []
for group_index in range(test_group_count):
    output = f"$(B)/unit-tests/group-{group_index:02}.stamp"
    unit_test_groups.append(command(
        name=f"unit_tests_group_{group_index:02}",
        outputs=[output],
        deps=[unit_tests, pty_test_helper],
        cmd=[
            [
                "$(B)/unit_tests",
                f"--group={group_index}",
                f"--group-count={test_group_count}",
                "--threads=1",
            ],
            touch_stamp(output),
        ],
        env={"SHITTY_PTY_TEST_HELPER": "$(B)/pty_test_helper"},
        # A group holds real-child pty tests with multi-second sleeps, and
        # continuous-mode coverage instrumentation slows the rest; on a
        # 4-vCPU CI runner the default 60-second budget is too tight.
        test_timeout_seconds=180,
        descr="UT",
        color="green",
    ))


def make_python_test_groups(name, output_directory, test_binary, test_target, pretty_test_binary, pretty_test_target, descr):
    result = []

    for group_index in range(test_group_count):
        output = f"$(B)/{output_directory}/group-{group_index:02}.stamp"
        result.append(command(
            name=f"{name}_group_{group_index:02}",
            inputs=python_test_inputs,
            outputs=[output],
            deps=[test_target, pretty_test_target, toml_dump, example],
            cmd=[
                [
                    sys.executable,
                    "tst/run_unittest_group.py",
                    f"--group={group_index}",
                    f"--group-count={test_group_count}",
                ],
                touch_stamp(output),
            ],
            cwd="$(S)",
            env={
                "SHITTY_TEST_BINARY": test_binary,
                "SHITTY_EMBED_EXAMPLE_BINARY": "$(B)/example",
                "SHITTY_PRETTY_TEST_BINARY": pretty_test_binary,
                "SHITTY_TOML_DUMP_BINARY": "$(B)/toml_dump",
                "SHITTY_TEST_FONTCONFIG": "1" if fontconfig else "0",
                "SHITTY_TEST_PLATFORM": "cocoa" if darwin else "wayland",
                "SHITTY_TEST_VERSION": shitty_version,
            },
            # A group contains about 210 independent tests.  Keep the
            # per-test default at 60 seconds, but allow the complete group to
            # absorb slow sandbox-only metadata probes without racing its own
            # aggregate timeout.
            test_timeout_seconds=120,
            descr=descr,
            color="cyan",
        ))

    return result


python_test_groups = make_python_test_groups(
    "test_suite",
    "python-tests",
    "$(B)/st_test",
    st_test,
    "$(B)/pt_test",
    pt_test,
    "TS",
)
python_test_prod_parser_groups = make_python_test_groups(
    "test_suite_prod_parser",
    "python-tests-prod-parser",
    "$(B)/st_test_prod_parser",
    st_test_prod_parser,
    "$(B)/pt_test_prod_parser",
    pt_test_prod_parser,
    "TP",
)


pretty_binary_branding = command(
    name="pretty_binary_branding",
    inputs=["$(S)/tst/pretty_binary_branding.py"],
    outputs=["$(B)/tst/pretty-binary-branding.stamp"],
    deps=[pt],
    cmd=[
        [sys.executable, "tst/pretty_binary_branding.py", "$(B)/pt"],
        touch_stamp("$(B)/tst/pretty-binary-branding.stamp"),
    ],
    cwd="$(S)",
    descr="PB",
    color="cyan",
)


production_surface = command(
    inputs=[
        "$(S)/tst/production_surface.py",
        "$(S)/bin/pt/pretty.toml",
    ],
    outputs=["$(B)/tst/production-surface.stamp"],
    deps=[st, pt],
    cmd=[
        [sys.executable, "tst/production_surface.py"],
        touch_stamp("$(B)/tst/production-surface.stamp"),
    ],
    cwd="$(S)",
    env={
        "SHITTY_PRODUCTION_BINARY": "$(B)/st",
        "SHITTY_PRETTY_BINARY": "$(B)/pt",
        "SHITTY_TEST_VERSION": shitty_version,
    },
    descr="PS",
    color="cyan",
)


# Issue #104's invariant on the production binary: PTY progress must
# survive the compositor withholding frame callbacks. Runs a headless
# sway and powers its output off; skips without sway or a Vulkan
# device unless SHITTY_TEST_WAYLAND_REQUIRED=1 (CI sets it).
wayland_frame_stall = command(
    name="wayland_frame_stall",
    inputs=["$(S)/tst/wayland_frame_stall.py"],
    outputs=["$(B)/tst/wayland-frame-stall.stamp"],
    deps=[st],
    cmd=[
        ["python3", "tst/wayland_frame_stall.py"],
        touch_stamp("$(B)/tst/wayland-frame-stall.stamp"),
    ],
    cwd="$(S)",
    env={"SHITTY_PRODUCTION_BINARY": "$(B)/st"},
    test_timeout_seconds=120,
    descr="WF",
    color="cyan",
)


# Issue #108's invariant on the production binary: the foreground
# process name fills the title while the app sets none, and a standing
# foreground keeps the title it set. Same sway harness and skip rules
# as wayland_frame_stall.
wayland_title_fallback = command(
    name="wayland_title_fallback",
    inputs=[
        "$(S)/tst/wayland_title_fallback.py",
        "$(S)/tst/wayland_frame_stall.py",
    ],
    outputs=["$(B)/tst/wayland-title-fallback.stamp"],
    deps=[st],
    cmd=[
        ["python3", "tst/wayland_title_fallback.py"],
        touch_stamp("$(B)/tst/wayland-title-fallback.stamp"),
    ],
    cwd="$(S)",
    env={"SHITTY_PRODUCTION_BINARY": "$(B)/st"},
    test_timeout_seconds=120,
    descr="WT",
    color="cyan",
)


test_suite = untimed_command(
    inputs=["$(S)/build.py"],
    outputs=["$(B)/tests.stamp"],
    deps=[*unit_test_groups, *python_test_groups],
    cmd=touch_stamp("$(B)/tests.stamp"),
    descr="TS",
    color="cyan",
)


test_suite_prod_parser = untimed_command(
    name="test_suite_prod_parser",
    inputs=["$(S)/build.py"],
    outputs=["$(B)/tests-prod-parser.stamp"],
    deps=python_test_prod_parser_groups,
    cmd=touch_stamp("$(B)/tests-prod-parser.stamp"),
    descr="TP",
    color="cyan",
)


parser_fuzz = command(
    inputs=["$(S)/tst/fuzz_parser.py", "$(S)/tst/harness.py"],
    outputs=["$(B)/parser-fuzz.stamp"],
    deps=[st_test],
    cmd=[
        [sys.executable, "tst/fuzz_parser.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; Path(r'$(B)/parser-fuzz.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="FZ",
    color="yellow",
)


vttest_profile = command(
    inputs=["$(S)/tst/vttest.py", "$(S)/tst/harness.py"],
    outputs=["$(B)/vttest.stamp"],
    deps=[st_test],
    cmd=[
        [sys.executable, "tst/vttest.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; Path(r'$(B)/vttest.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="VT",
    color="blue",
)


xtermjs_root = Path(__file__).parent / "tst" / "xtermjs"
xtermjs_cases = (xtermjs_root / "file_names.txt").read_text().split()
xtermjs_tests = []
for case in xtermjs_cases:
    xtermjs_tests.append(command(
        name="xtermjs_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/xtermjs/adapter.py",
            "$(S)/tst/xtermjs/file_names.txt",
            "$(S)/tst/xtermjs/xfail.txt",
            f"$(S)/tst/xtermjs/{case}.in",
            f"$(S)/tst/xtermjs/{case}.text",
        ],
        outputs=[f"$(B)/tst/xtermjs/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/xtermjs/adapter.py",
            case,
            "tst/xtermjs/xfail.txt",
            f"$(B)/tst/xtermjs/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XJ",
        color="cyan",
    ))


alacritty_root = Path(__file__).parent / "tst" / "alacritty"
alacritty_cases = (alacritty_root / "file_names.txt").read_text().split()
alacritty_tests = []
for case in alacritty_cases:
    alacritty_tests.append(command(
        name="alacritty_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/alacritty/adapter.py",
            "$(S)/tst/alacritty/file_names.txt",
            "$(S)/tst/alacritty/xfail.txt",
            f"$(S)/tst/alacritty/{case}/alacritty.recording",
            f"$(S)/tst/alacritty/{case}/config.json",
            f"$(S)/tst/alacritty/{case}/grid.json",
            f"$(S)/tst/alacritty/{case}/size.json",
        ],
        outputs=[f"$(B)/tst/alacritty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/alacritty/adapter.py",
            case,
            "tst/alacritty/xfail.txt",
            f"$(B)/tst/alacritty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="AL",
        color="cyan",
    ))


contour_vttest_sources = [
    source for source in build.glob("$(S)/tst/contour/vttest/*.c")
    if not source.endswith("/vms_io.c")
]
contour_vttest = program(
    name="contour_vttest_helper",
    srcs=contour_vttest_sources,
    cflags=["-Wno-error"],
    cppflags=[
        "-DHAVE_CONFIG_H",
        "-I$(S)/tst/contour/vttest",
    ],
    output="$(B)/tst/contour/vttest",
)


contour_root = Path(__file__).parent / "tst" / "contour"
contour_cases = (contour_root / "file_names.txt").read_text().split()
contour_tests = []
for case in contour_cases:
    golden_inputs = [
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((contour_root / "golden").glob(f"{case}.step*.dump"))
    ]
    contour_tests.append(command(
        name="contour_" + case.replace(".", "_"),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/contour/adapter.py",
            "$(S)/tst/contour/file_names.txt",
            "$(S)/tst/contour/scenarios.json",
            "$(S)/tst/contour/xfail.txt",
            *golden_inputs,
        ],
        outputs=[f"$(B)/tst/contour/{case}.stamp"],
        deps=[st_test, contour_vttest],
        cmd=[
            sys.executable,
            "tst/contour/adapter.py",
            "$(B)/tst/contour/vttest",
            case,
            "tst/contour/xfail.txt",
            f"$(B)/tst/contour/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="CO",
        color="cyan",
    ))


mosh_tests = []
for corpus in ("terminal_corpus", "terminal_parser_corpus"):
    mosh_tests.append(command(
        name="mosh_" + corpus,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/mosh/adapter.py",
            "$(S)/tst/mosh/xfail.txt",
            *build.glob(f"$(S)/tst/mosh/{corpus}/*"),
        ],
        outputs=[f"$(B)/tst/mosh/{corpus}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/mosh/adapter.py",
            corpus,
            "tst/mosh/xfail.txt",
            f"$(B)/tst/mosh/{corpus}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_root = Path(__file__).parent / "tst" / "mosh"
mosh_semantic_cases = (
    mosh_root / "semantic_file_names.txt"
).read_text().split()
mosh_semantic_tests = []
for case in mosh_semantic_cases:
    mosh_semantic_tests.append(command(
        name="mosh_semantic_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/mosh/semantic_adapter.py",
            "$(S)/tst/mosh/semantic_cases.py",
            "$(S)/tst/mosh/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/mosh/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/mosh/semantic_adapter.py",
            case,
            f"$(B)/tst/mosh/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="MO",
        color="cyan",
    ))

mosh_semantic_validation = command(
    name="mosh_semantic_catalog",
    inputs=[
        "$(S)/tst/mosh/semantic_cases.py",
        "$(S)/tst/mosh/semantic_file_names.txt",
        "$(S)/tst/mosh/semantic_validate.py",
    ],
    outputs=["$(B)/tst/mosh/semantic/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/mosh/semantic_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/mosh/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="MO",
    color="cyan",
)


libtsm_root = Path(__file__).parent / "tst" / "libtsm"
libtsm_semantic_cases = (
    libtsm_root / "semantic_file_names.txt"
).read_text().split()
libtsm_semantic_tests = []
for case in libtsm_semantic_cases:
    libtsm_semantic_tests.append(command(
        name="libtsm_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/libtsm/semantic_adapter.py",
            "$(S)/tst/libtsm/semantic_cases.py",
            "$(S)/tst/libtsm/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/libtsm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/libtsm/semantic_adapter.py",
            case,
            f"$(B)/tst/libtsm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LT",
        color="cyan",
    ))

libtsm_semantic_validation = command(
    name="libtsm_semantic_catalog",
    inputs=[
        "$(S)/tst/libtsm/semantic_cases.py",
        "$(S)/tst/libtsm/semantic_file_names.txt",
        "$(S)/tst/libtsm/semantic_validate.py",
    ],
    outputs=["$(B)/tst/libtsm/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/libtsm/semantic_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/libtsm/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="LT",
    color="cyan",
)


ghostty_root = Path(__file__).parent / "tst" / "ghostty"
ghostty_members = []
for corpus in ("osc-cmin", "parser-cmin", "stream-cmin"):
    ghostty_members.extend(
        f"{corpus}/{path.name}"
        for path in sorted((ghostty_root / corpus).iterdir())
    )
ghostty_xfails = {
    line.strip()
    for line in (ghostty_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_ghostty_xfails = ghostty_xfails - set(ghostty_members)
if unknown_ghostty_xfails:
    raise RuntimeError(
        "unknown Ghostty XFAIL members: "
        + ", ".join(sorted(unknown_ghostty_xfails))
    )

ghostty_tests = []
ghostty_shard_size = 64
for corpus in ("osc-cmin", "parser-cmin", "stream-cmin"):
    members = [
        member for member in ghostty_members
        if member.startswith(corpus + "/")
    ]
    for shard_index, start in enumerate(range(0, len(members), ghostty_shard_size)):
        shard = members[start : start + ghostty_shard_size]
        name = corpus.replace("-", "_") + f"_{shard_index:03d}"
        ghostty_tests.append(command(
            name="ghostty_" + name,
            inputs=[
                "$(S)/tst/harness.py",
                "$(S)/tst/fuzz_parser.py",
                "$(S)/tst/ghostty/adapter.py",
                "$(S)/tst/ghostty/xfail.txt",
                *("$(S)/tst/ghostty/" + member for member in shard),
            ],
            outputs=[f"$(B)/tst/ghostty/{name}.stamp"],
            deps=[st_test],
            cmd=[
                sys.executable,
                "tst/ghostty/adapter.py",
                "tst/ghostty/xfail.txt",
                f"$(B)/tst/ghostty/{name}.stamp",
                *shard,
            ],
            cwd="$(S)",
            env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
            descr="GH",
            color="cyan",
        ))


ghostty_semantic_cases = (
    ghostty_root / "semantic_file_names.txt"
).read_text().split()
ghostty_semantic_tests = []
for case in ghostty_semantic_cases:
    ghostty_semantic_tests.append(command(
        name="ghostty_model_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/ghostty/semantic_adapter.py",
            "$(S)/tst/ghostty/semantic_catalog.py",
            "$(S)/tst/ghostty/semantic_file_names.txt",
            "$(S)/tst/ghostty/semantic_xfail.txt",
            "$(S)/tst/ghostty/upstream/stream_terminal_tests.zig",
        ],
        outputs=[f"$(B)/tst/ghostty/model/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/ghostty/semantic_adapter.py",
            case,
            "tst/ghostty/semantic_xfail.txt",
            f"$(B)/tst/ghostty/model/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="GH",
        color="cyan",
    ))


ghostty_semantic_validation = command(
    name="ghostty_model_catalog",
    inputs=[
        "$(S)/tst/ghostty/semantic_catalog.py",
        "$(S)/tst/ghostty/semantic_file_names.txt",
        "$(S)/tst/ghostty/semantic_validate.py",
        "$(S)/tst/ghostty/semantic_xfail.txt",
        "$(S)/tst/ghostty/upstream/stream_terminal_tests.zig",
    ],
    outputs=["$(B)/tst/ghostty/model/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/ghostty/semantic_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/ghostty/model/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="GH",
    color="cyan",
)


kitty_root = Path(__file__).parent / "tst" / "kitty"
kitty_cases = (kitty_root / "file_names.txt").read_text().split()
kitty_tests = []
for case in kitty_cases:
    kitty_tests.append(command(
        name="kitty_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/kitty/adapter.py",
            "$(S)/tst/kitty/catalog.py",
            "$(S)/tst/kitty/file_names.txt",
            "$(S)/tst/kitty/xfail.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/kitty/adapter.py",
            case,
            "tst/kitty/xfail.txt",
            f"$(B)/tst/kitty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KI",
        color="cyan",
    ))


kitty_validation = command(
    name="kitty_catalog",
    inputs=[
        "$(S)/tst/kitty/catalog.py",
        "$(S)/tst/kitty/file_names.txt",
        "$(S)/tst/kitty/validate.py",
        "$(S)/tst/kitty/xfail.txt",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/kitty/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KI",
    color="cyan",
)


kitty_screen_cases = (
    kitty_root / "screen_file_names.txt"
).read_text().split()
kitty_screen_tests = []
for case in kitty_screen_cases:
    kitty_screen_tests.append(command(
        name="kitty_screen_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/kitty/screen_adapter.py",
            "$(S)/tst/kitty/screen_catalog.py",
            "$(S)/tst/kitty/screen_file_names.txt",
            "$(S)/tst/kitty/screen_xfail.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/kitty/screen_adapter.py",
            case,
            "tst/kitty/screen_xfail.txt",
            f"$(B)/tst/kitty/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))


kitty_screen_validation = command(
    name="kitty_screen_catalog",
    inputs=[
        "$(S)/tst/kitty/screen_catalog.py",
        "$(S)/tst/kitty/screen_file_names.txt",
        "$(S)/tst/kitty/screen_validate.py",
        "$(S)/tst/kitty/screen_xfail.txt",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/screen/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/kitty/screen_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/screen/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KS",
    color="cyan",
)


kitty_utf8 = command(
    name="kitty_utf8",
    inputs=[
        "$(S)/tst/harness.py",
        "$(S)/tst/kitty/utf8_adapter.py",
        "$(S)/tst/kitty/utf8_catalog.py",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/utf8.stamp"],
    deps=[st_test],
    cmd=[
        sys.executable,
        "tst/kitty/utf8_adapter.py",
        "$(B)/tst/kitty/utf8.stamp",
    ],
    cwd="$(S)",
    env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
    descr="KU",
    color="cyan",
)


kitty_transaction_cases = (
    kitty_root / "transaction_file_names.txt"
).read_text().split()
kitty_transaction_tests = []
for case in kitty_transaction_cases:
    kitty_transaction_tests.append(command(
        name="kitty_transaction_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/kitty/transaction_adapter.py",
            "$(S)/tst/kitty/transaction_cases.py",
            "$(S)/tst/kitty/transaction_file_names.txt",
            "$(S)/tst/kitty/upstream/parser.py",
        ],
        outputs=[f"$(B)/tst/kitty/transaction/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/kitty/transaction_adapter.py",
            case,
            f"$(B)/tst/kitty/transaction/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KT",
        color="cyan",
    ))

kitty_transaction_validation = command(
    name="kitty_transaction_catalog",
    inputs=[
        "$(S)/tst/harness.py",
        "$(S)/tst/kitty/transaction_cases.py",
        "$(S)/tst/kitty/transaction_file_names.txt",
        "$(S)/tst/kitty/transaction_validate.py",
        "$(S)/tst/kitty/upstream/parser.py",
    ],
    outputs=["$(B)/tst/kitty/transaction/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/kitty/transaction_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/kitty/transaction/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KT",
    color="cyan",
)


vte_root = Path(__file__).parent / "tst" / "vte"
vte_cases = (vte_root / "file_names.txt").read_text().split()
vte_tests = []
for case in vte_cases:
    vte_tests.append(command(
        name="vte_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/adapter.py",
            "$(S)/tst/vte/catalog.py",
            "$(S)/tst/vte/file_names.txt",
            "$(S)/tst/vte/xfail.txt",
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/adapter.py",
            case,
            "tst/vte/xfail.txt",
            f"$(B)/tst/vte/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="VE",
        color="cyan",
    ))


vte_validation = command(
    name="vte_catalog",
    inputs=[
        "$(S)/tst/vte/catalog.py",
        "$(S)/tst/vte/file_names.txt",
        "$(S)/tst/vte/validate.py",
        "$(S)/tst/vte/xfail.txt",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VE",
    color="cyan",
)


vte_known_cases = (vte_root / "known_file_names.txt").read_text().split()
vte_known_tests = []
for case in vte_known_cases:
    vte_known_tests.append(command(
        name="vte_known_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/known_adapter.py",
            "$(S)/tst/vte/known_cases.py",
            "$(S)/tst/vte/known_file_names.txt",
            *build.glob("$(S)/tst/vte/upstream/parser-*.hh"),
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/known/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/known_adapter.py",
            case,
            f"$(B)/tst/vte/known/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="VK",
        color="cyan",
    ))


vte_known_validation = command(
    name="vte_known_catalog",
    inputs=[
        "$(S)/tst/vte/known_cases.py",
        "$(S)/tst/vte/known_file_names.txt",
        "$(S)/tst/vte/known_validate.py",
        "$(S)/tst/vte/upstream/parser-esc.hh",
        "$(S)/tst/vte/upstream/parser-csi.hh",
        "$(S)/tst/vte/upstream/parser-dcs.hh",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/known/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/known_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/known/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VK",
    color="cyan",
)


vte_charset_cases = (vte_root / "charset_file_names.txt").read_text().split()
vte_charset_tests = []
for case in vte_charset_cases:
    vte_charset_tests.append(command(
        name="vte_charset_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/charset_adapter.py",
            "$(S)/tst/vte/charset_cases.py",
            "$(S)/tst/vte/charset_file_names.txt",
            "$(S)/tst/vte/upstream/parser-charset-tables.hh",
            "$(S)/tst/vte/upstream/parser-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/charset/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/charset_adapter.py",
            case,
            f"$(B)/tst/vte/charset/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_charset_validation = command(
    name="vte_charset_catalog",
    inputs=[
        "$(S)/tst/vte/charset_cases.py",
        "$(S)/tst/vte/charset_file_names.txt",
        "$(S)/tst/vte/charset_validate.py",
        "$(S)/tst/vte/upstream/parser-charset-tables.hh",
        "$(S)/tst/vte/upstream/parser-test.cc",
    ],
    outputs=["$(B)/tst/vte/charset/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/charset_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/charset/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VC",
    color="cyan",
)


vte_tabstop_cases = (vte_root / "tabstop_file_names.txt").read_text().split()
vte_tabstop_tests = []
for case in vte_tabstop_cases:
    vte_tabstop_tests.append(command(
        name="vte_tabstop_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/tabstop_adapter.py",
            "$(S)/tst/vte/tabstop_cases.py",
            "$(S)/tst/vte/tabstop_file_names.txt",
            "$(S)/tst/vte/upstream/tabstops-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/tabstops/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/tabstop_adapter.py",
            case,
            f"$(B)/tst/vte/tabstops/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VT",
        color="cyan",
    ))


vte_tabstop_validation = command(
    name="vte_tabstop_catalog",
    inputs=[
        "$(S)/tst/vte/tabstop_cases.py",
        "$(S)/tst/vte/tabstop_file_names.txt",
        "$(S)/tst/vte/tabstop_validate.py",
        "$(S)/tst/vte/upstream/tabstops-test.cc",
    ],
    outputs=["$(B)/tst/vte/tabstops/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/tabstop_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/tabstops/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VT",
    color="cyan",
)


vte_mode_cases = (vte_root / "mode_file_names.txt").read_text().split()
vte_mode_tests = []
for case in vte_mode_cases:
    vte_mode_tests.append(command(
        name="vte_mode_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/mode_adapter.py",
            "$(S)/tst/vte/mode_cases.py",
            "$(S)/tst/vte/mode_file_names.txt",
            "$(S)/tst/vte/upstream/modes-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/modes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/mode_adapter.py",
            case,
            f"$(B)/tst/vte/modes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VM",
        color="cyan",
    ))


vte_mode_validation = command(
    name="vte_mode_catalog",
    inputs=[
        "$(S)/tst/vte/mode_cases.py",
        "$(S)/tst/vte/mode_file_names.txt",
        "$(S)/tst/vte/mode_validate.py",
        "$(S)/tst/vte/upstream/modes-test.cc",
    ],
    outputs=["$(B)/tst/vte/modes/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/mode_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/modes/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VM",
    color="cyan",
)


vte_color_cases = (vte_root / "color_file_names.txt").read_text().split()
vte_color_tests = []
for case in vte_color_cases:
    vte_color_tests.append(command(
        name="vte_color_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/color_adapter.py",
            "$(S)/tst/vte/color_cases.py",
            "$(S)/tst/vte/color_file_names.txt",
            "$(S)/tst/vte/upstream/color-test.cc",
            "$(S)/tst/vte/upstream/color-names-tests.hh",
        ],
        outputs=[f"$(B)/tst/vte/color/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/color_adapter.py",
            case,
            f"$(B)/tst/vte/color/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VC",
        color="cyan",
    ))


vte_color_validation = command(
    name="vte_color_catalog",
    inputs=[
        "$(S)/tst/vte/color_cases.py",
        "$(S)/tst/vte/color_file_names.txt",
        "$(S)/tst/vte/color_validate.py",
        "$(S)/tst/vte/upstream/color-test.cc",
        "$(S)/tst/vte/upstream/color-names-tests.hh",
    ],
    outputs=["$(B)/tst/vte/color/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/color_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/color/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VC",
    color="cyan",
)


vte_paste_cases = (vte_root / "paste_file_names.txt").read_text().split()
vte_paste_tests = []
for case in vte_paste_cases:
    vte_paste_tests.append(command(
        name="vte_paste_" + case.replace("-", "_"),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/paste_adapter.py",
            "$(S)/tst/vte/paste_cases.py",
            "$(S)/tst/vte/paste_file_names.txt",
            "$(S)/tst/vte/upstream/pastify-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/paste/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/paste_adapter.py",
            case,
            f"$(B)/tst/vte/paste/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VP",
        color="cyan",
    ))


vte_paste_validation = command(
    name="vte_paste_catalog",
    inputs=[
        "$(S)/tst/vte/paste_cases.py",
        "$(S)/tst/vte/paste_file_names.txt",
        "$(S)/tst/vte/paste_validate.py",
        "$(S)/tst/vte/upstream/pastify-test.cc",
    ],
    outputs=["$(B)/tst/vte/paste/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/paste_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/paste/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VP",
    color="cyan",
)


vte_utf8_cases = (vte_root / "utf8_file_names.txt").read_text().split()
vte_utf8_tests = []
for case in vte_utf8_cases:
    vte_utf8_tests.append(command(
        name="vte_utf8_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/utf8_adapter.py",
            "$(S)/tst/vte/utf8_cases.py",
            "$(S)/tst/vte/utf8_file_names.txt",
        ],
        outputs=[f"$(B)/tst/vte/utf8/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/utf8_adapter.py",
            case,
            f"$(B)/tst/vte/utf8/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VU",
        color="cyan",
    ))


vte_utf8_validation = command(
    name="vte_utf8_catalog",
    inputs=[
        "$(S)/tst/vte/utf8_cases.py",
        "$(S)/tst/vte/utf8_file_names.txt",
        "$(S)/tst/vte/utf8_validate.py",
    ],
    outputs=["$(B)/tst/vte/utf8/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/utf8_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/utf8/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VU",
    color="cyan",
)


vte_width_cases = (vte_root / "width_file_names.txt").read_text().split()
vte_width_tests = []
for case in vte_width_cases:
    vte_width_tests.append(command(
        name="vte_width_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vte/width_adapter.py",
            "$(S)/tst/vte/width_catalog.py",
            "$(S)/tst/vte/width_file_names.txt",
            "$(S)/tst/vte/width_xfail.txt",
            "$(S)/tst/vte/upstream/unicode-width-test.cc",
        ],
        outputs=[f"$(B)/tst/vte/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vte/width_adapter.py",
            case,
            "tst/vte/width_xfail.txt",
            f"$(B)/tst/vte/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VW",
        color="cyan",
    ))


vte_width_validation = command(
    name="vte_width_catalog",
    inputs=[
        "$(S)/tst/vte/width_catalog.py",
        "$(S)/tst/vte/width_file_names.txt",
        "$(S)/tst/vte/width_validate.py",
        "$(S)/tst/vte/width_xfail.txt",
        "$(S)/tst/vte/upstream/unicode-width-test.cc",
    ],
    outputs=["$(B)/tst/vte/width/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/vte/width_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/vte/width/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="VW",
    color="cyan",
)


windows_terminal_root = Path(__file__).parent / "tst" / "windows_terminal"
windows_terminal_cases = (windows_terminal_root / "file_names.txt").read_text().split()
windows_terminal_tests = []
for case in windows_terminal_cases:
    windows_terminal_tests.append(command(
        name="windows_terminal_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/windows_terminal/adapter.py",
            "$(S)/tst/windows_terminal/catalog.py",
            "$(S)/tst/windows_terminal/file_names.txt",
            "$(S)/tst/windows_terminal/xfail.txt",
            "$(S)/tst/windows_terminal/upstream/StateMachineTest.cpp",
            "$(S)/tst/windows_terminal/upstream/OutputEngineTest.cpp",
        ],
        outputs=[f"$(B)/tst/windows_terminal/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/windows_terminal/adapter.py",
            case,
            "tst/windows_terminal/xfail.txt",
            f"$(B)/tst/windows_terminal/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WT",
        color="cyan",
    ))


windows_terminal_validation = command(
    name="windows_terminal_catalog",
    inputs=[
        "$(S)/tst/windows_terminal/catalog.py",
        "$(S)/tst/windows_terminal/file_names.txt",
        "$(S)/tst/windows_terminal/validate.py",
        "$(S)/tst/windows_terminal/xfail.txt",
        "$(S)/tst/windows_terminal/upstream/StateMachineTest.cpp",
        "$(S)/tst/windows_terminal/upstream/OutputEngineTest.cpp",
    ],
    outputs=["$(B)/tst/windows_terminal/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/windows_terminal/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/windows_terminal/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WT",
    color="cyan",
)


wezterm_root = Path(__file__).parent / "tst" / "wezterm"
wezterm_cases = (wezterm_root / "file_names.txt").read_text().split()
wezterm_tests = []
for case in wezterm_cases:
    wezterm_tests.append(command(
        name="wezterm_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/wezterm/adapter.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/file_names.txt",
            "$(S)/tst/wezterm/xfail.txt",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/adapter.py",
            case,
            "tst/wezterm/xfail.txt",
            f"$(B)/tst/wezterm/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WZ",
        color="cyan",
    ))


wezterm_validation = command(
    name="wezterm_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/file_names.txt",
        "$(S)/tst/wezterm/validate.py",
        "$(S)/tst/wezterm/xfail.txt",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WZ",
    color="cyan",
)


wezterm_screen_cases = (
    wezterm_root / "screen_file_names.txt"
).read_text().split()
wezterm_screen_tests = []
for case in wezterm_screen_cases:
    wezterm_screen_tests.append(command(
        name="wezterm_screen_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/screen_adapter.py",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/screen_file_names.txt",
            "$(S)/tst/wezterm/screen_xfail.txt",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/screen/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/screen_adapter.py",
            case,
            "tst/wezterm/screen_xfail.txt",
            f"$(B)/tst/wezterm/screen/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WS",
        color="cyan",
    ))


wezterm_screen_validation = command(
    name="wezterm_screen_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/screen_file_names.txt",
        "$(S)/tst/wezterm/screen_validate.py",
        "$(S)/tst/wezterm/screen_xfail.txt",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/screen/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/screen_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/screen/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WS",
    color="cyan",
)


wezterm_selection_cases = (
    wezterm_root / "selection_file_names.txt"
).read_text().split()
wezterm_selection_tests = []
for case in wezterm_selection_cases:
    wezterm_selection_tests.append(command(
        name="wezterm_selection_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/upstream/selection.rs",
            "$(S)/tst/wezterm/selection_adapter.py",
            "$(S)/tst/wezterm/selection_cases.py",
            "$(S)/tst/wezterm/selection_file_names.txt",
            "$(S)/tst/wezterm/selection_xfail.txt",
        ],
        outputs=[f"$(B)/tst/wezterm/selection/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/selection_adapter.py",
            case,
            "tst/wezterm/selection_xfail.txt",
            f"$(B)/tst/wezterm/selection/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WQ",
        color="cyan",
    ))

wezterm_selection_validation = command(
    name="wezterm_selection_catalog",
    inputs=[
        "$(S)/tst/wezterm/upstream/selection.rs",
        "$(S)/tst/wezterm/selection_cases.py",
        "$(S)/tst/wezterm/selection_file_names.txt",
        "$(S)/tst/wezterm/selection_xfail.txt",
        "$(S)/tst/wezterm/selection_validate.py",
    ],
    outputs=["$(B)/tst/wezterm/selection/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/selection_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/selection/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WQ",
    color="cyan",
)


wezterm_cursor_cases = (
    wezterm_root / "cursor_file_names.txt"
).read_text().split()
wezterm_cursor_tests = []
for case in wezterm_cursor_cases:
    wezterm_cursor_tests.append(command(
        name="wezterm_cursor_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/cursor_adapter.py",
            "$(S)/tst/wezterm/cursor_cases.py",
            "$(S)/tst/wezterm/cursor_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
        ],
        outputs=[f"$(B)/tst/wezterm/cursor/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/cursor_adapter.py",
            case,
            f"$(B)/tst/wezterm/cursor/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WC",
        color="cyan",
    ))

wezterm_cursor_validation = command(
    name="wezterm_cursor_catalog",
    inputs=[
        "$(S)/tst/wezterm/cursor_cases.py",
        "$(S)/tst/wezterm/cursor_file_names.txt",
        "$(S)/tst/wezterm/cursor_validate.py",
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        *build.glob("$(S)/tst/wezterm/upstream/*.rs"),
    ],
    outputs=["$(B)/tst/wezterm/cursor/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/cursor_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/cursor/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WC",
    color="cyan",
)


wezterm_damage_cases = (
    wezterm_root / "damage_file_names.txt"
).read_text().split()
wezterm_damage_tests = []
for case in wezterm_damage_cases:
    wezterm_damage_tests.append(command(
        name="wezterm_damage_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/damage_adapter.py",
            "$(S)/tst/wezterm/damage_cases.py",
            "$(S)/tst/wezterm/damage_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/damage/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/damage_adapter.py",
            case,
            f"$(B)/tst/wezterm/damage/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WD",
        color="cyan",
    ))

wezterm_damage_validation = command(
    name="wezterm_damage_catalog",
    inputs=[
        "$(S)/tst/wezterm/damage_cases.py",
        "$(S)/tst/wezterm/damage_file_names.txt",
        "$(S)/tst/wezterm/damage_validate.py",
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/damage/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/damage_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/damage/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WD",
    color="cyan",
)


wezterm_history_cases = (
    wezterm_root / "history_file_names.txt"
).read_text().split()
wezterm_history_tests = []
for case in wezterm_history_cases:
    wezterm_history_tests.append(command(
        name="wezterm_history_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/catalog.py",
            "$(S)/tst/wezterm/history_adapter.py",
            "$(S)/tst/wezterm/history_cases.py",
            "$(S)/tst/wezterm/history_file_names.txt",
            "$(S)/tst/wezterm/screen_catalog.py",
            "$(S)/tst/wezterm/upstream/csi.rs",
            "$(S)/tst/wezterm/upstream/mod.rs",
            "$(S)/tst/wezterm/upstream/selection.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/history/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/history_adapter.py",
            case,
            f"$(B)/tst/wezterm/history/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WH",
        color="cyan",
    ))

wezterm_history_validation = command(
    name="wezterm_history_catalog",
    inputs=[
        "$(S)/tst/wezterm/catalog.py",
        "$(S)/tst/wezterm/history_cases.py",
        "$(S)/tst/wezterm/history_file_names.txt",
        "$(S)/tst/wezterm/history_validate.py",
        "$(S)/tst/wezterm/screen_catalog.py",
        "$(S)/tst/wezterm/upstream/csi.rs",
        "$(S)/tst/wezterm/upstream/mod.rs",
        "$(S)/tst/wezterm/upstream/selection.rs",
    ],
    outputs=["$(B)/tst/wezterm/history/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/history_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/history/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WH",
    color="cyan",
)


wezterm_semantic_cases = (
    wezterm_root / "semantic_file_names.txt"
).read_text().split()
wezterm_semantic_tests = []
for case in wezterm_semantic_cases:
    wezterm_semantic_tests.append(command(
        name="wezterm_semantic_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/semantic_adapter.py",
            "$(S)/tst/wezterm/semantic_cases.py",
            "$(S)/tst/wezterm/semantic_file_names.txt",
            "$(S)/tst/wezterm/semantic_xfail.txt",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/semantic_adapter.py",
            case,
            "tst/wezterm/semantic_xfail.txt",
            f"$(B)/tst/wezterm/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WM",
        color="cyan",
    ))

wezterm_semantic_validation = command(
    name="wezterm_semantic_catalog",
    inputs=[
        "$(S)/tst/wezterm/semantic_cases.py",
        "$(S)/tst/wezterm/semantic_file_names.txt",
        "$(S)/tst/wezterm/semantic_validate.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/semantic/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/semantic_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WM",
    color="cyan",
)


wezterm_hyperlink_cases = (
    wezterm_root / "hyperlink_file_names.txt"
).read_text().split()
wezterm_hyperlink_tests = []
for case in wezterm_hyperlink_cases:
    wezterm_hyperlink_tests.append(command(
        name="wezterm_hyperlink_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/hyperlink_adapter.py",
            "$(S)/tst/wezterm/hyperlink_cases.py",
            "$(S)/tst/wezterm/hyperlink_file_names.txt",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/hyperlink/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/hyperlink_adapter.py",
            case,
            f"$(B)/tst/wezterm/hyperlink/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WL",
        color="cyan",
    ))

wezterm_hyperlink_validation = command(
    name="wezterm_hyperlink_catalog",
    inputs=[
        "$(S)/tst/wezterm/hyperlink_cases.py",
        "$(S)/tst/wezterm/hyperlink_file_names.txt",
        "$(S)/tst/wezterm/hyperlink_validate.py",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/hyperlink/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/hyperlink_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/hyperlink/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WL",
    color="cyan",
)


wezterm_metadata_cases = (
    wezterm_root / "metadata_file_names.txt"
).read_text().split()
wezterm_metadata_tests = []
for case in wezterm_metadata_cases:
    wezterm_metadata_tests.append(command(
        name="wezterm_metadata_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wezterm/metadata_adapter.py",
            "$(S)/tst/wezterm/metadata_cases.py",
            "$(S)/tst/wezterm/metadata_file_names.txt",
            "$(S)/tst/wezterm/upstream/csi.rs",
            "$(S)/tst/wezterm/upstream/mod.rs",
        ],
        outputs=[f"$(B)/tst/wezterm/metadata/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/wezterm/metadata_adapter.py",
            case,
            f"$(B)/tst/wezterm/metadata/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WX",
        color="cyan",
    ))

wezterm_metadata_validation = command(
    name="wezterm_metadata_catalog",
    inputs=[
        "$(S)/tst/wezterm/metadata_cases.py",
        "$(S)/tst/wezterm/metadata_file_names.txt",
        "$(S)/tst/wezterm/metadata_validate.py",
        "$(S)/tst/wezterm/upstream/csi.rs",
        "$(S)/tst/wezterm/upstream/mod.rs",
    ],
    outputs=["$(B)/tst/wezterm/metadata/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/wezterm/metadata_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/wezterm/metadata/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="WX",
    color="cyan",
)


konsole_root = Path(__file__).parent / "tst" / "konsole"
konsole_cases = (konsole_root / "file_names.txt").read_text().split()
konsole_tests = []
for case in konsole_cases:
    konsole_tests.append(command(
        name="konsole_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/konsole/adapter.py",
            "$(S)/tst/konsole/catalog.py",
            "$(S)/tst/konsole/file_names.txt",
            "$(S)/tst/konsole/xfail.txt",
            "$(S)/tst/konsole/upstream/Vt102EmulationTest.cpp",
        ],
        outputs=[f"$(B)/tst/konsole/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/adapter.py",
            case,
            "tst/konsole/xfail.txt",
            f"$(B)/tst/konsole/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KO",
        color="cyan",
    ))


konsole_validation = command(
    name="konsole_catalog",
    inputs=[
        "$(S)/tst/konsole/catalog.py",
        "$(S)/tst/konsole/file_names.txt",
        "$(S)/tst/konsole/validate.py",
        "$(S)/tst/konsole/xfail.txt",
        "$(S)/tst/konsole/upstream/Vt102EmulationTest.cpp",
    ],
    outputs=["$(B)/tst/konsole/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KO",
    color="cyan",
)


konsole_semantic_cases = (
    konsole_root / "semantic_file_names.txt"
).read_text().split()
konsole_semantic_tests = []
for case in konsole_semantic_cases:
    konsole_semantic_tests.append(command(
        name="konsole_semantic_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/semantic_adapter.py",
            "$(S)/tst/konsole/semantic_cases.py",
            "$(S)/tst/konsole/semantic_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/semantic/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/semantic_adapter.py",
            case,
            f"$(B)/tst/konsole/semantic/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KS",
        color="cyan",
    ))

konsole_semantic_validation = command(
    name="konsole_semantic_catalog",
    inputs=[
        "$(S)/tst/konsole/semantic_cases.py",
        "$(S)/tst/konsole/semantic_file_names.txt",
        "$(S)/tst/konsole/semantic_validate.py",
    ],
    outputs=["$(B)/tst/konsole/semantic/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/semantic_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/semantic/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KS",
    color="cyan",
)


konsole_vt_cases = (
    konsole_root / "vt_file_names.txt"
).read_text().split()
konsole_vt_tests = []
for case in konsole_vt_cases:
    konsole_vt_tests.append(command(
        name="konsole_vt_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/vt_adapter.py",
            "$(S)/tst/konsole/vt_cases.py",
            "$(S)/tst/konsole/vt_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/vt/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/vt_adapter.py",
            case,
            f"$(B)/tst/konsole/vt/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KV",
        color="cyan",
    ))

konsole_vt_validation = command(
    name="konsole_vt_catalog",
    inputs=[
        "$(S)/tst/konsole/vt_cases.py",
        "$(S)/tst/konsole/vt_file_names.txt",
        "$(S)/tst/konsole/vt_validate.py",
    ],
    outputs=["$(B)/tst/konsole/vt/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/vt_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/vt/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KV",
    color="cyan",
)


konsole_width_cases = (
    konsole_root / "width_file_names.txt"
).read_text().split()
konsole_width_tests = []
for case in konsole_width_cases:
    konsole_width_tests.append(command(
        name="konsole_width_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/CharacterWidthTest.cpp",
            "$(S)/tst/konsole/width_adapter.py",
            "$(S)/tst/konsole/width_catalog.py",
            "$(S)/tst/konsole/width_file_names.txt",
            "$(S)/tst/ucd.py",
            "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
            "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
        ],
        outputs=[f"$(B)/tst/konsole/width/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/width_adapter.py",
            case,
            f"$(B)/tst/konsole/width/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KW",
        color="cyan",
    ))

konsole_width_validation = command(
    name="konsole_width_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/CharacterWidthTest.cpp",
        "$(S)/tst/konsole/width_catalog.py",
        "$(S)/tst/konsole/width_file_names.txt",
        "$(S)/tst/konsole/width_validate.py",
        "$(S)/tst/ucd.py",
        "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
        "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
    ],
    outputs=["$(B)/tst/konsole/width/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/width_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/width/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KW",
    color="cyan",
)


konsole_keyboard_cases = (
    konsole_root / "keyboard_file_names.txt"
).read_text().split()
konsole_keyboard_tests = []
for case in konsole_keyboard_cases:
    konsole_keyboard_tests.append(command(
        name="konsole_keyboard_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/KeyboardTranslatorTest.cpp",
            "$(S)/tst/konsole/keyboard_adapter.py",
            "$(S)/tst/konsole/keyboard_catalog.py",
            "$(S)/tst/konsole/keyboard_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/keyboard/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/keyboard_adapter.py",
            case,
            f"$(B)/tst/konsole/keyboard/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KK",
        color="cyan",
    ))

konsole_keyboard_validation = command(
    name="konsole_keyboard_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/KeyboardTranslatorTest.cpp",
        "$(S)/tst/konsole/keyboard_catalog.py",
        "$(S)/tst/konsole/keyboard_file_names.txt",
        "$(S)/tst/konsole/keyboard_validate.py",
    ],
    outputs=["$(B)/tst/konsole/keyboard/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/keyboard_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/keyboard/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KK",
    color="cyan",
)


konsole_pty_cases = (
    konsole_root / "pty_file_names.txt"
).read_text().split()
konsole_pty_tests = []
for case in konsole_pty_cases:
    konsole_pty_tests.append(command(
        name="konsole_pty_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/konsole/upstream/PtyTest.cpp",
            "$(S)/tst/konsole/pty_adapter.py",
            "$(S)/tst/konsole/pty_catalog.py",
            "$(S)/tst/konsole/pty_file_names.txt",
        ],
        outputs=[f"$(B)/tst/konsole/pty/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/konsole/pty_adapter.py",
            case,
            f"$(B)/tst/konsole/pty/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="KP",
        color="cyan",
    ))

konsole_pty_validation = command(
    name="konsole_pty_catalog",
    inputs=[
        "$(S)/tst/konsole/upstream/PtyTest.cpp",
        "$(S)/tst/konsole/pty_catalog.py",
        "$(S)/tst/konsole/pty_file_names.txt",
        "$(S)/tst/konsole/pty_validate.py",
    ],
    outputs=["$(B)/tst/konsole/pty/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/konsole/pty_validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/konsole/pty/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="KP",
    color="cyan",
)


tmux_root = Path(__file__).parent / "tst" / "tmux"
tmux_corpus_members = [
    "corpus/" + path.name
    for path in sorted((tmux_root / "corpus").iterdir())
]
tmux_dictionary_members = [
    f"dictionary/{index:03d}"
    for index, line in enumerate(
        (tmux_root / "upstream" / "input-fuzzer.dict").read_text().splitlines()
    )
    if line.strip() and not line.startswith("#")
]
tmux_members = tmux_corpus_members + tmux_dictionary_members
tmux_xfails = {
    line.strip()
    for line in (tmux_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_tmux_xfails = tmux_xfails - set(tmux_members)
if unknown_tmux_xfails:
    raise RuntimeError(
        "unknown tmux XFAIL members: "
        + ", ".join(sorted(unknown_tmux_xfails))
    )

tmux_tests = []
tmux_shard_size = 128
for shard_index, start in enumerate(
    range(0, len(tmux_corpus_members), tmux_shard_size)
):
    shard = tmux_corpus_members[start : start + tmux_shard_size]
    name = f"input_corpus_{shard_index:03d}"
    tmux_tests.append(command(
        name="tmux_" + name,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/tmux/adapter.py",
            "$(S)/tst/tmux/xfail.txt",
            *("$(S)/tst/tmux/" + member for member in shard),
        ],
        outputs=[f"$(B)/tst/tmux/{name}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/tmux/adapter.py",
            "tst/tmux/xfail.txt",
            f"$(B)/tst/tmux/{name}.stamp",
            *shard,
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TM",
        color="cyan",
    ))

for member in tmux_dictionary_members:
    index = member.split("/", 1)[1]
    tmux_tests.append(command(
        name="tmux_input_dictionary_" + index,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/fuzz_parser.py",
            "$(S)/tst/tmux/adapter.py",
            "$(S)/tst/tmux/xfail.txt",
            "$(S)/tst/tmux/upstream/input-fuzzer.dict",
        ],
        outputs=[f"$(B)/tst/tmux/input_dictionary_{index}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/tmux/adapter.py",
            "tst/tmux/xfail.txt",
            f"$(B)/tst/tmux/input_dictionary_{index}.stamp",
            member,
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TM",
        color="cyan",
    ))


wraptest_helper = program(
    name="wraptest_helper",
    srcs=["$(S)/tst/wraptest/wraptest.c"],
    cflags=["-Wno-error"],
    output="$(B)/tst/wraptest/wraptest",
)

wraptest_root = Path(__file__).parent / "tst" / "wraptest"
wraptest_cases = json.loads((wraptest_root / "cases.json").read_text())
wraptest_tests = []
for case_id, _, _ in wraptest_cases:
    wraptest_tests.append(command(
        name="wraptest_" + case_id,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/wraptest/adapter.py",
            "$(S)/tst/wraptest/cases.json",
            "$(S)/tst/wraptest/xfail.txt",
        ],
        outputs=[f"$(B)/tst/wraptest/{case_id}.stamp"],
        deps=[st_test, wraptest_helper],
        cmd=[
            sys.executable,
            "tst/wraptest/adapter.py",
            "$(B)/tst/wraptest/wraptest",
            case_id,
            "tst/wraptest/xfail.txt",
            f"$(B)/tst/wraptest/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="WR",
        color="cyan",
    ))


tack_root = Path(__file__).parent / "tst" / "tack"
tack_cases = (tack_root / "file_names.txt").read_text().split()
tack_xfails = {
    line.strip()
    for line in (tack_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_tack_xfails = tack_xfails - set(tack_cases)
if unknown_tack_xfails:
    raise RuntimeError(
        "unknown tack XFAIL capabilities: "
        + ", ".join(sorted(unknown_tack_xfails))
    )
tack_upstream_inputs = [
    *build.glob("$(S)/tst/tack/upstream/*"),
    "$(S)/tst/tack/upstream/.clang-format",
]
tack_program = command(
    name="tack_program",
    inputs=[
        "$(S)/tst/tack/build_tack.sh",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tst/tack/tack"],
    cmd=[
        "sh",
        "tst/tack/build_tack.sh",
        "tst/tack/upstream",
        "$(B)/tst/tack/tack",
    ],
    cwd="$(S)",
    descr="TC",
    color="magenta",
)
tack_validation = command(
    name="tack_catalog",
    inputs=[
        "$(S)/tst/tack/file_names.txt",
        "$(S)/tst/tack/validate.py",
        *tack_upstream_inputs,
    ],
    outputs=["$(B)/tst/tack/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/tack/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/tack/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="TA",
    color="cyan",
)
tack_tests = []
for capability in tack_cases:
    tack_tests.append(command(
        name="tack_" + capability,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/tack/adapter.py",
            "$(S)/tst/tack/file_names.txt",
            "$(S)/tst/tack/xfail.txt",
        ],
        outputs=[f"$(B)/tst/tack/{capability}.stamp"],
        deps=[st_test, tack_program],
        cmd=[
            sys.executable,
            "tst/tack/adapter.py",
            "$(B)/tst/tack/tack",
            capability,
            "tst/tack/xfail.txt",
            f"$(B)/tst/tack/{capability}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TA",
        color="cyan",
    ))


ucs_detect_root = Path(__file__).parent / "tst" / "ucs_detect"
ucs_detect_tests = []
ucs_detect_table_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted(ucs_detect_root.glob("table_*.py"))
]
# The catalog derives the skipped visible format controls from the vendored
# UCD through tst/ucd.py, so every case list depends on these files.
ucs_detect_table_inputs += [
    "$(S)/tst/ucd.py",
    "$(S)/ext/unicode/DerivedGeneralCategory-17.0.0.txt",
    "$(S)/ext/unicode/DerivedCoreProperties-17.0.0.txt",
]
ucs_detect_shards = [
    (category, int(start), int(end))
    for category, start, end in (
        line.split() for line in
        (ucs_detect_root / "shards.txt").read_text().splitlines()
    )
]
ucs_detect_category_indices = {}
for category, start, end in ucs_detect_shards:
    shard_index = ucs_detect_category_indices.get(category, 0)
    ucs_detect_category_indices[category] = shard_index + 1
    name = f"{category}_{shard_index:03d}"
    ucs_detect_tests.append(command(
        name="ucs_detect_" + name,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/ucs_detect/adapter.py",
            "$(S)/tst/ucs_detect/catalog.py",
            "$(S)/tst/ucs_detect/shards.txt",
            "$(S)/tst/ucs_detect/xfail.txt",
            *ucs_detect_table_inputs,
        ],
        outputs=[f"$(B)/tst/ucs_detect/{name}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/ucs_detect/adapter.py",
            category,
            str(start),
            str(end),
            "tst/ucs_detect/xfail.txt",
            f"$(B)/tst/ucs_detect/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UC",
        color="cyan",
    ))

ucs_detect_validation = command(
    name="ucs_detect_catalog",
    inputs=[
        "$(S)/tst/ucs_detect/catalog.py",
        "$(S)/tst/ucs_detect/probe_cases.py",
        "$(S)/tst/ucs_detect/probe_names.txt",
        "$(S)/tst/ucs_detect/probe_xfail.txt",
        "$(S)/tst/ucs_detect/validate.py",
        "$(S)/tst/ucs_detect/shards.txt",
        "$(S)/tst/ucs_detect/xfail.txt",
        *ucs_detect_table_inputs,
    ],
    outputs=["$(B)/tst/ucs_detect/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/ucs_detect/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/ucs_detect/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="UC",
    color="cyan",
)


ucs_detect_probe_cases = (ucs_detect_root / "probe_names.txt").read_text().split()
ucs_detect_probe_tests = []
for case in ucs_detect_probe_cases:
    ucs_detect_probe_tests.append(command(
        name="ucs_detect_probe_" + case.lower(),
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/ucs_detect/probe_adapter.py",
            "$(S)/tst/ucs_detect/probe_cases.py",
            "$(S)/tst/ucs_detect/probe_names.txt",
            "$(S)/tst/ucs_detect/probe_xfail.txt",
            "$(S)/tst/ucs_detect/upstream/terminal.py",
            "$(S)/tst/ucs_detect/upstream/table_xtgettcap.py",
            "$(S)/tst/ucs_detect/upstream/data/shitty.yaml",
        ],
        outputs=[f"$(B)/tst/ucs_detect/probes/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/ucs_detect/probe_adapter.py",
            case,
            "tst/ucs_detect/probe_xfail.txt",
            f"$(B)/tst/ucs_detect/probes/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="UP",
        color="cyan",
    ))


vtebench_root = Path(__file__).parent / "tst" / "vtebench"
vtebench_cases = (vtebench_root / "file_names.txt").read_text().split()
vtebench_tests = []
for case in vtebench_cases:
    vtebench_case_inputs = [
        "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
        for path in sorted((vtebench_root / "benchmarks" / case).iterdir())
        if path.is_file()
    ]
    vtebench_tests.append(command(
        name="vtebench_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/vtebench/adapter.py",
            "$(S)/tst/vtebench/file_names.txt",
            "$(S)/tst/vtebench/xfail.txt",
            *vtebench_case_inputs,
        ],
        outputs=[f"$(B)/tst/vtebench/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/vtebench/adapter.py",
            case,
            "tst/vtebench/xfail.txt",
            f"$(B)/tst/vtebench/{case}.stamp",
            "30",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="VB",
        color="yellow",
    ))


libvterm_root = Path(__file__).parent / "tst" / "libvterm"
libvterm_cases = (libvterm_root / "file_names.txt").read_text().split()
libvterm_xfails = {
    line.strip()
    for line in (libvterm_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_libvterm_xfails = libvterm_xfails - set(libvterm_cases)
if unknown_libvterm_xfails:
    raise RuntimeError(
        "unknown libvterm XFAIL fixtures: "
        + ", ".join(sorted(unknown_libvterm_xfails))
    )
libvterm_tests = []
for case in libvterm_cases:
    name = case.removesuffix(".test").replace("-", "_")
    libvterm_tests.append(command(
        name="libvterm_" + name,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/libvterm/adapter.py",
            "$(S)/tst/libvterm/file_names.txt",
            "$(S)/tst/libvterm/xfail.txt",
            f"$(S)/tst/libvterm/upstream/{case}",
        ],
        outputs=[f"$(B)/tst/libvterm/{name}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/libvterm/adapter.py",
            case,
            "tst/libvterm/xfail.txt",
            f"$(B)/tst/libvterm/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="LV",
        color="cyan",
    ))


xterm_vttests_root = Path(__file__).parent / "tst" / "xterm_vttests"
xterm_vttests_cases = (xterm_vttests_root / "file_names.txt").read_text().split()
xterm_vttests_xfails = {
    line.strip()
    for line in (xterm_vttests_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_xterm_vttests_xfails = xterm_vttests_xfails - set(xterm_vttests_cases)
if unknown_xterm_vttests_xfails:
    raise RuntimeError(
        "unknown xterm vttests XFAIL scripts: "
        + ", ".join(sorted(unknown_xterm_vttests_xfails))
    )
xterm_vttests_tests = []
for case in xterm_vttests_cases:
    name = case.removesuffix(".sh").replace("-", "_")
    xterm_vttests_tests.append(command(
        name="xterm_vttests_" + name,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/xterm_vttests/adapter.py",
            "$(S)/tst/xterm_vttests/file_names.txt",
            "$(S)/tst/xterm_vttests/xfail.txt",
            *build.glob("$(S)/tst/xterm_vttests/bin/*"),
            *build.glob("$(S)/tst/xterm_vttests/lib/**/*.pm"),
            *build.glob("$(S)/tst/xterm_vttests/upstream/*"),
        ],
        outputs=[f"$(B)/tst/xterm_vttests/{name}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/xterm_vttests/adapter.py",
            case,
            "tst/xterm_vttests/xfail.txt",
            f"$(B)/tst/xterm_vttests/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="XV",
        color="cyan",
    ))


esctest_root = Path(__file__).parent / "tst" / "esctest"
esctest_cases = (esctest_root / "file_names.txt").read_text().split()
esctest_xfails = {
    line.strip()
    for line in (esctest_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_esctest_xfails = esctest_xfails - set(esctest_cases)
if unknown_esctest_xfails:
    raise RuntimeError(
        "unknown esctest XFAIL cases: "
        + ", ".join(sorted(unknown_esctest_xfails))
    )
esctest_ported_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted((esctest_root / "ported").rglob("*.py"))
]
esctest_tests = []
for case in esctest_cases:
    name = case.replace(".", "_")
    esctest_tests.append(command(
        name="esctest_" + name,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/esctest/adapter.py",
            "$(S)/tst/esctest/file_names.txt",
            "$(S)/tst/esctest/xfail.txt",
            *esctest_ported_inputs,
        ],
        outputs=[f"$(B)/tst/esctest/{name}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/esctest/adapter.py",
            case,
            "tst/esctest/xfail.txt",
            f"$(B)/tst/esctest/{name}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        test_timeout_seconds=120,
        descr="ES",
        color="cyan",
    ))


termless_root = Path(__file__).parent / "tst" / "termless"
termless_cases = json.loads((termless_root / "cases.json").read_text())
termless_ids = {case_id for case_id, _, _ in termless_cases}
termless_xfails = {
    line.strip()
    for line in (termless_root / "xfail.txt").read_text().splitlines()
    if line.strip() and not line.startswith("#")
}
unknown_termless_xfails = termless_xfails - termless_ids
if unknown_termless_xfails:
    raise RuntimeError(
        "unknown Termless XFAIL cases: "
        + ", ".join(sorted(unknown_termless_xfails))
    )
termless_upstream_inputs = [
    "$(S)/" + path.relative_to(Path(__file__).parent).as_posix()
    for path in sorted((termless_root / "upstream").rglob("*"))
    if path.is_file()
]
termless_validation = command(
    name="termless_catalog",
    inputs=[
        "$(S)/tst/termless/cases.json",
        "$(S)/tst/termless/validate.py",
        "$(S)/tst/termless/xfail.txt",
        *termless_upstream_inputs,
    ],
    outputs=["$(B)/tst/termless/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/termless/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/termless/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="TL",
    color="cyan",
)
termless_tests = []
for case_id, _, _ in termless_cases:
    termless_tests.append(command(
        name="termless_" + case_id,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/termless/adapter.py",
            "$(S)/tst/termless/backend.py",
            "$(S)/tst/termless/cases.py",
            "$(S)/tst/termless/cases.json",
            "$(S)/tst/termless/xfail.txt",
            *termless_upstream_inputs,
        ],
        outputs=[f"$(B)/tst/termless/{case_id}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/termless/adapter.py",
            case_id,
            "tst/termless/xfail.txt",
            f"$(B)/tst/termless/{case_id}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="TL",
        color="cyan",
    ))


realworld_root = Path(__file__).parent / "tst" / "realworld"
realworld_cases = (realworld_root / "file_names.txt").read_text().split()
realworld_validation = command(
    name="realworld_catalog",
    inputs=[
        "$(S)/tst/realworld/validate.py",
        "$(S)/tst/realworld/corpus.py",
        "$(S)/tst/realworld/zstd_codec.py",
        "$(S)/tst/realworld/cases.json",
        "$(S)/tst/realworld/file_names.txt",
        *build.glob("$(S)/tst/realworld/input/*.input.zst"),
        *build.glob("$(S)/tst/realworld/screen/*.screen.json"),
    ],
    outputs=["$(B)/tst/realworld/catalog.stamp"],
    cmd=[
        [sys.executable, "tst/realworld/validate.py"],
        [
            sys.executable, "-c",
            "from pathlib import Path; "
            "Path(r'$(B)/tst/realworld/catalog.stamp').touch()",
        ],
    ],
    cwd="$(S)",
    descr="RW",
    color="cyan",
)
realworld_tests = []
for case in realworld_cases:
    realworld_tests.append(command(
        name="realworld_" + case,
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/realworld/adapter.py",
            "$(S)/tst/realworld/corpus.py",
            "$(S)/tst/realworld/zstd_codec.py",
            "$(S)/tst/realworld/cases.json",
            "$(S)/tst/realworld/file_names.txt",
            f"$(S)/tst/realworld/input/{case}.input.zst",
            f"$(S)/tst/realworld/screen/{case}.screen.json",
        ],
        outputs=[f"$(B)/tst/realworld/{case}.stamp"],
        deps=[st_test],
        cmd=[
            sys.executable,
            "tst/realworld/adapter.py",
            case,
            f"$(B)/tst/realworld/{case}.stamp",
        ],
        cwd="$(S)",
        env={"SHITTY_TEST_BINARY": "$(B)/st_test"},
        descr="RW",
        color="cyan",
    ))

# The Cartesian key-encoding matrix is ~6500 cases; ten shards keep
# each node inside the ordinary test timeout, split by a stable hash
# of the case id.
keyboard_product_group_count = 10
keyboard_product_tests = []
for group_index in range(keyboard_product_group_count):
    output = f"$(B)/keyboard-product/group-{group_index:02}.stamp"
    keyboard_product_tests.append(command(
        name=f"keyboard_product_group_{group_index:02}",
        inputs=[
            "$(S)/tst/harness.py",
            "$(S)/tst/keyboard_layout_product.py",
        ],
        outputs=[output],
        deps=[st_test],
        cmd=[
            [
                sys.executable,
                "tst/keyboard_layout_product.py",
                f"--group={group_index}",
                f"--group-count={keyboard_product_group_count}",
            ],
            touch_stamp(output),
        ],
        cwd="$(S)",
        env={
            "SHITTY_TEST_BINARY": "$(B)/st_test",
            "SHITTY_TEST_FONTCONFIG": "1" if fontconfig else "0",
            "SHITTY_TEST_PLATFORM": "cocoa" if darwin else "wayland",
            "SHITTY_TEST_VERSION": shitty_version,
        },
        descr="KB",
        color="cyan",
    ))


group("install", st, pt)


# The lib/vterm boundary: the core includes nothing from lib/shitty.
vterm_boundary = command(
    name="vterm_boundary",
    inputs=[
        "$(S)/lib/vterm/check_includes.py",
        *build.glob("$(S)/lib/vterm/*.h"),
        *build.glob("$(S)/lib/vterm/*.cpp"),
    ],
    outputs=["$(B)/vterm-boundary.stamp"],
    cmd=[
        sys.executable,
        "$(S)/lib/vterm/check_includes.py",
        "$(S)/lib/vterm",
        "$(B)/vterm-boundary.stamp",
    ],
    descr="VB",
    color="magenta",
)

add_test(production_surface, pretty_binary_branding, vterm_boundary, instrumented=False)
if linux:
    add_test(wayland_frame_stall, wayland_title_fallback, instrumented=False)

add_test(
    *([plt_tests] if plt_tests is not None else []),
    *unit_test_groups,
    *python_test_groups,
    *python_test_prod_parser_groups,
    parser_fuzz,
    vttest_profile,
    *xtermjs_tests,
    *alacritty_tests,
    contour_vttest,
    *contour_tests,
    *mosh_tests,
    *mosh_semantic_tests,
    mosh_semantic_validation,
    *libtsm_semantic_tests,
    libtsm_semantic_validation,
    *ghostty_tests,
    *ghostty_semantic_tests,
    ghostty_semantic_validation,
    *kitty_tests,
    kitty_validation,
    *kitty_screen_tests,
    kitty_screen_validation,
    kitty_utf8,
    *kitty_transaction_tests,
    kitty_transaction_validation,
    *vte_tests,
    vte_validation,
    *vte_known_tests,
    vte_known_validation,
    *vte_charset_tests,
    vte_charset_validation,
    *vte_tabstop_tests,
    vte_tabstop_validation,
    *vte_mode_tests,
    vte_mode_validation,
    *vte_color_tests,
    vte_color_validation,
    *vte_paste_tests,
    vte_paste_validation,
    *vte_utf8_tests,
    vte_utf8_validation,
    *vte_width_tests,
    vte_width_validation,
    *windows_terminal_tests,
    windows_terminal_validation,
    *wezterm_tests,
    wezterm_validation,
    *wezterm_screen_tests,
    wezterm_screen_validation,
    *wezterm_selection_tests,
    wezterm_selection_validation,
    *wezterm_cursor_tests,
    wezterm_cursor_validation,
    *wezterm_damage_tests,
    wezterm_damage_validation,
    *wezterm_history_tests,
    wezterm_history_validation,
    *wezterm_semantic_tests,
    wezterm_semantic_validation,
    *wezterm_hyperlink_tests,
    wezterm_hyperlink_validation,
    *wezterm_metadata_tests,
    wezterm_metadata_validation,
    *konsole_tests,
    konsole_validation,
    *konsole_semantic_tests,
    konsole_semantic_validation,
    *konsole_vt_tests,
    konsole_vt_validation,
    *konsole_width_tests,
    konsole_width_validation,
    *konsole_keyboard_tests,
    konsole_keyboard_validation,
    *konsole_pty_tests,
    konsole_pty_validation,
    *tmux_tests,
    wraptest_helper,
    *wraptest_tests,
    tack_program,
    tack_validation,
    *tack_tests,
    *ucs_detect_tests,
    ucs_detect_validation,
    *ucs_detect_probe_tests,
    *vtebench_tests,
    *libvterm_tests,
    *xterm_vttests_tests,
    *esctest_tests,
    termless_validation,
    *termless_tests,
    realworld_validation,
    *realworld_tests,
    *keyboard_product_tests,
)
