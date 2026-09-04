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
