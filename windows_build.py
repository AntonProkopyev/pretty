import os
import sys


libstd = import_build(
    os.path.join("ext", "libstd", "build.py"),
    "libstd.a",
)
libplt = import_build(
    os.path.join("ext", "plt", "build.py"),
    "libplt_headless.a",
    extra_cppflags=[
        "-Dno_vendored_std",
        "-I$(S)/../libstd",
        "-Dplatforms=headless",
    ],
)
libplt_win32 = import_build(
    os.path.join("ext", "plt", "build.py"),
    "libplt.a",
    extra_cppflags=[
        "-Dno_vendored_std",
        "-I$(S)/../libstd",
    ],
)
libplt_win32.ldflags += [
    "-luser32",
    "-lshell32",
    "-lole32",
    "-limm32",
    "-luuid",
]

windows_headless_test = program(
    srcs=["$(S)/dev/windows/headless_test.cpp"],
    cflags=[
        "-I$(S)/ext",
        "-I$(S)/ext/libstd",
    ],
    cxxflags=["-std=c++23"],
    deps=[libplt, libstd],
    ldflags=["-static"],
)

group("windows-headless-test", windows_headless_test)

windows_win32_test = program(
    srcs=["$(S)/dev/windows/win32_test.cpp"],
    cflags=[
        "-I$(S)/ext",
        "-I$(S)/ext/libstd",
    ],
    cppflags=[
        "-DUNICODE=1",
        "-D_UNICODE=1",
        "-D_WIN32_WINNT=0x0A00",
    ],
    cxxflags=["-std=c++23"],
    deps=[libplt_win32, libstd],
    ldflags=["-static"],
)

group("windows-win32-test", windows_win32_test)

vcpkg_installed = os.environ.get("VCPKG_INSTALLED")
if vcpkg_installed:
    vcpkg_triplet = os.path.join(vcpkg_installed, "x64-mingw-dynamic")
    render_formats = [
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
    render_fragments = []
    render_targets = []
    for render_format in render_formats:
        fragment = f"$(B)/render_shader_{render_format}.inc"
        render_fragments.append(fragment)
        render_targets.append(command(
            name=f"windows_render_shader_{render_format}",
            inputs=[
                "$(S)/lib/shitty/render.comp",
                "$(S)/lib/shitty/generate_render_shaders.py",
            ],
            outputs=[fragment],
            cmd=[
                sys.executable,
                "$(S)/lib/shitty/generate_render_shaders.py",
                "compile",
                "$(S)/lib/shitty/render.comp",
                render_format,
                fragment,
                "glslangValidator",
            ],
            descr="SH",
            color="magenta",
        ))
    render_spv = command(
        name="windows_render_spv",
        inputs=["$(S)/lib/shitty/generate_render_shaders.py", *render_fragments],
        outputs=["$(B)/render_spv.h"],
        deps=render_targets,
        cmd=[
            sys.executable,
            "$(S)/lib/shitty/generate_render_shaders.py",
            "combine",
            "$(B)/render_spv.h",
            *render_fragments,
        ],
        descr="SH",
        color="magenta",
    )
    windows_renderer_probe = library(
        name="windows_renderer_probe",
        srcs=[{
            "src": "$(S)/lib/shitty/render_vk.cpp",
            "inputs": ["$(B)/render_spv.h"],
        }],
        cflags=[
            "-I$(S)",
            "-I$(S)/ext",
            "-I$(S)/ext/libstd",
            "-I$(S)/lib/shitty",
            "-I$(B)",
            f"-I{vcpkg_triplet}/include",
        ],
        cppflags=[
            "-DHAVE_VULKAN_RENDERER=1",
            "-DHAVE_VULKAN_WIN32=1",
        ],
        cxxflags=["-std=c++26"],
        deps=[render_spv],
    )
    windows_vulkan_test = program(
        srcs=["$(S)/dev/windows/vulkan_test.cpp"],
        cflags=[f"-I{vcpkg_triplet}/include"],
        cxxflags=["-std=c++23"],
        ldflags=[
            "-static",
            os.path.join(vcpkg_triplet, "lib", "libvulkan-1.dll.a"),
            "-luser32",
        ],
    )
    windows_font_backend_probe = library(
        name="windows_font_backend_probe",
        srcs=[
            "$(S)/lib/shitty/font_face.cpp",
            "$(S)/lib/shitty/font_windows.cpp",
        ],
        cflags=[
            "-I$(S)",
            "-I$(S)/ext",
            "-I$(S)/ext/libstd",
            "-I$(S)/lib/shitty",
        ],
        cxxflags=["-std=c++26"],
    )
    windows_font_test = program(
        srcs=[
            "$(S)/dev/windows/font_test.cpp",
            "$(S)/lib/shitty/font_face.cpp",
        ],
        cflags=[
            "-I$(S)",
            "-I$(S)/ext",
            "-I$(S)/ext/libstd",
        ],
        cxxflags=["-std=c++26"],
        deps=[libstd],
        ldflags=[
            "-static",
            "-ldwrite",
            "-lole32",
            "-luuid",
        ],
    )
    group("windows-vulkan-test", windows_renderer_probe)
    group("windows-vulkan-test", windows_vulkan_test)
    group("windows-font-test", windows_font_backend_probe)
    group("windows-font-test", windows_font_test)
