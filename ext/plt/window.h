#pragma once

#include "clipboard.h"

#include <std/str/view.h>
#include <std/sys/types.h>

namespace plt {
    struct DropTarget;
    struct InputSink;

    enum class RenderBackend : u8 {
        Wayland,
        Cocoa,
        Win32,
        Headless
    };

    struct RenderContext {
        RenderBackend backend;
        void* connection;
        void* window;
    };

    // The union of the wp_cursor_shape_device_v1 shapes and the public
    // NSCursor cursors, collapsed where both platforms mean the same thing
    // (pointer covers pointingHandCursor, grab covers openHandCursor, and so
    // on). A backend without a native cursor for a value substitutes the
    // closest one it has.
    enum class PointerIcon : u8 {
        Default,
        ContextMenu,
        Help,
        Pointer,
        Progress,
        Wait,
        Cell,
        Crosshair,
        Text,
        VerticalText,
        Alias,
        Copy,
        Move,
        NoDrop,
        NotAllowed,
        Grab,
        Grabbing,
        ResizeEast,
        ResizeNorth,
        ResizeNorthEast,
        ResizeNorthWest,
        ResizeSouth,
        ResizeSouthEast,
        ResizeSouthWest,
        ResizeWest,
        ResizeEastWest,
        ResizeNorthSouth,
        ResizeNorthEastSouthWest,
        ResizeNorthWestSouthEast,
        ResizeColumn,
        ResizeRow,
        AllScroll,
        ZoomIn,
        ZoomOut,
        DndAsk,
        ResizeAll,
        DisappearingItem
    };

    struct WindowInfo {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u32 screenPixelWidth = 0;
        u32 screenPixelHeight = 0;
        float contentScale = 1.0f;
        bool focused = false;
        bool iconified = false;
        bool maximized = false;
        bool fullscreen = false;
        bool tiled = false;
    };

    struct WindowEvents {
        virtual void close() = 0;
    };

    struct FrameCallback {
        // Returns true when a frame was submitted for presentation.
        virtual bool frame(const WindowInfo& info) = 0;
    };

    struct WindowColor {
        u8 red = 0;
        u8 green = 0;
        u8 blue = 0;
    };

    struct WindowTabs {
        virtual size_t count() const = 0;
        virtual size_t active() const = 0;
        virtual stl::StringView title(size_t index) const = 0;
        virtual WindowColor background() const = 0;
        virtual WindowColor foreground() const = 0;
        virtual void select(size_t index) = 0;
        virtual void close(size_t index) = 0;
        virtual void open() = 0;
    };

    struct WindowOptions {
        stl::StringView appId = {};
        stl::StringView title = {};
        u32 width = 800;
        u32 height = 600;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        bool decorations = true;
        bool globalToggleHotkey = false;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        // Null leaves the window rejecting every drag.
        DropTarget* drop = nullptr;
        // Encoded image bytes (PNG) for the application icon; empty keeps
        // the platform default. Cocoa sets the Dock icon from it, Wayland
        // has no icon protocol and ignores it.
        stl::StringView icon = {};
        // The human-visible application name. Cocoa pushes it to Launch
        // Services so the menu bar of an unbundled binary shows it
        // instead of argv[0]; Wayland ignores it (appId serves the
        // shell). The Cmd-Tab switcher is beyond reach: its label comes
        // from the application bundle, which a bare executable lacks.
        stl::StringView appName = {};
    };

    struct Window {
        virtual void requestShow() = 0;
        virtual void requestClose() = 0;
        virtual void requestFrame() = 0;

        virtual void requestTitle(stl::StringView title) = 0;
        virtual void requestAttention() = 0;
        virtual void requestRestore() = 0;
        virtual void requestIconify() = 0;
        virtual void requestMove(i32 x, i32 y) = 0;
        virtual void requestFocus() = 0;
        virtual void requestMaximized(bool maximized) = 0;
        virtual void requestFullscreen(bool fullscreen) = 0;
        virtual void requestResize(u32 width, u32 height) = 0;
        virtual void requestMinimumSize(u32 width, u32 height) = 0;
        virtual void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) = 0;

        // The primary selection. On macOS it maps to the Find pasteboard: the
        // platform has no primary selection, and the Find pasteboard is the
        // closest persistent per-application slot. Reads may therefore observe
        // search-field text.
        virtual Clipboard* primary() = 0;
        // The regular clipboard.
        virtual Clipboard* secondary() = 0;
        virtual void requestPointerIcon(PointerIcon icon) = 0;
        // Opens uri with the desktop's default handler for its scheme. The
        // launch is fire-and-forget: failures surface only in the desktop
        // environment, never back to the caller.
        virtual void requestOpenUri(stl::StringView uri) = 0;
        // Caret rectangle in surface pixels. Input methods position their
        // candidate window next to it (text-input-v3 cursor rectangle on
        // Wayland, firstRectForCharacterRange on macOS).
        virtual void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) = 0;
        virtual void requestTabs(WindowTabs*) {}
        virtual void requestTabsRedraw() {}

        virtual WindowInfo info() const = 0;
        // True while the user is interactively resizing the window; a
        // renderer presents transaction-synchronously then and stays
        // asynchronous otherwise.
        virtual bool inLiveResize() const = 0;
        virtual RenderContext renderContext() const = 0;
    };
}
