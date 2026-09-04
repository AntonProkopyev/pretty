/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>

struct Brand {
    virtual stl::StringView displayName() const = 0;
    virtual stl::StringView executableName() const = 0;
    virtual stl::StringView identifier() const = 0;
    virtual stl::StringView fontSizeEnvironment() const = 0;
    virtual stl::StringView versionEnvironment() const = 0;
    virtual stl::StringView iconData() const = 0;

    const char* identifierCString() const;
    void configureVersionEnvironment() const;

    // Headless/unit-test composers use a neutral brand unless their adapter
    // supplies the production brand explicitly.
    static Brand* generic();
};

int runMain(Brand& brand, int argc, char* argv[]);
#if defined(_WIN32)
int runWindowsMain(Brand& brand);
#endif
