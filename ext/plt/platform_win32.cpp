#include "platform_win32.h"

#include "fiber.h"
#include "drop.h"
#include "input.h"
#include "loop_wake.h"
#include "poller_loop.h"
#include "window.h"

#include <std/dbg/insist.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>
#include <string>

using namespace plt;
using namespace stl;

namespace {
    constexpr wchar_t windowClassName[] = L"Shitty.Platform.Win32";
    constexpr wchar_t dropTargetProperty[] = L"Shitty.Win32.DropTarget";
    const StringView utf8Mime(u8"text/plain;charset=utf-8");
    const StringView uriListMime(u8"text/uri-list");

    struct PlatformWin32;
    struct WindowWin32;

    LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    std::wstring wide(StringView text) {
        if (text.empty()) {
            return {};
        }
        STD_INSIST(text.length() <= INT_MAX);
        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(text.data()),
            static_cast<int>(text.length()),
            nullptr,
            0
        );
        STD_INSIST(length > 0);
        std::wstring result(static_cast<size_t>(length), L'\0');
        STD_INSIST(MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(text.data()),
            static_cast<int>(text.length()),
            result.data(),
            length
        ) == length);
        return result;
    }

    std::string utf8(const wchar_t* text, size_t length) {
        if (length == 0) {
            return {};
        }
        STD_INSIST(length <= INT_MAX);
        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            static_cast<int>(length),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        STD_INSIST(size > 0);
        std::string result(static_cast<size_t>(size), '\0');
        STD_INSIST(WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            static_cast<int>(length),
            result.data(),
            size,
            nullptr,
            nullptr
        ) == size);
        return result;
    }

    bool enableDpiAwareness() {
        if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0) {
            return true;
        }
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    RECT adjustedClientRect(
        LONG_PTR style,
        LONG_PTR extendedStyle,
        u32 width,
        u32 height,
        UINT dpi
    ) {
        RECT result = {
            .left = 0,
            .top = 0,
            .right = static_cast<LONG>(width),
            .bottom = static_cast<LONG>(height),
        };
        STD_INSIST(AdjustWindowRectExForDpi(
            &result,
            static_cast<DWORD>(style),
            FALSE,
            static_cast<DWORD>(extendedStyle),
            dpi
        ) != 0);
        return result;
    }

    LONG snappedSize(u32 value, u32 unit, u32 base, u32 minimum) {
        const u64 bounded = std::max<u64>(value, minimum);
        if (bounded <= base) {
            return static_cast<LONG>(std::max<u64>(base, minimum));
        }
        const u64 count = (bounded - base + unit / 2) / unit;
        return static_cast<LONG>(std::min<u64>(
            std::max<u64>(base + count * unit, minimum),
            LONG_MAX
        ));
    }

    InputKey inputKey(WPARAM value, LPARAM details) {
        const bool extended = (details & (1LL << 24)) != 0;
        const UINT scan = (static_cast<UINT>(details) >> 16) & 0xff;
        if (value >= VK_F1 && value <= VK_F24) {
            return static_cast<InputKey>(
                static_cast<u8>(InputKey::F1) + value - VK_F1
            );
        }
        if (value >= VK_NUMPAD0 && value <= VK_NUMPAD9) {
            return static_cast<InputKey>(
                static_cast<u8>(InputKey::Keypad0) + value - VK_NUMPAD0
            );
        }
        switch (value) {
        case VK_SPACE: return InputKey::Space;
        case VK_ESCAPE: return InputKey::Escape;
        case VK_RETURN: return extended ? InputKey::KeypadEnter : InputKey::Enter;
        case VK_BACK: return InputKey::Backspace;
        case VK_TAB: return InputKey::Tab;
        case VK_INSERT: return extended ? InputKey::Insert : InputKey::KeypadInsert;
        case VK_DELETE: return extended ? InputKey::Delete : InputKey::KeypadDelete;
        case VK_HOME: return extended ? InputKey::Home : InputKey::KeypadHome;
        case VK_END: return extended ? InputKey::End : InputKey::KeypadEnd;
        case VK_UP: return extended ? InputKey::Up : InputKey::KeypadUp;
        case VK_DOWN: return extended ? InputKey::Down : InputKey::KeypadDown;
        case VK_LEFT: return extended ? InputKey::Left : InputKey::KeypadLeft;
        case VK_RIGHT: return extended ? InputKey::Right : InputKey::KeypadRight;
        case VK_PRIOR: return extended ? InputKey::PageUp : InputKey::KeypadPageUp;
        case VK_NEXT: return extended ? InputKey::PageDown : InputKey::KeypadPageDown;
        case VK_CLEAR: return extended ? InputKey::Clear : InputKey::KeypadBegin;
        case VK_DECIMAL: return InputKey::KeypadDecimal;
        case VK_DIVIDE: return InputKey::KeypadDivide;
        case VK_MULTIPLY: return InputKey::KeypadMultiply;
        case VK_SUBTRACT: return InputKey::KeypadSubtract;
        case VK_ADD: return InputKey::KeypadAdd;
        case VK_SEPARATOR: return InputKey::KeypadSeparator;
        case VK_CAPITAL: return InputKey::CapsLock;
        case VK_SCROLL: return InputKey::ScrollLock;
        case VK_NUMLOCK: return InputKey::NumLock;
        case VK_SNAPSHOT: return InputKey::PrintScreen;
        case VK_PAUSE: return InputKey::Pause;
        case VK_APPS: return InputKey::Menu;
        case VK_LSHIFT: return InputKey::LeftShift;
        case VK_RSHIFT: return InputKey::RightShift;
        case VK_SHIFT: return scan == 0x36 ? InputKey::RightShift : InputKey::LeftShift;
        case VK_LCONTROL: return InputKey::LeftControl;
        case VK_RCONTROL: return InputKey::RightControl;
        case VK_CONTROL: return extended ? InputKey::RightControl : InputKey::LeftControl;
        case VK_LMENU: return InputKey::LeftAlt;
        case VK_RMENU: return InputKey::RightAlt;
        case VK_MENU: return extended ? InputKey::RightAlt : InputKey::LeftAlt;
        case VK_LWIN: return InputKey::LeftSuper;
        case VK_RWIN: return InputKey::RightSuper;
        case VK_MEDIA_PLAY_PAUSE: return InputKey::MediaPlayPause;
        case VK_MEDIA_STOP: return InputKey::MediaStop;
        case VK_MEDIA_NEXT_TRACK: return InputKey::MediaTrackNext;
        case VK_MEDIA_PREV_TRACK: return InputKey::MediaTrackPrevious;
        case VK_VOLUME_DOWN: return InputKey::VolumeDown;
        case VK_VOLUME_UP: return InputKey::VolumeUp;
        case VK_VOLUME_MUTE: return InputKey::VolumeMute;
        default:
            break;
        }
        if ((value >= '0' && value <= '9')
                || (value >= 'A' && value <= 'Z')
                || (value >= VK_OEM_1 && value <= VK_OEM_3)
                || (value >= VK_OEM_4 && value <= VK_OEM_8)
                || value == VK_OEM_102) {
            return InputKey::Printable;
        }
        return InputKey::Unknown;
    }

    u32 baseCodepoint(WPARAM key, bool shifted) {
        if (key >= 'A' && key <= 'Z') {
            return static_cast<u32>(shifted ? key : key + ('a' - 'A'));
        }
        if (key >= '0' && key <= '9') {
            constexpr char shiftedDigits[] = ")!@#$%^&*(";
            return static_cast<u32>(shifted ? shiftedDigits[key - '0'] : key);
        }
        switch (key) {
        case VK_OEM_1: return shifted ? ':' : ';';
        case VK_OEM_PLUS: return shifted ? '+' : '=';
        case VK_OEM_COMMA: return shifted ? '<' : ',';
        case VK_OEM_MINUS: return shifted ? '_' : '-';
        case VK_OEM_PERIOD: return shifted ? '>' : '.';
        case VK_OEM_2: return shifted ? '?' : '/';
        case VK_OEM_3: return shifted ? '~' : '`';
        case VK_OEM_4: return shifted ? '{' : '[';
        case VK_OEM_5: return shifted ? '|' : '\\';
        case VK_OEM_6: return shifted ? '}' : ']';
        case VK_OEM_7: return shifted ? '"' : '\'';
        default: return 0;
        }
    }

    u32 layoutCodepoint(WPARAM key, LPARAM details, bool shifted) {
        BYTE state[256]{};
        state[VK_SHIFT] = shifted ? 0x80 : 0;
        wchar_t value[4]{};
        const UINT scan = (static_cast<UINT>(details) >> 16) & 0xff;
        const int length = ToUnicodeEx(
            static_cast<UINT>(key),
            scan,
            state,
            value,
            4,
            4,
            GetKeyboardLayout(0)
        );
        if (length == 1 && (value[0] < 0xd800 || value[0] > 0xdfff)) {
            return value[0];
        }
        if (length >= 2
                && value[0] >= 0xd800 && value[0] <= 0xdbff
                && value[1] >= 0xdc00 && value[1] <= 0xdfff) {
            return 0x10000
                + ((static_cast<u32>(value[0]) - 0xd800) << 10)
                + static_cast<u32>(value[1]) - 0xdc00;
        }
        return baseCodepoint(key, shifted);
    }

    struct Win32BufferInput final: Input {
        Win32BufferInput(SmallObjAllocator& allocator_, Buffer&& content_)
            : allocator(allocator_)
            , content(static_cast<Buffer&&>(content_))
        {
        }

        void operator delete(Win32BufferInput* input, std::destroying_delete_t) noexcept {
            SmallObjAllocator& owner = input->allocator;
            owner.release(input);
        }

        size_t readImpl(void* data, size_t size) override {
            const size_t count = std::min(size, content.length() - offset);
            if (count != 0) {
                std::memcpy(data, static_cast<const u8*>(content.data()) + offset, count);
                offset += count;
            }
            return count;
        }

        SmallObjAllocator& allocator;
        Buffer content;
        size_t offset = 0;
    };

    struct PrimaryClipboardOutput final: Output {
        PrimaryClipboardOutput(SmallObjAllocator& allocator_, Buffer& destination_)
            : allocator(allocator_)
            , destination(destination_)
        {
        }

        void operator delete(PrimaryClipboardOutput* output, std::destroying_delete_t) noexcept {
            SmallObjAllocator& owner = output->allocator;
            owner.release(output);
        }

        size_t writeImpl(const void* data, size_t size) override {
            content.append(data, size);
            return size;
        }

        void finishImpl() override {
            if (!finished) {
                destination = content;
                finished = true;
            }
        }

        SmallObjAllocator& allocator;
        Buffer& destination;
        Buffer content;
        bool finished = false;
    };

    struct PrimaryClipboard final: Clipboard {
        explicit PrimaryClipboard(SmallObjAllocator& allocator_)
            : allocator(allocator_)
        {
        }

        Input* read() override {
            return allocator.make<Win32BufferInput>(allocator, Buffer(content));
        }

        Output* write() override {
            return allocator.make<PrimaryClipboardOutput>(allocator, content);
        }

        SmallObjAllocator& allocator;
        Buffer content;
    };

    bool openSystemClipboard() {
        for (unsigned attempt = 0; attempt != 10; ++attempt) {
            if (OpenClipboard(nullptr) != 0) {
                return true;
            }
            Sleep(1);
        }
        return false;
    }

    struct SystemClipboardOutput final: Output {
        explicit SystemClipboardOutput(SmallObjAllocator& allocator_)
            : allocator(allocator_)
        {
        }

        void operator delete(SystemClipboardOutput* output, std::destroying_delete_t) noexcept {
            SmallObjAllocator& owner = output->allocator;
            owner.release(output);
        }

        size_t writeImpl(const void* data, size_t size) override {
            content.append(data, size);
            return size;
        }

        void finishImpl() override {
            if (finished) {
                return;
            }
            const std::wstring value = wide(StringView(content));
            const SIZE_T bytes = (value.length() + 1) * sizeof(wchar_t);
            const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            STD_INSIST(memory != nullptr);
            void* const locked = GlobalLock(memory);
            STD_INSIST(locked != nullptr);
            std::memcpy(locked, value.c_str(), bytes);
            SetLastError(NO_ERROR);
            const BOOL unlocked = GlobalUnlock(memory);
            STD_INSIST(unlocked != 0 || GetLastError() == NO_ERROR);
            STD_INSIST(openSystemClipboard());
            STD_INSIST(EmptyClipboard() != 0);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                CloseClipboard();
                GlobalFree(memory);
                STD_INSIST(false);
            }
            STD_INSIST(CloseClipboard() != 0);
            finished = true;
        }

        SmallObjAllocator& allocator;
        Buffer content;
        bool finished = false;
    };

    struct SystemClipboard final: Clipboard {
        explicit SystemClipboard(SmallObjAllocator& allocator_)
            : allocator(allocator_)
        {
        }

        Input* read() override {
            STD_INSIST(openSystemClipboard());
            const HANDLE memory = GetClipboardData(CF_UNICODETEXT);
            if (memory == nullptr) {
                STD_INSIST(CloseClipboard() != 0);
                return allocator.make<Win32BufferInput>(allocator, Buffer());
            }
            const auto* const text = static_cast<const wchar_t*>(GlobalLock(memory));
            STD_INSIST(text != nullptr);
            const size_t capacity = GlobalSize(memory) / sizeof(wchar_t);
            size_t length = 0;
            while (length != capacity && text[length] != L'\0') {
                ++length;
            }
            const std::string value = utf8(text, length);
            SetLastError(NO_ERROR);
            const BOOL unlocked = GlobalUnlock(memory);
            STD_INSIST(unlocked != 0 || GetLastError() == NO_ERROR);
            STD_INSIST(CloseClipboard() != 0);
            return allocator.make<Win32BufferInput>(
                allocator,
                Buffer(value.data(), value.length())
            );
        }

        Output* write() override {
            return allocator.make<SystemClipboardOutput>(allocator);
        }

        SmallObjAllocator& allocator;
    };

    FORMATETC globalFormat(CLIPFORMAT format) {
        return {
            .cfFormat = format,
            .ptd = nullptr,
            .dwAspect = DVASPECT_CONTENT,
            .lindex = -1,
            .tymed = TYMED_HGLOBAL,
        };
    }

    bool dataFormat(IDataObject& object, CLIPFORMAT format) {
        FORMATETC request = globalFormat(format);
        return object.QueryGetData(&request) == S_OK;
    }

    struct Win32DropOffer final: DropOffer {
        size_t formats() const override {
            return static_cast<size_t>(files) + static_cast<size_t>(text);
        }

        StringView format(size_t index) const override {
            if (files) {
                if (index == 0) {
                    return uriListMime;
                }
                --index;
            }
            return text && index == 0 ? utf8Mime : StringView();
        }

        bool text = false;
        bool files = false;
    };

    Buffer unicodeDrop(IDataObject& object) {
        FORMATETC request = globalFormat(CF_UNICODETEXT);
        STGMEDIUM medium{};
        if (object.GetData(&request, &medium) != S_OK) {
            return {};
        }
        const auto* const text = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
        if (text == nullptr) {
            ReleaseStgMedium(&medium);
            return {};
        }
        const size_t capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
        size_t length = 0;
        while (length != capacity && text[length] != L'\0') {
            ++length;
        }
        const std::string value = utf8(text, length);
        SetLastError(NO_ERROR);
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return Buffer(value.data(), value.length());
    }

    std::string fileUri(const std::wstring& path) {
        std::string value = utf8(path.data(), path.length());
        std::replace(value.begin(), value.end(), '\\', '/');
        std::string result = value.starts_with("//") ? "file:" : "file:///";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char byte : value) {
            if ((byte >= 'a' && byte <= 'z')
                    || (byte >= 'A' && byte <= 'Z')
                    || (byte >= '0' && byte <= '9')
                    || byte == '-' || byte == '.' || byte == '_'
                    || byte == '~' || byte == '/' || byte == ':') {
                result.push_back(static_cast<char>(byte));
            } else {
                result.push_back('%');
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
            }
        }
        return result;
    }

    Buffer fileDrop(IDataObject& object) {
        FORMATETC request = globalFormat(CF_HDROP);
        STGMEDIUM medium{};
        if (object.GetData(&request, &medium) != S_OK) {
            return {};
        }
        const HDROP drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::string list;
        for (UINT index = 0; index != count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
                continue;
            }
            path.resize(length);
            list += fileUri(path);
            list += "\r\n";
        }
        ReleaseStgMedium(&medium);
        return Buffer(list.data(), list.length());
    }

    struct Win32Drop final: Drop {
        Win32Drop(
            SmallObjAllocator& allocator_,
            IDataObject& object_,
            Win32DropOffer& offer_
        )
            : allocator(allocator_)
            , object(object_)
            , offer(offer_)
        {
        }

        DropOffer* what() override {
            return &offer;
        }

        Input* read(StringView mime) override {
            STD_INSIST(!taken);
            taken = true;
            STD_INSIST(
                (mime == utf8Mime && offer.text)
                || (mime == uriListMime && offer.files)
            );
            return allocator.make<Win32BufferInput>(
                allocator,
                mime == uriListMime ? fileDrop(object) : unicodeDrop(object)
            );
        }

        SmallObjAllocator& allocator;
        IDataObject& object;
        Win32DropOffer& offer;
        bool taken = false;
    };

    struct Win32DropTarget final: IDropTarget {
        Win32DropTarget(
            SmallObjAllocator& allocator_,
            HWND window_,
            DropTarget& target_
        )
            : allocator(allocator_)
            , window(window_)
            , target(target_)
        {
        }

        ~Win32DropTarget() noexcept {
            releaseObject();
        }

        void operator delete(Win32DropTarget* drop, std::destroying_delete_t) noexcept {
            SmallObjAllocator& owner = drop->allocator;
            owner.release(drop);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) {
                *object = static_cast<IDropTarget*>(this);
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
            const ULONG left = static_cast<ULONG>(InterlockedDecrement(&references));
            if (left == 0) {
                delete this;
            }
            return left;
        }

        bool releaseObject() {
            if (object == nullptr) {
                return false;
            }
            object->Release();
            object = nullptr;
            offer = {};
            return true;
        }

        DWORD dragEffect(POINTL position, DWORD allowed) {
            POINT point = {.x = position.x, .y = position.y};
            ScreenToClient(window, &point);
            const DropReply reply = target.dragOver(offer, point.x, point.y);
            DWORD requested = DROPEFFECT_NONE;
            if ((reply.mime == utf8Mime && offer.text)
                    || (reply.mime == uriListMime && offer.files)) {
                requested = reply.action == DropAction::Move
                    ? DROPEFFECT_MOVE
                    : DROPEFFECT_COPY;
            }
            return (allowed & requested) != 0 ? requested : DROPEFFECT_NONE;
        }

        HRESULT STDMETHODCALLTYPE DragEnter(
            IDataObject* data, DWORD, POINTL position, DWORD* effect
        ) override {
            releaseObject();
            if (data != nullptr) {
                object = data;
                object->AddRef();
                offer.text = dataFormat(*object, CF_UNICODETEXT);
                offer.files = dataFormat(*object, CF_HDROP);
            }
            *effect = dragEffect(position, *effect);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragOver(
            DWORD, POINTL position, DWORD* effect
        ) override {
            *effect = dragEffect(position, *effect);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override {
            target.dragLeft();
            releaseObject();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Drop(
            IDataObject* data, DWORD, POINTL position, DWORD* effect
        ) override {
            if (data != object) {
                releaseObject();
                if (data != nullptr) {
                    object = data;
                    object->AddRef();
                    offer.text = dataFormat(*object, CF_UNICODETEXT);
                    offer.files = dataFormat(*object, CF_HDROP);
                }
            }
            *effect = dragEffect(position, *effect);
            if (*effect != DROPEFFECT_NONE && object != nullptr) {
                Win32Drop drop(allocator, *object, offer);
                target.dropped(drop);
                if (!drop.taken) {
                    *effect = DROPEFFECT_NONE;
                }
            }
            releaseObject();
            return S_OK;
        }

        SmallObjAllocator& allocator;
        HWND window;
        DropTarget& target;
        LONG references = 1;
        IDataObject* object = nullptr;
        Win32DropOffer offer;
    };

    struct WindowWin32 final: Window {
        WindowWin32(
            PlatformWin32& platform_,
            PrimaryClipboard& primary_,
            SystemClipboard& secondary_,
            InputSink* input_,
            WindowEvents* events_,
            FrameCallback* frame_,
            DropTarget* drop_
        )
            : platform(platform_)
            , primary_(primary_)
            , secondary_(secondary_)
            , input(input_)
            , events(events_)
            , frame(frame_)
            , drop(drop_)
        {
        }

        ~WindowWin32() noexcept;

        WindowWin32(const WindowWin32&) = delete;
        WindowWin32& operator=(const WindowWin32&) = delete;
        WindowWin32(WindowWin32&&) = delete;
        WindowWin32& operator=(WindowWin32&&) = delete;

        WindowWin32& initialize(const WindowOptions& options);
        WindowWin32& attach(HWND window);
        bool revokeDrop() noexcept;
        LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
        WindowInfo refreshInfo();
        u16 updateModifiers(InputKey key, bool pressed);
        u16 currentModifiers() const;
        LRESULT keyMessage(WPARAM wparam, LPARAM lparam);
        LRESULT textMessage(WPARAM wparam);
        LRESULT imeComposition(LPARAM lparam);
        LRESULT pointerMotionMessage(LPARAM lparam);
        LRESULT pointerButtonMessage(UINT message, WPARAM wparam, LPARAM lparam);
        LRESULT scrollMessage(UINT message, WPARAM wparam, LPARAM lparam);

        void requestShow() override;
        void requestClose() override;
        void requestFrame() override;
        void requestTitle(StringView title) override;
        void requestAttention() override;
        void requestRestore() override;
        void requestIconify() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        Clipboard* primary() override;
        Clipboard* secondary() override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestOpenUri(StringView uri) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        WindowInfo info() const override;
        bool inLiveResize() const override;
        RenderContext renderContext() const override;

        PlatformWin32& platform;
        PrimaryClipboard& primary_;
        SystemClipboard& secondary_;
        InputSink* input;
        WindowEvents* events;
        FrameCallback* frame;
        DropTarget* drop;
        IDropTarget* oleDropTarget = nullptr;
        HWND handle = nullptr;
        WindowInfo info_;
        WINDOWPLACEMENT placement{};
        RECT restoredRect{};
        LONG_PTR restoredStyle = 0;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeWidth = 1;
        u32 resizeHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        bool liveResize = false;
        bool restoredMaximized = false;
        bool pointerInside = false;
        u16 modifiers = 0;
        u8 shiftKeys = 0;
        u8 controlKeys = 0;
        u8 altKeys = 0;
        u8 superKeys = 0;
        bool altGraph = false;
        wchar_t highSurrogate = 0;
        HCURSOR cursor = nullptr;
    };

    struct PlatformWin32 final: Platform {
        PlatformWin32(
            PollerLoop& poller_,
            SmallObjAllocator& allocator_,
            Scheduler& scheduler_,
            PrimaryClipboard& primaryClipboard_,
            SystemClipboard& secondaryClipboard_,
            HINSTANCE instance_,
            ATOM windowClass_
        )
            : poller_(poller_)
            , allocator_(allocator_)
            , scheduler_(scheduler_)
            , primaryClipboard_(primaryClipboard_)
            , secondaryClipboard_(secondaryClipboard_)
            , instance_(instance_)
            , windowClass_(windowClass_)
        {
        }

        ~PlatformWin32() noexcept {
            UnregisterClassW(MAKEINTATOM(windowClass_), instance_);
            OleUninitialize();
        }

        PlatformWin32(const PlatformWin32&) = delete;
        PlatformWin32& operator=(const PlatformWin32&) = delete;
        PlatformWin32(PlatformWin32&&) = delete;
        PlatformWin32& operator=(PlatformWin32&&) = delete;

        bool dispatchMessages();

        void run() override;
        void stop() override;
        Poller* poller() override;
        Scheduler* scheduler() override;
        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        LoopWake* createLoopWake(ObjPool& owner, TimerCallback& callback) override;

        PollerLoop& poller_;
        SmallObjAllocator& allocator_;
        Scheduler& scheduler_;
        PrimaryClipboard& primaryClipboard_;
        SystemClipboard& secondaryClipboard_;
        HINSTANCE const instance_;
        ATOM const windowClass_;
        bool stopped_ = false;
    };
}

WindowWin32::~WindowWin32() noexcept {
    if (handle != nullptr) {
        revokeDrop();
        DestroyWindow(handle);
    }
}

bool WindowWin32::revokeDrop() noexcept {
    if (oleDropTarget == nullptr) {
        return false;
    }
    RevokeDragDrop(handle);
    RemovePropW(handle, dropTargetProperty);
    oleDropTarget->Release();
    oleDropTarget = nullptr;
    return true;
}

WindowWin32& WindowWin32::attach(HWND window) {
    STD_INSIST(handle == nullptr);
    handle = window;
    return *this;
}

WindowWin32& WindowWin32::initialize(const WindowOptions& options) {
    minimumWidth = std::max(1u, options.minimumWidth);
    minimumHeight = std::max(1u, options.minimumHeight);
    if ((GetKeyState(VK_CAPITAL) & 1) != 0) {
        modifiers |= InputCapsLock;
    }
    if ((GetKeyState(VK_NUMLOCK) & 1) != 0) {
        modifiers |= InputNumLock;
    }
    const DWORD style = options.decorations
        ? WS_OVERLAPPEDWINDOW
        : WS_POPUP | WS_THICKFRAME;
    const RECT outer = adjustedClientRect(
        style,
        0,
        std::max(1u, options.width),
        std::max(1u, options.height),
        GetDpiForSystem()
    );
    const std::wstring title = wide(options.title);
    const HWND window = CreateWindowExW(
        0,
        windowClassName,
        title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        outer.right - outer.left,
        outer.bottom - outer.top,
        nullptr,
        nullptr,
        platform.instance_,
        this
    );
    STD_INSIST(window != nullptr);
    STD_INSIST(handle == window);
    if (drop != nullptr) {
        oleDropTarget = platform.allocator_.make<Win32DropTarget>(
            platform.allocator_,
            handle,
            *drop
        );
        STD_INSIST(SetPropW(
            handle,
            dropTargetProperty,
            oleDropTarget
        ) != 0);
        STD_INSIST(RegisterDragDrop(handle, oleDropTarget) == S_OK);
    }
    refreshInfo();
    return *this;
}

WindowInfo WindowWin32::refreshInfo() {
    if (handle == nullptr) {
        return info_;
    }
    RECT client;
    RECT outer;
    STD_INSIST(GetClientRect(handle, &client) != 0);
    STD_INSIST(GetWindowRect(handle, &outer) != 0);
    info_.x = outer.left;
    info_.y = outer.top;
    info_.width = static_cast<u32>(std::max<LONG>(0, client.right - client.left));
    info_.height = static_cast<u32>(std::max<LONG>(0, client.bottom - client.top));
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(MONITORINFO);
    STD_INSIST(GetMonitorInfoW(
        MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
        &monitor
    ) != 0);
    info_.screenPixelWidth = static_cast<u32>(
        monitor.rcMonitor.right - monitor.rcMonitor.left
    );
    info_.screenPixelHeight = static_cast<u32>(
        monitor.rcMonitor.bottom - monitor.rcMonitor.top
    );
    info_.contentScale = static_cast<float>(GetDpiForWindow(handle)) / 96.0f;
    info_.focused = GetFocus() == handle;
    info_.iconified = IsIconic(handle) != 0;
    info_.maximized = IsZoomed(handle) != 0;
    return info_;
}

u16 WindowWin32::currentModifiers() const {
    u16 result = modifiers;
    if (shiftKeys != 0) {
        result |= InputShift;
    }
    if (controlKeys != 0) {
        result |= InputControl;
    }
    if (altKeys != 0) {
        result |= InputAlt;
    }
    if (superKeys != 0) {
        result |= InputSuper;
    }
    if (altGraph) {
        result &= static_cast<u16>(~(InputControl | InputAlt));
        result |= InputAltGraph;
    }
    return result;
}

u16 WindowWin32::updateModifiers(InputKey key, bool pressed) {
    const auto update = [pressed](u8& keys, u8 bit) {
        keys = pressed
            ? static_cast<u8>(keys | bit)
            : static_cast<u8>(keys & ~bit);
    };
    switch (key) {
    case InputKey::LeftShift: update(shiftKeys, 1); break;
    case InputKey::RightShift: update(shiftKeys, 2); break;
    case InputKey::LeftControl: update(controlKeys, 1); break;
    case InputKey::RightControl: update(controlKeys, 2); break;
    case InputKey::LeftAlt: update(altKeys, 1); break;
    case InputKey::RightAlt:
        update(altKeys, 2);
        altGraph = pressed && controlKeys != 0;
        break;
    case InputKey::LeftSuper: update(superKeys, 1); break;
    case InputKey::RightSuper: update(superKeys, 2); break;
    case InputKey::CapsLock:
        if (pressed) {
            modifiers ^= InputCapsLock;
        }
        break;
    case InputKey::NumLock:
        if (pressed) {
            modifiers ^= InputNumLock;
        }
        break;
    default:
        break;
    }
    return currentModifiers();
}

LRESULT WindowWin32::keyMessage(WPARAM wparam, LPARAM lparam) {
    if (input == nullptr) {
        return 0;
    }
    const bool released = (lparam & (1LL << 31)) != 0;
    const bool repeat = !released && (lparam & (1LL << 30)) != 0;
    const InputKey key = inputKey(wparam, lparam);
    const u16 flags = repeat
        ? currentModifiers()
        : updateModifiers(key, !released);
    input->key({
        .key = key,
        .action = released
            ? InputAction::Release
            : (repeat ? InputAction::Repeat : InputAction::Press),
        .modifiers = flags,
        .layoutCodepoint = key == InputKey::Printable
            ? layoutCodepoint(wparam, lparam, false)
            : 0,
        .baseCodepoint = key == InputKey::Printable
            ? baseCodepoint(wparam, false)
            : 0,
        .shiftedCodepoint = key == InputKey::Printable
            ? layoutCodepoint(wparam, lparam, true)
            : 0,
    });
    input->flush();
    return 0;
}

LRESULT WindowWin32::textMessage(WPARAM wparam) {
    if (input == nullptr) {
        return 0;
    }
    const wchar_t unit = static_cast<wchar_t>(wparam);
    if (unit >= 0xd800 && unit <= 0xdbff) {
        if (highSurrogate != 0) {
            input->text({.codepoint = 0xfffd, .modifiers = currentModifiers()});
        }
        highSurrogate = unit;
        return 0;
    }
    u32 codepoint = unit;
    if (unit >= 0xdc00 && unit <= 0xdfff) {
        if (highSurrogate == 0) {
            codepoint = 0xfffd;
        } else {
            codepoint = 0x10000
                + ((static_cast<u32>(highSurrogate) - 0xd800) << 10)
                + static_cast<u32>(unit) - 0xdc00;
            highSurrogate = 0;
        }
    } else if (highSurrogate != 0) {
        input->text({.codepoint = 0xfffd, .modifiers = currentModifiers()});
        highSurrogate = 0;
    }
    input->text({.codepoint = codepoint, .modifiers = currentModifiers()});
    input->flush();
    return 0;
}

LRESULT WindowWin32::imeComposition(LPARAM lparam) {
    const HIMC context = ImmGetContext(handle);
    if (context == nullptr || input == nullptr) {
        if (context != nullptr) {
            ImmReleaseContext(handle, context);
        }
        return 0;
    }
    if ((lparam & GCS_RESULTSTR) != 0) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring result(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            STD_INSIST(ImmGetCompositionStringW(
                context,
                GCS_RESULTSTR,
                result.data(),
                static_cast<DWORD>(bytes)
            ) == bytes);
            for (const wchar_t unit : result) {
                textMessage(unit);
            }
        }
    }
    if ((lparam & GCS_COMPSTR) != 0) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        if (bytes >= 0) {
            std::wstring composition(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            if (bytes > 0) {
                STD_INSIST(ImmGetCompositionStringW(
                    context,
                    GCS_COMPSTR,
                    composition.data(),
                    static_cast<DWORD>(bytes)
                ) == bytes);
            }
            const LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
            const std::string value = utf8(composition.data(), composition.length());
            const size_t units = cursor < 0
                ? 0
                : std::min<size_t>(static_cast<size_t>(cursor), composition.length());
            const i32 position = cursor < 0
                ? -1
                : static_cast<i32>(utf8(composition.data(), units).length());
            input->preedit(
                StringView(reinterpret_cast<const u8*>(value.data()), value.length()),
                position,
                position
            );
            input->flush();
        }
    }
    ImmReleaseContext(handle, context);
    return 0;
}

LRESULT WindowWin32::pointerMotionMessage(LPARAM lparam) {
    if (input == nullptr) {
        return 0;
    }
    if (!pointerInside) {
        TRACKMOUSEEVENT tracking = {
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = handle,
            .dwHoverTime = 0,
        };
        STD_INSIST(TrackMouseEvent(&tracking) != 0);
        pointerInside = true;
        input->pointerPresence(true);
    }
    input->pointerMotion({
        .pixelX = static_cast<short>(LOWORD(lparam)),
        .pixelY = static_cast<short>(HIWORD(lparam)),
        .modifiers = currentModifiers(),
    });
    input->flush();
    return 0;
}

LRESULT WindowWin32::pointerButtonMessage(UINT message_, WPARAM wparam, LPARAM lparam) {
    if (input == nullptr) {
        return 0;
    }
    PointerButton button;
    switch (message_) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: button = PointerButton::Primary; break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: button = PointerButton::Secondary; break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: button = PointerButton::Middle; break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        button = HIWORD(wparam) == XBUTTON1
            ? PointerButton::Auxiliary1
            : PointerButton::Auxiliary2;
        break;
    default:
        return 0;
    }
    const bool pressed = message_ == WM_LBUTTONDOWN
        || message_ == WM_RBUTTONDOWN
        || message_ == WM_MBUTTONDOWN
        || message_ == WM_XBUTTONDOWN;
    if (pressed) {
        SetCapture(handle);
    } else if ((LOWORD(wparam) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0) {
        ReleaseCapture();
    }
    input->pointerButton({
        .button = button,
        .pressed = pressed,
        .pixelX = static_cast<short>(LOWORD(lparam)),
        .pixelY = static_cast<short>(HIWORD(lparam)),
        .modifiers = currentModifiers(),
        .time = GetMessageTime() / 1000.0,
    });
    input->flush();
    return message_ == WM_XBUTTONDOWN || message_ == WM_XBUTTONUP ? TRUE : 0;
}

LRESULT WindowWin32::scrollMessage(UINT message_, WPARAM wparam, LPARAM lparam) {
    if (input == nullptr) {
        return 0;
    }
    POINT point = {
        .x = static_cast<short>(LOWORD(lparam)),
        .y = static_cast<short>(HIWORD(lparam)),
    };
    ScreenToClient(handle, &point);
    const double delta = static_cast<short>(HIWORD(wparam))
        / static_cast<double>(WHEEL_DELTA);
    input->scroll({
        .x = message_ == WM_MOUSEHWHEEL ? delta : 0.0,
        .y = message_ == WM_MOUSEWHEEL ? delta : 0.0,
        .pixelX = point.x,
        .pixelY = point.y,
        .modifiers = currentModifiers(),
        .time = GetMessageTime() / 1000.0,
    });
    input->flush();
    return 0;
}

LRESULT WindowWin32::message(UINT message_, WPARAM wparam, LPARAM lparam) {
    switch (message_) {
    case WM_CLOSE:
        if (events != nullptr) {
            events->close();
        } else {
            DestroyWindow(handle);
        }
        return 0;
    case WM_NCDESTROY:
        revokeDrop();
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        {
            const HWND destroyed = handle;
            handle = nullptr;
            return DefWindowProcW(destroyed, message_, wparam, lparam);
        }
    case WM_MOVE:
    case WM_SIZE:
        refreshInfo();
        return 0;
    case WM_SETFOCUS:
        refreshInfo();
        if (input != nullptr) {
            input->focus(true);
            input->flush();
        }
        return 0;
    case WM_KILLFOCUS:
        refreshInfo();
        shiftKeys = 0;
        controlKeys = 0;
        altKeys = 0;
        superKeys = 0;
        altGraph = false;
        highSurrogate = 0;
        if (input != nullptr) {
            input->focus(false);
            input->flush();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return keyMessage(wparam, lparam);
    case WM_CHAR:
    case WM_SYSCHAR:
        return textMessage(wparam);
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return 0;
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR) {
            return TRUE;
        }
        if (input != nullptr) {
            input->text({
                .codepoint = static_cast<u32>(wparam),
                .modifiers = currentModifiers(),
            });
            input->flush();
        }
        return 0;
    case WM_IME_COMPOSITION:
        return imeComposition(lparam);
    case WM_IME_ENDCOMPOSITION:
        highSurrogate = 0;
        if (input != nullptr) {
            input->preedit({}, -1, -1);
            input->flush();
        }
        return 0;
    case WM_MOUSEMOVE:
        return pointerMotionMessage(lparam);
    case WM_MOUSELEAVE:
        pointerInside = false;
        if (input != nullptr) {
            input->pointerPresence(false);
            input->flush();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return pointerButtonMessage(message_, wparam, lparam);
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return scrollMessage(message_, wparam, lparam);
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && cursor != nullptr) {
            SetCursor(cursor);
            return TRUE;
        }
        return DefWindowProcW(handle, message_, wparam, lparam);
    case WM_ENTERSIZEMOVE:
        liveResize = true;
        return 0;
    case WM_EXITSIZEMOVE:
        liveResize = false;
        refreshInfo();
        return 0;
    case WM_GETMINMAXINFO: {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
        const RECT minimum = adjustedClientRect(
            GetWindowLongPtrW(handle, GWL_STYLE),
            GetWindowLongPtrW(handle, GWL_EXSTYLE),
            minimumWidth,
            minimumHeight,
            GetDpiForWindow(handle)
        );
        limits.ptMinTrackSize.x = minimum.right - minimum.left;
        limits.ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_SIZING: {
        auto& sizing = *reinterpret_cast<RECT*>(lparam);
        const RECT frame = adjustedClientRect(
            GetWindowLongPtrW(handle, GWL_STYLE),
            GetWindowLongPtrW(handle, GWL_EXSTYLE),
            0,
            0,
            GetDpiForWindow(handle)
        );
        const LONG frameWidth = frame.right - frame.left;
        const LONG frameHeight = frame.bottom - frame.top;
        const u32 clientWidth = static_cast<u32>(std::max<LONG>(
            0,
            sizing.right - sizing.left - frameWidth
        ));
        const u32 clientHeight = static_cast<u32>(std::max<LONG>(
            0,
            sizing.bottom - sizing.top - frameHeight
        ));
        const LONG width = snappedSize(
            clientWidth,
            resizeWidth,
            resizeBaseWidth,
            minimumWidth
        ) + frameWidth;
        const LONG height = snappedSize(
            clientHeight,
            resizeHeight,
            resizeBaseHeight,
            minimumHeight
        ) + frameHeight;
        switch (wparam) {
        case WMSZ_LEFT:
        case WMSZ_TOPLEFT:
        case WMSZ_BOTTOMLEFT:
            sizing.left = sizing.right - width;
            break;
        case WMSZ_RIGHT:
        case WMSZ_TOPRIGHT:
        case WMSZ_BOTTOMRIGHT:
            sizing.right = sizing.left + width;
            break;
        default:
            break;
        }
        switch (wparam) {
        case WMSZ_TOP:
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            sizing.top = sizing.bottom - height;
            break;
        case WMSZ_BOTTOM:
        case WMSZ_BOTTOMLEFT:
        case WMSZ_BOTTOMRIGHT:
            sizing.bottom = sizing.top + height;
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT& suggested = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(
            handle,
            nullptr,
            suggested.left,
            suggested.top,
            suggested.right - suggested.left,
            suggested.bottom - suggested.top,
            SWP_NOACTIVATE | SWP_NOZORDER
        );
        refreshInfo();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(handle, &paint);
        if (frame != nullptr) {
            frame->frame(refreshInfo());
        }
        EndPaint(handle, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(handle, message_, wparam, lparam);
    }
}

void WindowWin32::requestShow() {
    RECT requested;
    STD_INSIST(GetWindowRect(handle, &requested) != 0);
    ShowWindow(handle, SW_SHOW);
    if (IsZoomed(handle) == 0) {
        STD_INSIST(SetWindowPos(
            handle,
            nullptr,
            requested.left,
            requested.top,
            requested.right - requested.left,
            requested.bottom - requested.top,
            SWP_NOACTIVATE | SWP_NOZORDER
        ) != 0);
    }
    refreshInfo();
    requestFrame();
}

void WindowWin32::requestClose() {
    STD_INSIST(PostMessageW(handle, WM_CLOSE, 0, 0) != 0);
}

void WindowWin32::requestFrame() {
    STD_INSIST(InvalidateRect(handle, nullptr, FALSE) != 0);
}

void WindowWin32::requestTitle(StringView title) {
    const std::wstring value = wide(title);
    STD_INSIST(SetWindowTextW(handle, value.c_str()) != 0);
}

void WindowWin32::requestAttention() {
    FLASHWINFO request = {
        .cbSize = sizeof(FLASHWINFO),
        .hwnd = handle,
        .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG,
        .uCount = 3,
        .dwTimeout = 0,
    };
    FlashWindowEx(&request);
}

void WindowWin32::requestRestore() {
    ShowWindow(handle, SW_RESTORE);
    refreshInfo();
}

void WindowWin32::requestIconify() {
    ShowWindow(handle, SW_MINIMIZE);
    refreshInfo();
}

void WindowWin32::requestMove(i32 x, i32 y) {
    STD_INSIST(SetWindowPos(
        handle,
        nullptr,
        x,
        y,
        0,
        0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
    ) != 0);
    refreshInfo();
}

void WindowWin32::requestFocus() {
    SetForegroundWindow(handle);
    SetFocus(handle);
    refreshInfo();
}

void WindowWin32::requestMaximized(bool maximized) {
    ShowWindow(handle, maximized ? SW_MAXIMIZE : SW_RESTORE);
    refreshInfo();
}

void WindowWin32::requestFullscreen(bool fullscreen) {
    if (fullscreen == info_.fullscreen) {
        return;
    }
    if (fullscreen) {
        placement.length = sizeof(WINDOWPLACEMENT);
        STD_INSIST(GetWindowPlacement(handle, &placement) != 0);
        STD_INSIST(GetWindowRect(handle, &restoredRect) != 0);
        restoredMaximized = info_.maximized;
        restoredStyle = GetWindowLongPtrW(handle, GWL_STYLE);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(MONITORINFO);
        STD_INSIST(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) != 0);
        SetWindowLongPtrW(handle, GWL_STYLE, restoredStyle & ~WS_OVERLAPPEDWINDOW);
        STD_INSIST(SetWindowPos(
            handle,
            HWND_TOP,
            monitor.rcMonitor.left,
            monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE
        ) != 0);
    } else {
        SetWindowLongPtrW(handle, GWL_STYLE, restoredStyle);
        if (restoredMaximized) {
            STD_INSIST(SetWindowPlacement(handle, &placement) != 0);
            STD_INSIST(SetWindowPos(
                handle,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
            ) != 0);
        } else {
            STD_INSIST(SetWindowPos(
                handle,
                nullptr,
                restoredRect.left,
                restoredRect.top,
                restoredRect.right - restoredRect.left,
                restoredRect.bottom - restoredRect.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER
            ) != 0);
        }
    }
    info_.fullscreen = fullscreen;
    refreshInfo();
}

void WindowWin32::requestResize(u32 width, u32 height) {
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    const RECT outer = adjustedClientRect(
        style,
        GetWindowLongPtrW(handle, GWL_EXSTYLE),
        std::max(1u, width),
        std::max(1u, height),
        GetDpiForWindow(handle)
    );
    STD_INSIST(SetWindowPos(
        handle,
        nullptr,
        0,
        0,
        outer.right - outer.left,
        outer.bottom - outer.top,
        SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER
    ) != 0);
    refreshInfo();
}

void WindowWin32::requestMinimumSize(u32 width, u32 height) {
    minimumWidth = std::max(1u, width);
    minimumHeight = std::max(1u, height);
}

void WindowWin32::requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeWidth = std::max(1u, width);
    resizeHeight = std::max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
}

Clipboard* WindowWin32::primary() {
    return &primary_;
}

Clipboard* WindowWin32::secondary() {
    return &secondary_;
}

void WindowWin32::requestPointerIcon(PointerIcon icon) {
    LPCWSTR name;
    switch (icon) {
    case PointerIcon::Default:
    case PointerIcon::ContextMenu:
    case PointerIcon::Alias:
    case PointerIcon::Copy:
    case PointerIcon::Move:
    case PointerIcon::ZoomIn:
    case PointerIcon::ZoomOut:
    case PointerIcon::DndAsk:
    case PointerIcon::DisappearingItem:
        name = IDC_ARROW;
        break;
    case PointerIcon::Help:
        name = IDC_HELP;
        break;
    case PointerIcon::Pointer:
    case PointerIcon::Grab:
    case PointerIcon::Grabbing:
        name = IDC_HAND;
        break;
    case PointerIcon::Progress:
        name = IDC_APPSTARTING;
        break;
    case PointerIcon::Wait:
        name = IDC_WAIT;
        break;
    case PointerIcon::Cell:
    case PointerIcon::Crosshair:
        name = IDC_CROSS;
        break;
    case PointerIcon::Text:
    case PointerIcon::VerticalText:
        name = IDC_IBEAM;
        break;
    case PointerIcon::NoDrop:
    case PointerIcon::NotAllowed:
        name = IDC_NO;
        break;
    case PointerIcon::ResizeEast:
    case PointerIcon::ResizeWest:
    case PointerIcon::ResizeEastWest:
    case PointerIcon::ResizeColumn:
        name = IDC_SIZEWE;
        break;
    case PointerIcon::ResizeNorth:
    case PointerIcon::ResizeSouth:
    case PointerIcon::ResizeNorthSouth:
    case PointerIcon::ResizeRow:
        name = IDC_SIZENS;
        break;
    case PointerIcon::ResizeNorthEast:
    case PointerIcon::ResizeSouthWest:
    case PointerIcon::ResizeNorthEastSouthWest:
        name = IDC_SIZENESW;
        break;
    case PointerIcon::ResizeNorthWest:
    case PointerIcon::ResizeSouthEast:
    case PointerIcon::ResizeNorthWestSouthEast:
        name = IDC_SIZENWSE;
        break;
    case PointerIcon::AllScroll:
    case PointerIcon::ResizeAll:
        name = IDC_SIZEALL;
        break;
    }
    cursor = LoadCursorW(nullptr, name);
    SetCursor(cursor);
}

void WindowWin32::requestOpenUri(StringView uri) {
    const std::wstring value = wide(uri);
    ShellExecuteW(handle, L"open", value.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void WindowWin32::requestTextInputRect(i32 x, i32 y, u32, u32 height) {
    const HIMC context = ImmGetContext(handle);
    if (context == nullptr) {
        return;
    }
    const POINT point = {
        .x = x,
        .y = y + static_cast<LONG>(height),
    };
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = point;
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = point;
    ImmSetCompositionWindow(context, &composition);
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(handle, context);
}

WindowInfo WindowWin32::info() const {
    return info_;
}

bool WindowWin32::inLiveResize() const {
    return liveResize;
}

RenderContext WindowWin32::renderContext() const {
    return {
        .backend = RenderBackend::Win32,
        .connection = platform.instance_,
        .window = handle,
    };
}

bool PlatformWin32::dispatchMessages() {
    MSG message;
    bool dispatched = false;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        dispatched = true;
        if (message.message == WM_QUIT) {
            stopped_ = true;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return dispatched;
}

void PlatformWin32::run() {
    while (!stopped_) {
        poller_.dispatchTimers();
        dispatchMessages();
        if (!stopped_) {
            poller_.wait(poller_.nextDeadline());
            dispatchMessages();
        }
    }
    stopped_ = false;
}

void PlatformWin32::stop() {
    stopped_ = true;
}

Poller* PlatformWin32::poller() {
    return &poller_;
}

Scheduler* PlatformWin32::scheduler() {
    return &scheduler_;
}

Window* PlatformWin32::createWindow(ObjPool& owner, const WindowOptions& options) {
    WindowWin32* const window = owner.make<WindowWin32>(
        *this,
        primaryClipboard_,
        secondaryClipboard_,
        options.input,
        options.events,
        options.frame,
        options.drop
    );
    return &window->initialize(options);
}

LoopWake* PlatformWin32::createLoopWake(ObjPool& owner, TimerCallback& callback) {
    return LoopWake::create(owner, poller_, callback);
}

namespace {
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    WindowWin32* target = reinterpret_cast<WindowWin32*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)
    );
    if (message == WM_NCCREATE) {
        const auto& create = *reinterpret_cast<const CREATESTRUCTW*>(lparam);
        target = static_cast<WindowWin32*>(create.lpCreateParams);
        target->attach(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(target));
    }
    return target == nullptr
        ? DefWindowProcW(window, message, wparam, lparam)
        : target->message(message, wparam, lparam);
}
}

Platform* plt::createWin32Platform(ObjPool& owner) {
    STD_INSIST(enableDpiAwareness());
    const HRESULT ole = OleInitialize(nullptr);
    STD_INSIST(ole == S_OK || ole == S_FALSE);
    HINSTANCE const instance = GetModuleHandleW(nullptr);
    STD_INSIST(instance != nullptr);
    WNDCLASSEXW description{};
    description.cbSize = sizeof(WNDCLASSEXW);
    description.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    description.lpfnWndProc = windowProcedure;
    description.hInstance = instance;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = windowClassName;
    const ATOM windowClass = RegisterClassExW(&description);
    STD_INSIST(windowClass != 0);
    PollerLoop& poller = *PollerLoop::create(owner);
    SmallObjAllocator& allocator = *SmallObjAllocator::create(&owner);
    Scheduler& scheduler = *Scheduler::create(owner, poller);
    PrimaryClipboard& primaryClipboard = *owner.make<PrimaryClipboard>(allocator);
    SystemClipboard& secondaryClipboard = *owner.make<SystemClipboard>(allocator);
    return owner.make<PlatformWin32>(
        poller,
        allocator,
        scheduler,
        primaryClipboard,
        secondaryClipboard,
        instance,
        windowClass
    );
}
