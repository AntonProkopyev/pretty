/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"

#include "pretty_icon_data.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

using namespace stl;

namespace {
    struct PrettyBrand final: public Brand {
        StringView displayName() const override;
        StringView executableName() const override;
        StringView identifier() const override;
        StringView fontSizeEnvironment() const override;
        StringView versionEnvironment() const override;
        StringView iconData() const override;
    };

    static Brand* createBrand();
}

StringView PrettyBrand::displayName() const {
    return StringView(u8"Pretty");
}

StringView PrettyBrand::executableName() const {
    return StringView(u8"pt");
}

StringView PrettyBrand::identifier() const {
    return StringView(u8"pretty");
}

StringView PrettyBrand::fontSizeEnvironment() const {
    return StringView(u8"PRETTY_FONT_SIZE");
}

StringView PrettyBrand::versionEnvironment() const {
    return StringView(u8"PRETTY_VERSION");
}

StringView PrettyBrand::iconData() const {
    return StringView((const u8*)(prettyIcon.data), prettyIcon.size);
}

namespace {
    static Brand* createBrand() {
        static PrettyBrand brand;
        return &brand;
    }
}

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    (void)(argc);
    (void)(argv);
    return runWindowsMain(*createBrand());
#else
    return runMain(*createBrand(), argc, argv);
#endif
}

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return runWindowsMain(*createBrand());
}
#endif
