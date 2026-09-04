#include <plt/platform.h>
#include <plt/drop.h>
#include <plt/input.h>
#include <plt/loop_wake.h>
#include <plt/poller.h>
#include <plt/window.h>

#include <std/mem/obj_pool.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <cstring>

using namespace plt;
using namespace stl;

namespace {
    struct CloseEvents final: WindowEvents {
        explicit CloseEvents(Platform& platform_)
            : platform(platform_)
        {
        }

        void close() override {
            ++calls;
            platform.stop();
        }

        Platform& platform;
        size_t calls = 0;
    };

    struct StopLoop final: TimerCallback {
        explicit StopLoop(Platform& platform_)
            : platform(platform_)
        {
        }

        void ready() override {
            platform.stop();
        }

        Platform& platform;
    };

    struct FrameProbe final: FrameCallback {
        explicit FrameProbe(Platform& platform_)
            : platform(platform_)
        {
        }

        bool frame(const WindowInfo& info) override {
            ++calls;
            seen = info;
            if (stopOnFrame) {
                platform.stop();
            }
            return true;
        }

        Platform& platform;
        WindowInfo seen;
        size_t calls = 0;
        bool stopOnFrame = false;
    };

    struct InputProbe final: InputSink {
        void key(const KeyInput& input) override {
            lastKey = input;
            ++keys;
        }

        void text(const TextInput& input) override {
            lastText = input;
            ++texts;
        }

        void preedit(StringView, i32, i32) override {
            ++preedits;
        }

        void pointerMotion(const PointerMotionInput& input) override {
            lastMotion = input;
            ++motions;
        }

        void pointerButton(const PointerButtonInput& input) override {
            lastButton = input;
            ++buttons;
        }

        void scroll(const ScrollInput& input) override {
            lastScroll = input;
            ++scrolls;
        }

        void focus(bool value) override {
            focused = value;
            ++focuses;
        }

        void pointerPresence(bool value) override {
            present = value;
            ++presences;
        }

        void flush() override {
            ++flushes;
        }

        KeyInput lastKey;
        TextInput lastText;
        PointerMotionInput lastMotion;
        PointerButtonInput lastButton;
        ScrollInput lastScroll;
        size_t keys = 0;
        size_t texts = 0;
        size_t preedits = 0;
        size_t motions = 0;
        size_t buttons = 0;
        size_t scrolls = 0;
        size_t focuses = 0;
        size_t presences = 0;
        size_t flushes = 0;
        bool focused = false;
        bool present = false;
    };

    bool clipboardRoundTrip(Clipboard& clipboard, StringView value) {
        Output* const output = clipboard.write();
        output->write(value.data(), value.length());
        output->finish();
        delete output;
        Input* const input = clipboard.read();
        Buffer received;
        input->readAll(received);
        delete input;
        return received.length() == value.length()
            && std::memcmp(received.data(), value.data(), received.length()) == 0;
    }

    struct SavedClipboard final {
        bool capture() {
            return OleGetClipboard(&content) == S_OK;
        }

        bool restore() {
            if (content == nullptr) {
                return true;
            }
            HRESULT set = CLIPBRD_E_CANT_OPEN;
            for (unsigned attempt = 0; attempt != 20 && FAILED(set); ++attempt) {
                set = OleSetClipboard(content);
                if (FAILED(set)) {
                    Sleep(5);
                }
            }
            const HRESULT flush = SUCCEEDED(set) ? OleFlushClipboard() : set;
            const bool restored = SUCCEEDED(set) && SUCCEEDED(flush);
            content->Release();
            content = nullptr;
            return restored;
        }

        ~SavedClipboard() noexcept {
            restore();
        }

        IDataObject* content = nullptr;
    };

    struct DropProbe final: DropTarget {
        DropReply dragOver(const DropOffer& offer, i32, i32) override {
            ++dragOvers;
            for (size_t index = 0; index != offer.formats(); ++index) {
                const StringView format = offer.format(index);
                if (format == StringView(u8"text/uri-list")
                        || format == StringView(u8"text/plain;charset=utf-8")) {
                    selected = format;
                    return {.mime = format, .action = DropAction::Copy};
                }
            }
            return {};
        }

        void dragLeft() override {
            ++dragLeaves;
        }

        void dropped(Drop& drop) override {
            ++drops;
            content.reset();
            Input* const input = drop.read(selected);
            input->readAll(content);
            delete input;
        }

        size_t dragOvers = 0;
        size_t dragLeaves = 0;
        size_t drops = 0;
        StringView selected;
        Buffer content;
    };

    struct GlobalDataObject final: IDataObject {
        GlobalDataObject(CLIPFORMAT format_, Buffer&& content_)
            : format(format_)
            , content(static_cast<Buffer&&>(content_))
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDataObject)) {
                *object = static_cast<IDataObject*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override {
            return static_cast<ULONG>(InterlockedIncrement(&references));
        }

        ULONG STDMETHODCALLTYPE Release() override {
            return static_cast<ULONG>(InterlockedDecrement(&references));
        }

        HRESULT STDMETHODCALLTYPE GetData(
            FORMATETC* request,
            STGMEDIUM* medium
        ) override {
            if (QueryGetData(request) != S_OK) {
                return DV_E_FORMATETC;
            }
            const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, content.length());
            if (memory == nullptr) {
                return E_OUTOFMEMORY;
            }
            void* const destination = GlobalLock(memory);
            if (destination == nullptr) {
                GlobalFree(memory);
                return E_OUTOFMEMORY;
            }
            std::memcpy(destination, content.data(), content.length());
            SetLastError(NO_ERROR);
            GlobalUnlock(memory);
            *medium = {
                .tymed = TYMED_HGLOBAL,
                .hGlobal = memory,
                .pUnkForRelease = nullptr,
            };
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
            return request->cfFormat == format
                    && (request->tymed & TYMED_HGLOBAL) != 0
                    && request->dwAspect == DVASPECT_CONTENT
                ? S_OK
                : DV_E_FORMATETC;
        }

        HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
            FORMATETC*, FORMATETC* output
        ) override {
            output->ptd = nullptr;
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE DAdvise(
            FORMATETC*, DWORD, IAdviseSink*, DWORD*
        ) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }

        HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }

        HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }

        CLIPFORMAT format;
        Buffer content;
        LONG references = 1;
    };

    Buffer fileDropContent() {
        constexpr wchar_t paths[] = L"C:\\Temp\\alpha beta.txt\0D:\\Ω🙂.txt\0";
        DROPFILES header{};
        header.pFiles = sizeof(DROPFILES);
        header.fWide = TRUE;
        Buffer content;
        content.append(&header, sizeof(header));
        content.append(paths, sizeof(paths));
        return content;
    }
}

int main() {
    ObjPool::Ref pool = ObjPool::fromMemory();
    Platform& platform = *Platform::create(*pool);
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    if (CoGetApartmentType(&apartment, &qualifier) != S_OK
            || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA)) {
        return 17;
    }
    CloseEvents events(platform);
    FrameProbe frame(platform);
    InputProbe input;
    DropProbe drop;
    Window& window = *platform.createWindow(
        *pool,
        {
            .title = u8"Shitty Win32 test",
            .width = 320,
            .height = 200,
            .input = &input,
            .events = &events,
            .frame = &frame,
            .drop = &drop,
        }
    );
    const WindowInfo info = window.info();
    const RenderContext context = window.renderContext();
    if (info.width != 320 || info.height != 200) {
        return 1;
    }
    if (context.backend != RenderBackend::Win32
            || context.connection == nullptr
            || context.window == nullptr) {
        return 2;
    }
    const HWND handle = static_cast<HWND>(context.window);
    if (GetPropW(handle, L"Shitty.Win32.DropTarget") == nullptr) {
        return 34;
    }

    const LPARAM aKey = 1 | (0x1e << 16);
    SendMessageW(handle, WM_KEYDOWN, 'A', aKey);
    if (input.keys != 1
            || input.lastKey.key != InputKey::Printable
            || input.lastKey.action != InputAction::Press
            || input.lastKey.layoutCodepoint == 0
            || input.lastKey.baseCodepoint != 'a'
            || input.lastKey.shiftedCodepoint == 0) {
        return 18;
    }
    SendMessageW(handle, WM_KEYUP, 'A', aKey | (1LL << 30) | (1LL << 31));
    if (input.keys != 2 || input.lastKey.action != InputAction::Release) {
        return 19;
    }
    const LPARAM leftControl = 1 | (0x1d << 16);
    const LPARAM rightAlt = 1 | (0x38 << 16) | (1LL << 24);
    SendMessageW(handle, WM_KEYDOWN, VK_CONTROL, leftControl);
    SendMessageW(handle, WM_SYSKEYDOWN, VK_MENU, rightAlt);
    SendMessageW(handle, WM_KEYDOWN, 'A', aKey);
    if ((input.lastKey.modifiers
            & (InputControl | InputAlt | InputAltGraph)) != InputAltGraph) {
        return 26;
    }
    SendMessageW(handle, WM_KEYUP, 'A', aKey | (1LL << 30) | (1LL << 31));
    SendMessageW(handle, WM_SYSKEYUP, VK_MENU, rightAlt | (1LL << 30) | (1LL << 31));
    SendMessageW(handle, WM_KEYUP, VK_CONTROL, leftControl | (1LL << 30) | (1LL << 31));
    SendMessageW(handle, WM_SYSKEYDOWN, VK_MENU, rightAlt);
    if ((input.lastKey.modifiers & (InputAlt | InputAltGraph)) != InputAlt) {
        return 39;
    }
    SendMessageW(handle, WM_SYSKEYUP, VK_MENU, rightAlt | (1LL << 30) | (1LL << 31));
    SendMessageW(handle, WM_CHAR, 'A', 1);
    SendMessageW(handle, WM_CHAR, 0xd83d, 1);
    SendMessageW(handle, WM_CHAR, 0xde42, 1);
    if (input.texts != 2 || input.lastText.codepoint != 0x1f642) {
        return 20;
    }

    SendMessageW(handle, WM_MOUSEMOVE, 0, MAKELPARAM(17, 23));
    if (input.motions != 1
            || input.lastMotion.pixelX != 17
            || input.lastMotion.pixelY != 23
            || input.presences != 1
            || !input.present) {
        return 21;
    }
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(17, 23));
    SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(17, 23));
    if (input.buttons != 2
            || input.lastButton.button != PointerButton::Primary
            || input.lastButton.pressed) {
        return 22;
    }
    SendMessageW(
        handle,
        WM_MOUSEWHEEL,
        MAKEWPARAM(0, WHEEL_DELTA),
        MAKELPARAM(17, 23)
    );
    if (input.scrolls != 1 || input.lastScroll.y != 1.0) {
        return 23;
    }
    SendMessageW(
        handle,
        WM_MOUSEHWHEEL,
        MAKEWPARAM(0, WHEEL_DELTA),
        MAKELPARAM(17, 23)
    );
    if (input.scrolls != 2 || input.lastScroll.x != 1.0) {
        return 27;
    }
    SendMessageW(handle, WM_MOUSELEAVE, 0, 0);
    if (input.presences != 2 || input.present) {
        return 24;
    }
    SendMessageW(handle, WM_SETFOCUS, 0, 0);
    SendMessageW(handle, WM_KILLFOCUS, 0, 0);
    if (input.focuses != 2 || input.focused || input.flushes == 0) {
        return 25;
    }
    window.requestTextInputRect(11, 13, 17, 19);
    SendMessageW(handle, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(handle, WM_IME_ENDCOMPOSITION, 0, 0);
    if (input.preedits != 1) {
        return 28;
    }
    const struct CursorCase {
        PointerIcon icon;
        LPCWSTR native;
    } cursorCases[] = {
        {PointerIcon::Default, IDC_ARROW},
        {PointerIcon::Help, IDC_HELP},
        {PointerIcon::Pointer, IDC_HAND},
        {PointerIcon::Progress, IDC_APPSTARTING},
        {PointerIcon::Wait, IDC_WAIT},
        {PointerIcon::Crosshair, IDC_CROSS},
        {PointerIcon::Text, IDC_IBEAM},
        {PointerIcon::NotAllowed, IDC_NO},
        {PointerIcon::ResizeEastWest, IDC_SIZEWE},
        {PointerIcon::ResizeNorthSouth, IDC_SIZENS},
        {PointerIcon::ResizeNorthEastSouthWest, IDC_SIZENESW},
        {PointerIcon::ResizeNorthWestSouthEast, IDC_SIZENWSE},
        {PointerIcon::ResizeAll, IDC_SIZEALL},
    };
    for (const CursorCase& cursor : cursorCases) {
        window.requestPointerIcon(cursor.icon);
        if (GetCursor() != LoadCursorW(nullptr, cursor.native)) {
            return 33;
        }
    }
    SendMessageW(
        handle,
        WM_SETCURSOR,
        reinterpret_cast<WPARAM>(handle),
        MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)
    );
    if (GetCursor() != LoadCursorW(nullptr, IDC_SIZEALL)) {
        return 38;
    }

    window.requestMinimumSize(123, 77);
    MINMAXINFO limits{};
    SendMessageW(
        handle,
        WM_GETMINMAXINFO,
        0,
        reinterpret_cast<LPARAM>(&limits)
    );
    if (limits.ptMinTrackSize.x < 123 || limits.ptMinTrackSize.y < 77) {
        return 3;
    }

    window.requestResizeUnit(10, 5, 0, 0);
    RECT sizing;
    GetWindowRect(handle, &sizing);
    sizing.right += 3;
    sizing.bottom += 2;
    SendMessageW(
        handle,
        WM_SIZING,
        WMSZ_BOTTOMRIGHT,
        reinterpret_cast<LPARAM>(&sizing)
    );
    SetWindowPos(
        handle,
        nullptr,
        sizing.left,
        sizing.top,
        sizing.right - sizing.left,
        sizing.bottom - sizing.top,
        SWP_NOACTIVATE | SWP_NOZORDER
    );
    const WindowInfo snapped = window.info();
    if (snapped.width % 10 != 0 || snapped.height % 5 != 0) {
        return 4;
    }

    window.requestMove(30, 40);
    window.requestResize(400, 250);
    const WindowInfo resized = window.info();
    if (resized.x != 30 || resized.y != 40
            || resized.width != 400 || resized.height != 250) {
        return 5;
    }
    window.requestFullscreen(true);
    if (!window.info().fullscreen) {
        return 6;
    }
    window.requestFullscreen(false);
    if (window.info().fullscreen) {
        return 7;
    }
    const WindowInfo restored = window.info();
    if (restored.width != 400 || restored.height != 250) {
        std::fprintf(
            stderr,
            "restored size=%ux%u\n",
            restored.width,
            restored.height
        );
        return 8;
    }

    window.requestShow();
    const WindowInfo shown = window.info();
    if (shown.width != 400 || shown.height != 250) {
        std::fprintf(stderr, "shown size=%ux%u\n", shown.width, shown.height);
        return 9;
    }
    frame.calls = 0;
    frame.stopOnFrame = true;
    window.requestFrame();
    platform.run();
    if (frame.calls == 0
            || frame.seen.width == 0
            || frame.seen.height == 0
            || frame.seen.contentScale <= 0.0f) {
        std::fprintf(
            stderr,
            "frame calls=%zu size=%ux%u\n",
            frame.calls,
            frame.seen.width,
            frame.seen.height
        );
        return 10;
    }
    frame.stopOnFrame = false;

    window.requestTitle(u8"Updated Win32 title");
    wchar_t title[64]{};
    GetWindowTextW(handle, title, static_cast<int>(std::size(title)));
    if (std::wcscmp(title, L"Updated Win32 title") != 0) {
        return 11;
    }

    window.requestIconify();
    if (!window.info().iconified) {
        return 12;
    }
    window.requestRestore();
    if (window.info().iconified) {
        return 13;
    }
    const auto waitMaximized = [&window](bool expected) {
        for (unsigned attempt = 0; attempt != 1000; ++attempt) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (window.info().maximized == expected) {
                return true;
            }
            Sleep(1);
        }
        return false;
    };
    window.requestMaximized(true);
    if (!waitMaximized(true)) {
        return 14;
    }
    window.requestMaximized(false);
    if (!waitMaximized(false)) {
        return 15;
    }

    if (!clipboardRoundTrip(*window.primary(), u8"primary Ω 🙂")) {
        return 29;
    }
    SavedClipboard savedClipboard;
    if (!savedClipboard.capture()) {
        return 30;
    }
    const bool secondaryRoundTrip = clipboardRoundTrip(
        *window.secondary(),
        u8"secondary Ω 🙂"
    );
    DWORD dropEffect = DROPEFFECT_COPY;
    RECT dropRect;
    GetWindowRect(handle, &dropRect);
    const POINTL dropPoint = {
        .x = dropRect.left + 1,
        .y = dropRect.top + 1,
    };
    IDropTarget* const nativeDrop = static_cast<IDropTarget*>(
        GetPropW(handle, L"Shitty.Win32.DropTarget")
    );
    constexpr wchar_t droppedTextWide[] = L"secondary Ω 🙂";
    GlobalDataObject dropObject(
        CF_UNICODETEXT,
        Buffer(droppedTextWide, sizeof(droppedTextWide))
    );
    nativeDrop->DragEnter(&dropObject, MK_LBUTTON, dropPoint, &dropEffect);
    nativeDrop->Drop(&dropObject, MK_LBUTTON, dropPoint, &dropEffect);
    const StringView droppedText(u8"secondary Ω 🙂");
    const bool textDropReady = dropEffect == DROPEFFECT_COPY
        && drop.drops == 1
        && drop.content.length() == droppedText.length()
        && std::memcmp(
            drop.content.data(),
            droppedText.data(),
            drop.content.length()
        ) == 0;
    GlobalDataObject fileObject(CF_HDROP, fileDropContent());
    dropEffect = DROPEFFECT_COPY;
    nativeDrop->DragEnter(&fileObject, MK_LBUTTON, dropPoint, &dropEffect);
    nativeDrop->Drop(&fileObject, MK_LBUTTON, dropPoint, &dropEffect);
    const StringView droppedFiles(
        u8"file:///C:/Temp/alpha%20beta.txt\r\n"
        u8"file:///D:/%CE%A9%F0%9F%99%82.txt\r\n"
    );
    const bool fileDropReady = dropEffect == DROPEFFECT_COPY
        && drop.drops == 2
        && drop.content.length() == droppedFiles.length()
        && std::memcmp(
            drop.content.data(),
            droppedFiles.data(),
            drop.content.length()
        ) == 0;
    const bool clipboardRestored = savedClipboard.restore();
    if (!secondaryRoundTrip) {
        return 31;
    }
    if (!clipboardRestored) {
        return 32;
    }
    if (!textDropReady) {
        return 35;
    }
    if (!fileDropReady) {
        return 36;
    }

    StopLoop stop(platform);
    LoopWake& wake = *platform.createLoopWake(*pool, stop);
    wake.signal();
    platform.run();

    window.requestClose();
    platform.run();

    return events.calls == 1 ? 0 : 16;
}
