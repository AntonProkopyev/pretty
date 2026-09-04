/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/input_handler.h>

#include <std/sys/types.h>

namespace stl {
    class IntrusiveList;
}

struct Composer;

enum class InputActions : u8 {
    Copy,
    Paste,
    PastePrimary,
    PageUp,
    PageDown,
    IncFontSize,
    DecFontSize,
    ResetFontSize,
    ReloadConfig,
    NewTab,
    CloseTab,
    PrevTab,
    NextTab,
    // Direct tab selection, iTerm style: the ninth chord jumps to the
    // last tab however many there are. Contiguous, so ordinal arithmetic
    // against SelectTab1 is safe.
    SelectTab1,
    SelectTab2,
    SelectTab3,
    SelectTab4,
    SelectTab5,
    SelectTab6,
    SelectTab7,
    SelectTab8,
    SelectTab9,
    Clear,
    // The natural-editing gestures: what the chord means, sent to the
    // shell as the readline bytes the platform's editors agree on.
    WordLeft,
    WordRight,
    LineStart,
    LineEnd,
    KillLine,
    EraseWord,
    Count,
};

struct InputBindings: public InputHandler {
    virtual void add(InputActions action, stl::IntrusiveList* listeners) = 0;

    static InputBindings* create(Composer& composer);
};
