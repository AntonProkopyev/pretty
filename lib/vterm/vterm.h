/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "vt_pointer.h"

#include <lib/vterm/rect.h>
#include <lib/vterm/terminal_types.h>

#include <std/str/view.h>
#include <std/sys/types.h>

#include <stddef.h>
#include <stdint.h>
#include <plt/input.h>

namespace stl {
    class Input;
    class ObjPool;
    class SmallObjAllocator;
    class Output;
}

namespace plt {
    struct Scheduler;
}

struct VtCellExtras;
struct VtConfigSlot;
struct VtGeometry;
struct VtHost;
struct PtyHandle;
struct CellExtraStore;
struct Screen;
struct VtermTraceFactory;
struct Vterm;

struct VtermTitleChanged {
    Vterm* source;
    stl::StringView title;
};

enum class VtModifier : u8 {
    none = 0,
    shift = 1,
    control = 2,
    shift_control = 3,
    alt = 4,
    shift_alt = 5,
    control_alt = 6,
    shift_control_alt = 7,
    super = 8
};

constexpr VtModifier operator|(VtModifier lhs, VtModifier rhs) {
    return (VtModifier)((u8)(lhs) | (u8)(rhs));
}

constexpr VtModifier operator&(VtModifier lhs, VtModifier rhs) {
    return (VtModifier)((u8)(lhs) & (u8)(rhs));
}

enum class MouseTrackingMode : u8 {
    Disabled = 0,
    X10_Compat,
    VT200,
    VT200_ButtonEvent,
    VT200_AnyEvent,
    VT200_Highlight
};
enum class MouseTrackingEnc : u8 {
    Default = 0,
    UTF8,
    SGR,
    URXVT,
    SGRPixels
};

struct MouseTrackingState {
    MouseTrackingMode mode = MouseTrackingMode::Disabled;
    MouseTrackingEnc enc = MouseTrackingEnc::Default;
    bool focusEventMode = false;
    u32 generation = 0;

    void setMode(MouseTrackingMode value);
    void setEncoding(MouseTrackingEnc value);
};

struct RectangleOrigin {
    u16 rowBase;
    u16 columnBase;
    u16 rowLimit;
    u16 columnLimit;
};

enum class VtermKeyEventType : u8 {
    Press = 1,
    Repeat = 2,
    Release = 3
};

struct VtermTextResult {
    stl::StringView text;
    bool status = false;
};

// A mode snapshot for embedders that surface terminal state without
// reaching into the protocol - what an emulator wrapper reports as
// "modes". Presentation state travels with TerminalUpdate instead.
struct VtermState {
    MouseTrackingMode mouseTracking = MouseTrackingMode::Disabled;
    MouseTrackingEnc mouseEncoding = MouseTrackingEnc::Default;
    bool synchronizedOutput = false;
    bool alternateScreen = false;
    bool bracketedPaste = false;
    bool applicationCursorKeys = false;
    bool applicationKeypad = false;
    bool focusEvents = false;
    bool autoWrap = false;
    bool originMode = false;
    bool insertMode = false;
    bool showCursor = false;
    bool screenReverse = false;
    // DECSET 1007: on the alternate screen the wheel sends arrow keys
    // rather than moving a history the alternate screen does not have.
    bool alternateScroll = false;
};

struct TerminalUpdate {
    // The damaged view rows, ascending; each re-renders wholly.
    const TerminalRow* rows = nullptr;
    size_t rowCount = 0;
    // The model behind this frame: a strip-consuming renderer reads its
    // view rows and shapes them through the composer's span shaper. Null
    // when the update is synthesized without a screen (renderer-internal
    // repaints).
    Screen* shapes = nullptr;
    // The preedit preview: overlayCount cells drawn over overlayRow from
    // overlayColumn, covering the row content beneath them. They exist
    // outside the screen model, so the renderer shapes them itself as a
    // loose cell run. Zero count when no preview is active.
    const TerminalCell* overlayCells = nullptr;
    u16 overlayRow = 0;
    u16 overlayColumn = 0;
    u16 overlayCount = 0;
    // Every row carries cells foreign to the model (retained cells
    // re-rendered under a rebuilt presentation); the renderer shapes
    // each row as a loose cell run instead of the screen rows.
    bool shapeFromCells = false;
    const TerminalColors* colors = nullptr;
    u32 viewOffset = 0;
    u32 historyRows = 0;
    TerminalCursor cursor;
    Rect selection;
    Rect snappedSelection;
    Color selectionForeground;
    Color selectionBackground;
    u8 selectionColorMask = 0;
    u32 hoveredHyperlink = 0;
    u32 hoveredLinkBegin = 0;
    u32 hoveredLinkEnd = 0;
    bool screenReverse = false;
    bool blinkVisible = true;
    bool cursorBlink = false;
};

struct Vterm {
    virtual stl::StringView directory() const { return {}; }
    // Makes the terminal's presentation current and repaints it. The
    // repaint is not optional: a renderer may retain cells from the
    // presentation it consumed before this one.
    virtual void activate() = 0;
    // Ends the terminal's current presentation and drops its input focus.
    virtual void deactivate() = 0;
    // Input callbacks are invoked by whichever client currently presents
    // this terminal; Vterm does not join a global input router itself.
    virtual bool key(const plt::KeyInput& input) = 0;
    virtual bool text(const plt::TextInput& input) = 0;
    virtual bool pointerMotion(const VtPointerMotion& input) = 0;
    virtual bool pointerButton(const VtPointerButton& input) = 0;
    virtual bool scroll(const VtScroll& input) = 0;
    virtual void focus(bool focused) = 0;
    virtual void pointerPresence(bool present) = 0;
    // Placement changed under a stationary pointer. Update local state
    // without generating a synthetic mouse-motion report.
    virtual void pointerRepositioned(const VtPointerPosition& position) = 0;
    virtual void flush() = 0;
    // Terminal actions are likewise invoked by the presenting client.
    virtual void copy() = 0;
    virtual void paste(bool primary) = 0;
    virtual void pageUp() = 0;
    virtual void pageDown() = 0;
    // Moves the view through the scrollback: positive rows scroll up
    // into history, negative back toward the live bottom. Both clamp to
    // the retained history and return the resulting offset, which is 0
    // once the view is live again.
    virtual u32 scrollView(i32 rows) = 0;
    virtual u32 scrollViewTo(u32 offset) = 0;
    // What Ctrl+L means, reached from a platform's own chord. The byte
    // goes to the shell rather than clearing here, so the shell's own
    // idea of a clear - prompt redraw and all - is what happens.
    virtual void clear() = 0;
    virtual void feedPty(stl::StringView bytes) = 0;
    // One batch, one round of cursor and presentation bookkeeping: the
    // pty drain hands over whole blocks, and paying the per-feed wrap
    // per block would cost more than the parse.
    virtual void feedPty(const stl::StringView* slices, size_t count) = 0;
    virtual void expose() = 0;
    virtual void sendBytes(stl::StringView bytes, bool userInput) = 0;
    // Input-method composition preview, rendered as an overlay on the
    // cursor row of the emitted frame; never enters the screen model,
    // the scrollback, or the pty. Empty text clears the preview.
    // cursorBegin/cursorEnd are byte offsets into text, or -1 when the
    // input method hides its cursor. The preview clusters like printed
    // text, so it shows what the grid will hold once the composition
    // commits.
    virtual void preedit(stl::StringView text, i32 cursorBegin, i32 cursorEnd) = 0;
    // Text dropped onto the window by a drag-and-drop session; the stream
    // is pulled on the calling fiber chunk by chunk under the PTY mutex,
    // with the same sanitizing and bracketed-paste treatment as a
    // clipboard paste. Off fibers the payload is buffered whole (bounded)
    // and replayed from a transaction.
    virtual void dropText(stl::Input& source) = 0;
    // A text/uri-list drop: every entry is inserted shell-quoted with a
    // trailing separator through the same paste path as dropText().
    virtual void dropUriList(stl::Input& source) = 0;

    // Reads the pty's foreground process and, when its name differs
    // from the last one seen, makes that name the title - displacing
    // whatever the previous foreground left behind. While the name is
    // stable an application's own title stands untouched. Driven for
    // the active terminal by the embedder's one polling timer.
    virtual void refreshForegroundName() = 0;

    virtual bool expireSynchronizedOutput(bool force) = 0;
    virtual bool advanceAnimation(bool force) = 0;
    virtual const TerminalUpdate* output() = 0;
    virtual void consume() = 0;
    virtual VtermState state() const = 0;

    // The window geometry changed: adopt the composer's grid and redraw.
    // Delivered to every session, background ones included - a terminal
    // that resized only on activation would come back wrong.
    virtual void windowResized() = 0;
    // A configuration snapshot replaced the one behind VtConfigSlot::config;
    // the terminal re-materializes what it derived. Allocates before it
    // touches state: a throw leaves the previous materialization intact,
    // and the owner delivering the reload must keep walking its other
    // terminals.
    virtual void configChanged() = 0;
    // The embedder rebuilt how cells become pixels - every retained row
    // it holds is stale. Re-expose the whole screen and redraw.
    virtual void presentationInvalidated() = 0;
    // Whether the presentation moved past what the renderer last
    // consumed.
    virtual bool presentationChanged() const = 0;

    // The terminal and everything it owns - fiber stacks, screens - come
    // out of owner, which is what lets a session die by dropping its
    // arena.
    static Vterm* create(stl::ObjPool& owner, VtGeometry& geometry, const VtConfigSlot& config, VtCellExtras& extras, stl::SmallObjAllocator& smallObjects, plt::Scheduler& scheduler, VtHost& host, PtyHandle& pty, VtermTraceFactory* traceFactory);
};
