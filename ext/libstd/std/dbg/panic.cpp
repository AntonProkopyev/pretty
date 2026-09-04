#include "panic.h"

#include <std/sys/crt.h>
#include <std/alg/exchange.h>

#include <stdlib.h>

#if defined(_WIN32)
    #include <stdio.h>
#else
    #include "color.h"
    #include <std/ios/sys.h>
    #include <std/str/view.h>
    #include <std/ios/output.h>
#endif

using namespace stl;

namespace {
    static void emptyFunc() {
    }

    static PanicHandler panicHandler1 = (PanicHandler)emptyFunc;
    static PanicHandler panicHandler2 = (PanicHandler)abort;
}

PanicHandler stl::setPanicHandler1(PanicHandler hndl) noexcept {
    return exchange(panicHandler1, hndl);
}

PanicHandler stl::setPanicHandler2(PanicHandler hndl) noexcept {
    return exchange(panicHandler2, hndl);
}

void stl::panic(const u8* what, u32 line, const u8* file) {
    panicHandler1();

#if defined(_WIN32)
    fwrite(what, 1, strLen(what), stderr);
    fputs(" failed, at ", stderr);
    fwrite(file, 1, strLen(file), stderr);
    fprintf(stderr, ":%u\n", line);
    fflush(stderr);
#else
    sysE << Color::bright(AnsiColor::Red)
         << StringView(what, strLen(what))
         << StringView(u8" failed, at ")
         << StringView(file, strLen(file))
         << StringView(u8":")
         << line
         << Color::reset()
         << endL
         << finI;
#endif

    panicHandler2();
}
