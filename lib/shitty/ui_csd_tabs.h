/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

namespace stl {
    class ObjPool;
}

struct Composer;

// macOS-only chrome: the session tabs drawn in the window's own title
// bar, the iTerm2 look. Creates a self-contained pool object that
// projects the SessionSet tab model from sessionsChangedListeners onto
// an AppKit view over the NSWindow the render context carries, and
// drives SessionSet from clicks. Nothing calls the object afterwards;
// off macOS nothing defines this.
void createCsdTabsUi(stl::ObjPool& owner, Composer& composer);

// Windows chrome uses the same session model in a custom non-client strip.
void createCsdTabsWin32Ui(stl::ObjPool& owner, Composer& composer);
