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
#include <commctrl.h>
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <new>
#include <string>

using namespace plt;
using namespace stl;

namespace {
    constexpr wchar_t windowClassName[] = L"Shitty.Platform.Win32";
    constexpr wchar_t dropTargetProperty[] = L"Shitty.Win32.DropTarget";
    constexpr UINT_PTR frameTimerId = 1;
    constexpr int globalToggleHotkeyId = 0x5348;
    constexpr UINT frameDelayMilliseconds = 8;
    constexpr ULONGLONG attentionIntervalMilliseconds = 1000;
    const StringView utf8Mime(u8"text/plain;charset=utf-8");
    const StringView uriListMime(u8"text/uri-list");

    struct PlatformWin32;
    struct WindowWin32;

    enum class ChromePart { None, Tab, CloseTab, NewTab, Minimize, Maximize, CloseWindow };

    struct ChromeHit {
        ChromePart part = ChromePart::None;
        size_t index = 0;
        bool operator==(const ChromeHit&) const = default;
    };

    constexpr UINT renameTabCommand = 0x6101;
    constexpr UINT pinTabCommand = 0x6102;
    constexpr UINT newTabHereCommand = 0x6103;
    constexpr UINT closeTabCommand = 0x6104;
    constexpr UINT renameTabMessage = WM_APP + 2;
    LRESULT CALLBACK renameProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data);

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
        RECT outerRect(u32 width, u32 height, UINT dpi) const;
        UINT dpi() const;
        LONG frameHeight() const;
        LONG captionHeight() const;
        LRESULT hitTest(LPARAM position) const;
        RECT tabBounds(size_t index) const;
        RECT newTabBounds() const;
        void paintChrome(HDC dc) const;
        ChromeHit chromeHit(POINT point) const;
        size_t tabIndex(u64 identity) const;
        void chromeAction(ChromeHit hit);
        void chromeDown(POINT point, UINT button);
        void chromeUp(POINT point, UINT button);
        void chromeMotion(POINT point);
        void cancelChromePress();
        void chromeMenu(POINT point);
        void menuAction(UINT command);
        void beginRename(size_t index);
        void finishRename(bool accept);
        void updateTooltip();
        bool revokeDrop() noexcept;
        LRESULT message(HWND source, UINT message, WPARAM wparam, LPARAM lparam);
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
        void requestTabs(WindowTabs* tabs_) override;
        void requestTabsRedraw() override;
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
        WindowTabs* tabs = nullptr;
        IDropTarget* oleDropTarget = nullptr;
        HWND handle = nullptr;
        HWND chrome = nullptr;
        HWND surface = nullptr;
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
        bool frameTimerArmed = false;
        bool customFrame = false;
        bool globalHotkeyRegistered = false;
        bool chromeTracking = false;
        POINT chromePointer{-1, -1};
        ChromeHit chromePressed;
        POINT chromePressPoint{};
        UINT chromePressButton = 0;
        u64 chromePressTab = 0;
        bool chromeDragging = false;
        HWND tooltip = nullptr;
        std::wstring tooltipText;
        HWND renameEdit = nullptr;
        u64 renameTab = 0;
        u64 menuTab = 0;
        ULONGLONG lastAttention = 0;
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
        if (frameTimerArmed) {
            KillTimer(handle, frameTimerId);
        }
        revokeDrop();
        DestroyWindow(handle);
    }
}

bool WindowWin32::revokeDrop() noexcept {
    if (oleDropTarget == nullptr) {
        return false;
    }
    RevokeDragDrop(surface == nullptr ? handle : surface);
    RemovePropW(handle, dropTargetProperty);
    oleDropTarget->Release();
    oleDropTarget = nullptr;
    return true;
}

WindowWin32& WindowWin32::attach(HWND window) {
    if (handle == nullptr) {
        handle = window;
    } else if (chrome == nullptr) {
        chrome = window;
    } else {
        STD_INSIST(surface == nullptr);
        surface = window;
    }
    return *this;
}

WindowWin32& WindowWin32::initialize(const WindowOptions& options) {
    customFrame = options.decorations;
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
    const u32 width = std::max(1u, options.width);
    const u32 height = std::max(1u, options.height);
    const UINT dpi = GetDpiForSystem();
    const LONG chromeHeight = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
        + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
        + MulDiv(34, static_cast<int>(dpi), 96);
    const RECT outer = customFrame
        ? RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height) + chromeHeight}
        : adjustedClientRect(style, 0, width, height, dpi);
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
    RECT client{};
    STD_INSIST(GetClientRect(handle, &client) != 0);
    const HWND chromeWindow = CreateWindowExW(
        0, windowClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, client.right, chromeHeight,
        handle, nullptr, platform.instance_, this
    );
    STD_INSIST(chromeWindow != nullptr && chrome == chromeWindow);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    STD_INSIST(InitCommonControlsEx(&controls) != 0);
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, chrome, nullptr, platform.instance_, nullptr);
    STD_INSIST(tooltip != nullptr);
    TOOLINFOW tool{};
    // The default comctl32 activation context uses the V2 TOOLINFO size.
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = chrome;
    tool.uId = 1;
    GetClientRect(chrome, &tool.rect);
    tool.lpszText = const_cast<wchar_t*>(L"");
    STD_INSIST(SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)) != 0);
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(600, static_cast<int>(dpi), 96));
    const HWND renderSurface = CreateWindowExW(
        0,
        windowClassName,
        L"",
        WS_CHILD | WS_VISIBLE,
        0,
        chromeHeight,
        client.right,
        std::max<LONG>(1, client.bottom - chromeHeight),
        handle,
        nullptr,
        platform.instance_,
        this
    );
    STD_INSIST(renderSurface != nullptr && surface == renderSurface);
    if (options.globalToggleHotkey) {
        globalHotkeyRegistered = RegisterHotKey(
            handle,
            globalToggleHotkeyId,
            MOD_CONTROL | MOD_NOREPEAT,
            VK_OEM_3
        ) != 0;
        if (!globalHotkeyRegistered) {
            OutputDebugStringW(L"shitty: cannot register Ctrl+` global hotkey\n");
        }
    }
    if (customFrame) {
        STD_INSIST(SetWindowPos(
            handle,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
                | SWP_NOACTIVATE | SWP_NOZORDER
        ) != 0);
    }
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
        STD_INSIST(RegisterDragDrop(surface, oleDropTarget) == S_OK);
    }
    refreshInfo();
    return *this;
}

RECT WindowWin32::outerRect(u32 width, u32 height, UINT dpi) const {
    if (customFrame) {
        const LONG chromeHeight = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
            + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
            + MulDiv(34, static_cast<int>(dpi), 96);
        return {
            .left = 0,
            .top = 0,
            .right = static_cast<LONG>(width),
            .bottom = static_cast<LONG>(height) + chromeHeight,
        };
    }
    return adjustedClientRect(
        GetWindowLongPtrW(handle, GWL_STYLE),
        GetWindowLongPtrW(handle, GWL_EXSTYLE),
        width,
        height,
        dpi
    );
}

UINT WindowWin32::dpi() const {
    const UINT value = GetDpiForWindow(handle);
    return value == 0 ? 96 : value;
}

LONG WindowWin32::frameHeight() const {
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi())
        + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
}

LONG WindowWin32::captionHeight() const {
    return frameHeight()
        + MulDiv(34, static_cast<int>(dpi()), 96);
}

LRESULT WindowWin32::hitTest(LPARAM position) const {
    if (info_.fullscreen) {
        return HTCLIENT;
    }
    RECT bounds{};
    if (GetWindowRect(handle, &bounds) == 0) {
        return HTNOWHERE;
    }
    const POINT point = {
        .x = static_cast<short>(LOWORD(position)),
        .y = static_cast<short>(HIWORD(position)),
    };
    const LONG frame = frameHeight();
    const bool left = point.x < bounds.left + frame;
    const bool right = point.x >= bounds.right - frame;
    const bool top = point.y < bounds.top + frame;
    const bool bottom = point.y >= bounds.bottom - frame;
    if (IsZoomed(handle) == 0) {
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    if (point.y < bounds.top + captionHeight()) {
        const LONG button = std::max<LONG>(46, MulDiv(46, static_cast<int>(dpi()), 96));
        if (point.x >= bounds.right - button) return HTCLOSE;
        if (point.x >= bounds.right - 2 * button) return HTMAXBUTTON;
        if (point.x >= bounds.right - 3 * button) return HTMINBUTTON;
    }
    return point.y < bounds.top + captionHeight() ? HTCAPTION : HTCLIENT;
}

RECT WindowWin32::tabBounds(size_t index) const {
    RECT client{};
    GetClientRect(chrome, &client);
    const int scale = static_cast<int>(dpi());
    const LONG left = MulDiv(12, scale, 96);
    const LONG controls = client.right - 3 * MulDiv(46, scale, 96);
    const LONG available = std::max<LONG>(0, controls - left - MulDiv(44, scale, 96));
    const size_t count = tabs == nullptr ? 0 : tabs->count();
    // ponytail: O(n) per tab; cache the pinned prefix if hundreds of tabs become common.
    size_t pins = 0;
    for (size_t at = 0; at != count; ++at) {
        pins += tabs->pinned(at);
    }
    const LONG pinWidth = count == 0 ? 0 : std::min<LONG>(MulDiv(44, scale, 96), available / static_cast<LONG>(count));
    const LONG pinnedWidth = static_cast<LONG>(pins) * pinWidth;
    const LONG width = count == pins ? 0 : std::min<LONG>(MulDiv(240, scale, 96), (available - pinnedWidth) / static_cast<LONG>(count - pins));
    const LONG start = index < pins ? left + static_cast<LONG>(index) * pinWidth : left + pinnedWidth + static_cast<LONG>(index - pins) * width;
    return {start, frameHeight(), start + (index < pins ? pinWidth : width), client.bottom - MulDiv(2, scale, 96)};
}

RECT WindowWin32::newTabBounds() const {
    const size_t count = tabs == nullptr ? 0 : tabs->count();
    RECT bounds = tabBounds(count == 0 ? 0 : count - 1);
    const int scale = static_cast<int>(dpi());
    bounds.left = bounds.right + MulDiv(8, scale, 96);
    bounds.right = bounds.left + MulDiv(28, scale, 96);
    return bounds;
}

void WindowWin32::paintChrome(HDC target) const {
    if (!customFrame || handle == nullptr) {
        return;
    }
    RECT bounds{};
    if (chrome == nullptr || GetClientRect(chrome, &bounds) == 0) {
        return;
    }
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const HDC dc = CreateCompatibleDC(target);
    if (dc == nullptr) {
        return;
    }
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width * 2, height * 2);
    if (bitmap == nullptr) {
        DeleteDC(dc);
        return;
    }
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(dc);
        return;
    }
    // Supersample this small GDI strip to smooth the curved tab shoulders.
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, width, height, nullptr);
    SetViewportExtEx(dc, width * 2, height * 2, nullptr);
    const WindowColor background = tabs == nullptr
        ? WindowColor{38, 50, 56}
        : tabs->background();
    const WindowColor foreground = tabs == nullptr
        ? WindowColor{236, 239, 241}
        : tabs->foreground();
    const auto tone = [&](int amount) {
        return RGB(
            (background.red * (100 - amount) + foreground.red * amount) / 100,
            (background.green * (100 - amount) + foreground.green * amount) / 100,
            (background.blue * (100 - amount) + foreground.blue * amount) / 100
        );
    };
    const COLORREF bg = RGB(background.red, background.green, background.blue);
    const COLORREF fg = RGB(foreground.red, foreground.green, foreground.blue);
    const HBRUSH backgroundBrush = CreateSolidBrush(bg);
    RECT bar{0, 0, width, height};
    FillRect(dc, &bar, backgroundBrush);
    DeleteObject(backgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    const int scale = static_cast<int>(dpi());
    const int fontHeight = -MulDiv(10, scale, 72);
    const HFONT font = CreateFontW(
        fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI"
    );
    const HGDIOBJ previousFont = SelectObject(dc, font);
    const LONG button = std::max<LONG>(46, MulDiv(46, static_cast<int>(dpi()), 96));
    const LONG controlsLeft = width - 3 * button;
    const HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
    const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const LONG radius = MulDiv(10, scale, 96);
    const LONG icon = MulDiv(12, scale, 96);
    const LONG iconRadius = MulDiv(10, scale, 96);
    if (tabs != nullptr && tabs->count() != 0 && controlsLeft > 48) {
        const size_t count = tabs->count();
        const size_t active = tabs->active();
        for (size_t at = 0; at != count; ++at) {
            const RECT cell = tabBounds(at);
            if (at != active && PtInRect(&cell, chromePointer)) {
                SetDCBrushColor(dc, tone(4));
                const LONG inset = MulDiv(2, scale, 96);
                RoundRect(dc, cell.left + inset, cell.top + inset, cell.right - inset, cell.bottom - inset, 2 * radius, 2 * radius);
            }
            if (at + 1 != count && at != active && at + 1 != active) {
                SetDCBrushColor(dc, tone(28));
                const LONG middle = (cell.top + cell.bottom) / 2;
                RoundRect(dc, cell.right, middle - icon / 2, cell.right + std::max(1, MulDiv(1, scale, 96)), middle + icon / 2, 2, 2);
            }
        }
        const RECT selected = tabBounds(active);
        if (selected.right > selected.left) {
            const LONG r = std::min<LONG>(radius, (selected.right - selected.left) / 3);
            const LONG bend = MulDiv(r, 55, 100);
            BeginPath(dc);
            MoveToEx(dc, selected.left - r, selected.bottom, nullptr);
            const POINT lowerLeft[]{{selected.left - r + bend, selected.bottom}, {selected.left, selected.bottom - r + bend}, {selected.left, selected.bottom - r}};
            PolyBezierTo(dc, lowerLeft, 3);
            LineTo(dc, selected.left, selected.top + r);
            const POINT upperLeft[]{{selected.left, selected.top + r - bend}, {selected.left + r - bend, selected.top}, {selected.left + r, selected.top}};
            PolyBezierTo(dc, upperLeft, 3);
            LineTo(dc, selected.right - r, selected.top);
            const POINT upperRight[]{{selected.right - r + bend, selected.top}, {selected.right, selected.top + r - bend}, {selected.right, selected.top + r}};
            PolyBezierTo(dc, upperRight, 3);
            LineTo(dc, selected.right, selected.bottom - r);
            const POINT lowerRight[]{{selected.right, selected.bottom - r + bend}, {selected.right + r - bend, selected.bottom}, {selected.right + r, selected.bottom}};
            PolyBezierTo(dc, lowerRight, 3);
            CloseFigure(dc);
            EndPath(dc);
            SetDCBrushColor(dc, tone(7));
            FillPath(dc);
            RECT seam{0, selected.bottom, width, height};
            FillRect(dc, &seam, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        const HPEN iconPen = CreatePen(PS_SOLID, std::max(1, MulDiv(1, scale, 96)), fg);
        SelectObject(dc, iconPen);
        for (size_t at = 0; at != count; ++at) {
            const RECT cell = tabBounds(at);
            if (cell.right <= cell.left) {
                continue;
            }
            const LONG centerX = cell.right - MulDiv(16, scale, 96);
            const LONG centerY = (cell.top + cell.bottom) / 2;
            RECT close{centerX - iconRadius, centerY - iconRadius, centerX + iconRadius, centerY + iconRadius};
            if (!tabs->pinned(at) && cell.right - cell.left >= MulDiv(48, scale, 96) && PtInRect(&close, chromePointer)) {
                SelectObject(dc, GetStockObject(NULL_PEN));
                SetDCBrushColor(dc, tone(14));
                Ellipse(dc, close.left, close.top, close.right, close.bottom);
                SelectObject(dc, iconPen);
            }
            RECT label = cell;
            label.left += MulDiv(12, scale, 96);
            label.right -= MulDiv(32, scale, 96);
            const std::wstring text = wide(tabs->title(at));
            if (tabs->pinned(at)) {
                const int length = text.size() > 1 && IS_HIGH_SURROGATE(text[0]) ? 2 : std::min<int>(1, text.size());
                RECT iconLabel = cell;
                DrawTextW(dc, text.c_str(), length, &iconLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                DrawTextW(dc, text.c_str(), static_cast<int>(text.length()), &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (!tabs->pinned(at) && cell.right - cell.left >= MulDiv(48, scale, 96)) {
                const LONG half = MulDiv(4, scale, 96);
                MoveToEx(dc, centerX - half, centerY - half, nullptr);
                LineTo(dc, centerX + half, centerY + half);
                MoveToEx(dc, centerX + half, centerY - half, nullptr);
                LineTo(dc, centerX - half, centerY + half);
            }
        }
        const RECT add = newTabBounds();
        const LONG centerX = (add.left + add.right) / 2;
        const LONG centerY = (add.top + add.bottom) / 2;
        if (PtInRect(&add, chromePointer)) {
            SelectObject(dc, GetStockObject(NULL_PEN));
            SetDCBrushColor(dc, tone(7));
            Ellipse(dc, add.left, centerY - (add.right - add.left) / 2, add.right, centerY + (add.right - add.left) / 2);
            SelectObject(dc, iconPen);
        }
        MoveToEx(dc, centerX - icon / 2, centerY, nullptr);
        LineTo(dc, centerX + icon / 2, centerY);
        MoveToEx(dc, centerX, centerY - icon / 2, nullptr);
        LineTo(dc, centerX, centerY + icon / 2);
        SelectObject(dc, GetStockObject(NULL_PEN));
        DeleteObject(iconPen);
    }
    const HBRUSH controlBrush = CreateSolidBrush(fg);
    const LONG middle = height / 2;
    const LONG minimize = controlsLeft + button / 2;
    RECT stroke{minimize - 6, middle + 4, minimize + 6, middle + 5};
    FillRect(dc, &stroke, controlBrush);
    const LONG maximize = controlsLeft + button + button / 2;
    RECT top{maximize - 6, middle - 6, maximize + 6, middle - 5};
    RECT bottom{maximize - 6, middle + 5, maximize + 6, middle + 6};
    RECT left{maximize - 6, middle - 6, maximize - 5, middle + 6};
    RECT right{maximize + 5, middle - 6, maximize + 6, middle + 6};
    FillRect(dc, &top, controlBrush);
    FillRect(dc, &bottom, controlBrush);
    FillRect(dc, &left, controlBrush);
    FillRect(dc, &right, controlBrush);
    const LONG close = controlsLeft + 2 * button + button / 2;
    for (LONG offset = -5; offset <= 5; ++offset) {
        RECT descending{close + offset, middle + offset, close + offset + 2, middle + offset + 2};
        RECT ascending{close + offset, middle - offset, close + offset + 2, middle - offset + 2};
        FillRect(dc, &descending, controlBrush);
        FillRect(dc, &ascending, controlBrush);
    }
    DeleteObject(controlBrush);
    SelectObject(dc, previousFont);
    DeleteObject(font);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    // Publish the completed strip so title updates never expose the cleared background.
    SetMapMode(dc, MM_TEXT);
    const int previousStretch = SetStretchBltMode(target, HALFTONE);
    POINT previousOrigin{};
    SetBrushOrgEx(target, 0, 0, &previousOrigin);
    StretchBlt(target, 0, 0, width, height, dc, 0, 0, width * 2, height * 2, SRCCOPY);
    SetBrushOrgEx(target, previousOrigin.x, previousOrigin.y, nullptr);
    SetStretchBltMode(target, previousStretch);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

ChromeHit WindowWin32::chromeHit(POINT point) const {
    RECT bounds{};
    if (chrome == nullptr || GetClientRect(chrome, &bounds) == 0) {
        return {};
    }
    const LONG x = point.x;
    const LONG y = point.y;
    if (y < frameHeight() || y >= bounds.bottom - bounds.top) {
        return {};
    }
    const LONG width = bounds.right - bounds.left;
    const LONG button = std::max<LONG>(46, MulDiv(46, static_cast<int>(dpi()), 96));
    const LONG controlsLeft = width - 3 * button;
    if (x >= controlsLeft && x < width) {
        if (x >= width - button) {
            return {ChromePart::CloseWindow};
        } else if (x >= width - 2 * button) {
            return {ChromePart::Maximize};
        } else {
            return {ChromePart::Minimize};
        }
    }
    if (tabs == nullptr || tabs->count() == 0) {
        return {};
    }
    const RECT add = newTabBounds();
    if (PtInRect(&add, point)) {
        return {ChromePart::NewTab};
    }
    const size_t count = tabs->count();
    for (size_t index = 0; index != count; ++index) {
        const RECT cell = tabBounds(index);
        if (!PtInRect(&cell, point)) {
            continue;
        }
        const int scale = static_cast<int>(dpi());
        if (!tabs->pinned(index) && cell.right - cell.left >= MulDiv(48, scale, 96) && x >= cell.right - MulDiv(28, scale, 96)) {
            return {ChromePart::CloseTab, index};
        } else {
            return {ChromePart::Tab, index};
        }
    }
    return {};
}

size_t WindowWin32::tabIndex(u64 identity) const {
    const size_t count = tabs == nullptr ? 0 : tabs->count();
    for (size_t index = 0; index != count; ++index) {
        if (tabs->identity(index) == identity) {
            return index;
        }
    }
    return count;
}

void WindowWin32::chromeAction(ChromeHit hit) {
    switch (hit.part) {
    case ChromePart::Tab: tabs->select(hit.index); break;
    case ChromePart::CloseTab: tabs->close(hit.index); break;
    case ChromePart::NewTab: tabs->open(); break;
    case ChromePart::Minimize: requestIconify(); break;
    case ChromePart::Maximize: requestMaximized(IsZoomed(handle) == 0); break;
    case ChromePart::CloseWindow: requestClose(); break;
    case ChromePart::None: break;
    }
}

void WindowWin32::cancelChromePress() {
    chromePressed = {};
    chromePressTab = 0;
    chromePressButton = 0;
    chromeDragging = false;
    requestTabsRedraw();
}

void WindowWin32::chromeDown(POINT point, UINT button) {
    finishRename(true);
    SetFocus(surface);
    chromePressed = chromeHit(point);
    if (chromePressed.part == ChromePart::None) {
        if (button == WM_LBUTTONDOWN) {
            ClientToScreen(chrome, &point);
            SendMessageW(handle, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(point.x, point.y));
        }
        return;
    }
    chromePressPoint = point;
    chromePressButton = button;
    chromeDragging = false;
    chromePressTab = chromePressed.part == ChromePart::Tab || chromePressed.part == ChromePart::CloseTab ? tabs->identity(chromePressed.index) : 0;
    SetCapture(chrome);
    SendMessageW(tooltip, TTM_POP, 0, 0);
    if (button == WM_LBUTTONDOWN && chromePressed.part == ChromePart::Tab) {
        tabs->select(chromePressed.index);
    }
    requestTabsRedraw();
}

void WindowWin32::chromeUp(POINT point, UINT button) {
    const ChromeHit released = chromeHit(point);
    const ChromeHit pressed = chromePressed;
    const UINT pressedButton = chromePressButton;
    const bool dragging = chromeDragging;
    const u64 identity = chromePressTab;
    cancelChromePress();
    if (GetCapture() == chrome) {
        ReleaseCapture();
    }
    if (dragging || pressedButton != button) {
        return;
    }
    if (identity != 0) {
        if (released.part != ChromePart::Tab && released.part != ChromePart::CloseTab) {
            return;
        }
        if (tabs->identity(released.index) != identity) {
            return;
        }
        if (button == WM_MBUTTONDOWN || (pressed.part == ChromePart::CloseTab && released.part == ChromePart::CloseTab)) {
            tabs->close(released.index);
        }
    } else if (button == WM_LBUTTONDOWN && released == pressed) {
        chromeAction(released);
    }
}

void WindowWin32::chromeMotion(POINT point) {
    chromePointer = point;
    if (tabs != nullptr && chromePressButton == WM_LBUTTONDOWN && chromePressed.part == ChromePart::Tab) {
        chromeDragging = chromeDragging || std::abs(static_cast<int>(point.x - chromePressPoint.x)) >= GetSystemMetrics(SM_CXDRAG) || std::abs(static_cast<int>(point.y - chromePressPoint.y)) >= GetSystemMetrics(SM_CYDRAG);
        const ChromeHit over = chromeHit(point);
        const size_t from = tabIndex(chromePressTab);
        if (chromeDragging && from < tabs->count() && (over.part == ChromePart::Tab || over.part == ChromePart::CloseTab) && tabs->pinned(from) == tabs->pinned(over.index)) {
            tabs->move(from, over.index);
        }
    }
    updateTooltip();
    requestTabsRedraw();
}

void WindowWin32::updateTooltip() {
    if (tooltip == nullptr || chrome == nullptr) {
        return;
    }
    const ChromeHit hit = chromeHit(chromePointer);
    std::wstring text;
    switch (hit.part) {
    case ChromePart::Tab:
        text = wide(tabs->title(hit.index));
        if (!tabs->directory(hit.index).empty()) {
            text += L"\n" + wide(tabs->directory(hit.index));
        }
        break;
    case ChromePart::CloseTab: text = L"Close tab (Ctrl+Shift+W)"; break;
    case ChromePart::NewTab: text = L"New tab (Ctrl+Shift+T)"; break;
    case ChromePart::Minimize: text = L"Minimize"; break;
    case ChromePart::Maximize: text = IsZoomed(handle) ? L"Restore" : L"Maximize"; break;
    case ChromePart::CloseWindow: text = L"Close window (Alt+F4)"; break;
    case ChromePart::None: break;
    }
    if (text != tooltipText) {
        SendMessageW(tooltip, TTM_POP, 0, 0);
        tooltipText = text;
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.hwnd = chrome;
        tool.uId = 1;
        tool.lpszText = tooltipText.data();
        SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void WindowWin32::beginRename(size_t index) {
    finishRename(false);
    renameTab = tabs->identity(index);
    RECT cell = tabBounds(index);
    cell.right = std::max<LONG>(cell.right, cell.left + MulDiv(180, static_cast<int>(dpi()), 96));
    const std::wstring title = wide(tabs->title(index));
    renameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", title.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, cell.left + 4, cell.top, cell.right - cell.left - 8, cell.bottom - cell.top, chrome, nullptr, platform.instance_, nullptr);
    if (renameEdit == nullptr) {
        return;
    }
    SetWindowSubclass(renameEdit, renameProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(renameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(renameEdit, EM_SETLIMITTEXT, 256, 0);
    SendMessageW(renameEdit, EM_SETSEL, 0, -1);
    SetFocus(renameEdit);
}

void WindowWin32::finishRename(bool accept) {
    if (renameEdit == nullptr) {
        return;
    }
    const HWND edit = renameEdit;
    renameEdit = nullptr;
    const size_t index = tabIndex(renameTab);
    if (accept && tabs != nullptr && index < tabs->count()) {
        wchar_t text[257]{};
        const int length = GetWindowTextW(edit, text, 257);
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, nullptr, 0, nullptr, nullptr);
        if (length == 0 || bytes > 0) {
            std::string title(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, title.data(), bytes, nullptr, nullptr);
            tabs->rename(index, StringView(reinterpret_cast<const u8*>(title.data()), title.size()));
        }
    }
    DestroyWindow(edit);
    requestTabsRedraw();
}

namespace {
    LRESULT CALLBACK renameProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
        auto* const owner = reinterpret_cast<WindowWin32*>(data);
        if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
            owner->finishRename(wparam == VK_RETURN);
            SetFocus(owner->surface);
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, renameProcedure, id);
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }
}

void WindowWin32::chromeMenu(POINT point) {
    finishRename(true);
    const ChromeHit hit = chromeHit(point);
    if (hit.part != ChromePart::Tab && hit.part != ChromePart::CloseTab) {
        return;
    }
    menuTab = tabs->identity(hit.index);
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, renameTabCommand, L"Rename tab");
    AppendMenuW(menu, MF_STRING, pinTabCommand, tabs->pinned(hit.index) ? L"Unpin tab" : L"Pin tab");
    AppendMenuW(menu, MF_STRING | (tabs->directory(hit.index).empty() ? MF_GRAYED : 0), newTabHereCommand, L"New tab in this folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, closeTabCommand, L"Close tab\tCtrl+Shift+W");
    ClientToScreen(chrome, &point);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, point.x, point.y, handle, nullptr);
    DestroyMenu(menu);
}

void WindowWin32::menuAction(UINT selected) {
    const size_t index = tabIndex(menuTab);
    if (tabs == nullptr || index >= tabs->count()) {
        return;
    }
    switch (selected) {
    case renameTabCommand:
        renameTab = menuTab;
        PostMessageW(handle, renameTabMessage, 0, 0);
        break;
    case pinTabCommand: tabs->pin(index, !tabs->pinned(index)); break;
    case newTabHereCommand:
        if (!tabs->openNear(index)) {
            MessageBoxW(handle, L"Could not open a tab in this folder.", L"New tab", MB_OK | MB_ICONERROR);
        }
        break;
    case closeTabCommand: tabs->close(index); break;
    default: break;
    }
}

WindowInfo WindowWin32::refreshInfo() {
    if (handle == nullptr) {
        return info_;
    }
    RECT client;
    RECT outer;
    STD_INSIST(GetClientRect(surface == nullptr ? handle : surface, &client) != 0);
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
    info_.contentScale = static_cast<float>(dpi()) / 96.0f;
    info_.focused = GetFocus() == (surface == nullptr ? handle : surface);
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
    const bool textExpected = !released
        && (key == InputKey::Printable || key == InputKey::Space)
        && (((flags & (InputControl | InputSuper)) == 0)
            || (flags & InputAltGraph) != 0);
    if (!textExpected) {
        input->flush();
    }
    return 0;
}

LRESULT WindowWin32::textMessage(WPARAM wparam) {
    if (input == nullptr) {
        return 0;
    }
    const u16 flags = currentModifiers();
    if ((flags & (InputControl | InputSuper)) != 0
            && (flags & InputAltGraph) == 0) {
        return 0;
    }
    const wchar_t unit = static_cast<wchar_t>(wparam);
    if (unit < 0x20 || unit == 0x7f) {
        return 0;
    }
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
    input->text({.codepoint = codepoint, .modifiers = flags});
    input->flush();
    return 0;
}

LRESULT WindowWin32::imeComposition(LPARAM lparam) {
    const HWND inputWindow = surface == nullptr ? handle : surface;
    const HIMC context = ImmGetContext(inputWindow);
    if (context == nullptr || input == nullptr) {
        if (context != nullptr) {
            ImmReleaseContext(inputWindow, context);
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
    ImmReleaseContext(inputWindow, context);
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
            .hwndTrack = surface == nullptr ? handle : surface,
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
    ScreenToClient(surface == nullptr ? handle : surface, &point);
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

LRESULT WindowWin32::message(HWND source, UINT message_, WPARAM wparam, LPARAM lparam) {
    switch (message_) {
    case renameTabMessage:
        if (tabs != nullptr && tabIndex(renameTab) < tabs->count()) {
            beginRename(tabIndex(renameTab));
        }
        return 0;
    case WM_COMMAND:
        if (lparam == 0 && LOWORD(wparam) >= renameTabCommand && LOWORD(wparam) <= closeTabCommand) {
            menuAction(LOWORD(wparam));
            return 0;
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_NCCALCSIZE:
        return source == handle && customFrame
            ? 0
            : DefWindowProcW(source, message_, wparam, lparam);
    case WM_NCHITTEST:
        if (customFrame) {
            const LRESULT result = hitTest(lparam);
            if (source == handle
                    || result == HTLEFT || result == HTRIGHT
                    || result == HTTOP || result == HTBOTTOM
                    || result == HTTOPLEFT || result == HTTOPRIGHT
                    || result == HTBOTTOMLEFT || result == HTBOTTOMRIGHT) {
                return result;
            }
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_NCPAINT:
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_NCACTIVATE:
        if (source == handle) {
            InvalidateRect(handle, nullptr, FALSE);
            return TRUE;
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_NCLBUTTONDOWN:
        if (source != handle
                && (wparam == HTLEFT || wparam == HTRIGHT
                    || wparam == HTTOP || wparam == HTBOTTOM
                    || wparam == HTTOPLEFT || wparam == HTTOPRIGHT
                    || wparam == HTBOTTOMLEFT || wparam == HTBOTTOMRIGHT)) {
            return SendMessageW(handle, message_, wparam, lparam);
        }
        if (source == handle && (wparam == HTCAPTION || wparam == HTCLOSE || wparam == HTMAXBUTTON || wparam == HTMINBUTTON)) {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ScreenToClient(chrome, &point);
            if (chromeHit(point).part != ChromePart::None) {
                chromeDown(point, WM_LBUTTONDOWN);
                return 0;
            }
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_CLOSE:
        if (events != nullptr) {
            events->close();
        } else {
            DestroyWindow(handle);
        }
        return 0;
    case WM_HOTKEY:
        if (wparam != globalToggleHotkeyId) {
            return DefWindowProcW(source, message_, wparam, lparam);
        }
        if (IsWindowVisible(handle) != 0) {
            ShowWindow(handle, SW_HIDE);
        } else {
            ShowWindow(handle, IsIconic(handle) != 0 ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(handle);
            SetFocus(surface == nullptr ? handle : surface);
            refreshInfo();
            requestFrame();
        }
        return 0;
    case WM_NCDESTROY:
        if (source != handle) {
            SetWindowLongPtrW(source, GWLP_USERDATA, 0);
            if (source == chrome) {
                tooltip = nullptr;
                renameEdit = nullptr;
                chrome = nullptr;
            } else if (source == surface) {
                surface = nullptr;
            }
            return DefWindowProcW(source, message_, wparam, lparam);
        }
        if (globalHotkeyRegistered) {
            UnregisterHotKey(handle, globalToggleHotkeyId);
            globalHotkeyRegistered = false;
        }
        revokeDrop();
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        {
            const HWND destroyed = handle;
            handle = nullptr;
            return DefWindowProcW(destroyed, message_, wparam, lparam);
        }
    case WM_MOVE:
    case WM_SIZE:
        if (source == handle && surface != nullptr) {
            RECT client{};
            if (GetClientRect(handle, &client) != 0) {
                MoveWindow(chrome, 0, 0, client.right, captionHeight(), TRUE);
                MoveWindow(surface, 0, captionHeight(), client.right, std::max<LONG>(1, client.bottom - captionHeight()), TRUE);
            }
        }
        refreshInfo();
        requestTabsRedraw();
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
        if (input != nullptr) {
            input->flush();
        }
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
        if (source == chrome) {
            chromeMotion({static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))});
            if (!chromeTracking) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, chrome, 0};
                chromeTracking = TrackMouseEvent(&tracking) != 0;
            }
            requestTabsRedraw();
            return 0;
        }
        return pointerMotionMessage(lparam);
    case WM_MOUSELEAVE:
        if (source == chrome) {
            chromeTracking = false;
            chromePointer = {-1, -1};
            requestTabsRedraw();
            return 0;
        }
        pointerInside = false;
        if (input != nullptr) {
            input->pointerPresence(false);
            input->flush();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (source == chrome) {
            chromeDown({static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))}, WM_LBUTTONDOWN);
            return 0;
        }
        finishRename(true);
    case WM_LBUTTONUP:
        if (source == chrome) {
            chromeUp({static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))}, WM_LBUTTONDOWN);
            return 0;
        }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (source == chrome) {
            const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (message_ == WM_MBUTTONDOWN) {
                chromeDown(point, WM_MBUTTONDOWN);
            } else if (message_ == WM_MBUTTONUP) {
                chromeUp(point, WM_MBUTTONDOWN);
            } else if (message_ == WM_RBUTTONUP) {
                chromeMenu(point);
            }
            return 0;
        }
        return pointerButtonMessage(message_, wparam, lparam);
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (source == chrome) {
            cancelChromePress();
            return 0;
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_CONTEXTMENU:
        if (source == chrome || source == handle) {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (lparam == -1 && tabs != nullptr && tabs->count() != 0) {
                const RECT cell = tabBounds(tabs->active());
                point = {cell.left + 8, cell.bottom - 1};
            } else {
                ScreenToClient(chrome, &point);
            }
            chromeMenu(point);
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return scrollMessage(message_, wparam, lparam);
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && cursor != nullptr) {
            SetCursor(cursor);
            return TRUE;
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    case WM_ENTERSIZEMOVE:
        liveResize = true;
        return 0;
    case WM_EXITSIZEMOVE:
        liveResize = false;
        refreshInfo();
        return 0;
    case WM_GETMINMAXINFO: {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
        const RECT minimum = outerRect(
            minimumWidth,
            minimumHeight,
            dpi()
        );
        limits.ptMinTrackSize.x = minimum.right - minimum.left;
        limits.ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_SIZING: {
        auto& sizing = *reinterpret_cast<RECT*>(lparam);
        const RECT frame = outerRect(
            0,
            0,
            dpi()
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
    case WM_TIMER:
        if (wparam != frameTimerId) {
            return DefWindowProcW(handle, message_, wparam, lparam);
        }
        KillTimer(handle, frameTimerId);
        frameTimerArmed = false;
        STD_INSIST(InvalidateRect(surface == nullptr ? handle : surface, nullptr, FALSE) != 0);
        return 0;
    case WM_PAINT: {
        if (source == surface && frameTimerArmed) {
            KillTimer(handle, frameTimerId);
            frameTimerArmed = false;
        }
        PAINTSTRUCT paint;
        BeginPaint(source, &paint);
        if (source == chrome) {
            paintChrome(paint.hdc);
        } else if (source == surface && frame != nullptr) {
            frame->frame(refreshInfo());
        }
        EndPaint(source, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (source == chrome) {
            paintChrome(reinterpret_cast<HDC>(wparam));
            return 0;
        }
        return DefWindowProcW(source, message_, wparam, lparam);
    default:
        return DefWindowProcW(source, message_, wparam, lparam);
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

void WindowWin32::requestTabs(WindowTabs* tabs_) {
    tabs = tabs_;
    requestTabsRedraw();
}

void WindowWin32::requestTabsRedraw() {
    if (handle != nullptr && customFrame) {
        if (chrome != nullptr) {
            if (tooltip != nullptr) {
                TOOLINFOW tool{};
                tool.cbSize = TTTOOLINFOW_V2_SIZE;
                tool.hwnd = chrome;
                tool.uId = 1;
                GetClientRect(chrome, &tool.rect);
                SendMessageW(tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                updateTooltip();
            }
            if (renameEdit != nullptr && (tabs == nullptr || tabIndex(renameTab) == tabs->count())) {
                finishRename(false);
            }
            InvalidateRect(chrome, nullptr, FALSE);
        }
    }
}

void WindowWin32::requestClose() {
    STD_INSIST(PostMessageW(handle, WM_CLOSE, 0, 0) != 0);
}

void WindowWin32::requestFrame() {
    if (frameTimerArmed) {
        return;
    }
    frameTimerArmed = true;
    STD_INSIST(SetTimer(handle, frameTimerId, frameDelayMilliseconds, nullptr) == frameTimerId);
}

void WindowWin32::requestTitle(StringView title) {
    const std::wstring value = wide(title);
    STD_INSIST(SetWindowTextW(handle, value.c_str()) != 0);
}

void WindowWin32::requestAttention() {
    if (GetForegroundWindow() == handle) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (lastAttention != 0 && now - lastAttention < attentionIntervalMilliseconds) {
        return;
    }
    lastAttention = now;
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
    SetFocus(renameEdit != nullptr ? renameEdit : (surface == nullptr ? handle : surface));
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
    const RECT outer = outerRect(
        std::max(1u, width),
        std::max(1u, height),
        dpi()
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
    const HWND inputWindow = surface == nullptr ? handle : surface;
    const HIMC context = ImmGetContext(inputWindow);
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
    ImmReleaseContext(inputWindow, context);
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
        .window = surface == nullptr ? handle : surface,
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
        : target->message(window, message, wparam, lparam);
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
    description.hIcon = reinterpret_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(1),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR | LR_SHARED
    ));
    description.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(1),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED
    ));
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
