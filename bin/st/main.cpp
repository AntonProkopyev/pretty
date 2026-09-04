/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"

#include "shitty_icon_data.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

using namespace stl;

namespace {
    struct ShittyBrand final: public Brand {
        StringView displayName() const override;
        StringView executableName() const override;
        StringView identifier() const override;
        StringView fontSizeEnvironment() const override;
        StringView versionEnvironment() const override;
        StringView iconData() const override;
    };

    static Brand* createBrand();
}

StringView ShittyBrand::displayName() const {
    return StringView(u8"Shitty");
}

StringView ShittyBrand::executableName() const {
    return StringView(u8"st");
}

StringView ShittyBrand::identifier() const {
    return StringView(u8"shitty");
}

StringView ShittyBrand::fontSizeEnvironment() const {
    return StringView(u8"SHITTY_FONT_SIZE");
}

StringView ShittyBrand::versionEnvironment() const {
    return StringView(u8"SHITTY_VERSION");
}

StringView ShittyBrand::iconData() const {
    return StringView((const u8*)(shittyIcon.data), shittyIcon.size);
}

namespace {
    static Brand* createBrand() {
        static ShittyBrand brand;
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
