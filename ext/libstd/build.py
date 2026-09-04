import os
import build


build.cxxflags += [
    "-W",
    "-Wall",
    "-std=c++26",
    "-O2",
    "-g",
    "-fno-omit-frame-pointer",
    "-mno-omit-leaf-frame-pointer",
]
if build.target.startswith("x86_64"):
    build.cxxflags += ["-mcx16"]
std_sources = sorted(build.glob("$(S)/std/*/*.cpp"))
unit_sources = [source for source in std_sources if source.endswith("_ut.cpp")]
windows = "windows" in build.target
windows_library_sources = [
    "$(S)/std/alg/exchange.cpp",
    "$(S)/std/alg/xchg.cpp",
    "$(S)/std/dbg/assert.cpp",
    "$(S)/std/dbg/insist.cpp",
    "$(S)/std/dbg/panic.cpp",
    "$(S)/std/ios/in_zc.cpp",
    "$(S)/std/ios/input.cpp",
    "$(S)/std/ios/out_buf.cpp",
    "$(S)/std/ios/out_zc.cpp",
    "$(S)/std/ios/output.cpp",
    "$(S)/std/ios/unbound.cpp",
    "$(S)/std/lib/buffer.cpp",
    "$(S)/std/lib/list.cpp",
    "$(S)/std/lib/node.cpp",
    "$(S)/std/lib/vector.cpp",
    "$(S)/std/map/treap.cpp",
    "$(S)/std/map/treap_node.cpp",
    "$(S)/std/mem/disposable.cpp",
    "$(S)/std/mem/disposer.cpp",
    "$(S)/std/mem/free_list.cpp",
    "$(S)/std/mem/mem_pool.cpp",
    "$(S)/std/mem/new.cpp",
    "$(S)/std/mem/obj_pool.cpp",
    "$(S)/std/mem/small_obj_allocator.cpp",
    "$(S)/std/ptr/arc.cpp",
    "$(S)/std/rng/split_mix_64.cpp",
    "$(S)/std/str/builder.cpp",
    "$(S)/std/str/fmt.cpp",
    "$(S)/std/str/hash.cpp",
    "$(S)/std/str/view.cpp",
    "$(S)/std/sys/crt.cpp",
    "$(S)/std/sys/throw.cpp",
    "$(S)/std/sys/types.cpp",
    "$(S)/std/thr/context.cpp",
    "$(S)/std/thr/runable.cpp",
]
library_sources = (
    windows_library_sources
    if windows
    else [source for source in std_sources if not source.endswith("_ut.cpp")]
)

external_monotonic_clock = "-DSTL_EXTERNAL_MONOTONIC_NOW_US=1" in build.cppflags
position_independent = "-fPIC" in build.cflags
libstd_name = (
    "libstd_external_clock" if external_monotonic_clock
    else "libstd_pic" if position_independent
    else "libstd"
)

libstd = library(
    name=libstd_name,
    srcs=library_sources,
    output=f"$(B)/{libstd_name}.a",
)

if windows:
    group("windows-headless", libstd)
    install(libstd)
else:
    test_sources = sorted(build.glob("$(S)/tst/*.cpp"))
    test_main = "$(S)/tst/test.cpp"
    test = program(
        srcs=[test_main, *unit_sources],
        output="$(B)/tst/test",
        deps=[libstd],
    )

    test_binaries = []
    for source in test_sources:
        if source == test_main:
            continue
        name = os.path.basename(source).removesuffix(".cpp")
        test_binaries.append(program(
            name=name,
            srcs=[source],
            output=f"$(B)/tst/{name}",
            deps=[libstd],
        ))

    install(libstd, test, *test_binaries)
