import os


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
