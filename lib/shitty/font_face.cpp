/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font_face.h"

#include <std/lib/buffer.h>
#include <std/ptr/arc.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/throw.h>

#include <errno.h>
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace stl;

namespace {
    struct CountedFontFace: public FontFace {
        u32 id() const noexcept override;
        void ref() noexcept override;
        i32 unref() noexcept override;
        i32 refCount() const noexcept override;

        const u32 id_ = nextFontFaceId();
        ARC arc_;
    };

    struct MemoryFontFace final: public CountedFontFace {
        MemoryFontFace(const void* data, size_t size, i32 faceIndex);

        const void* data() const override;
        size_t size() const override;
        i32 faceIndex() const override;

        const void* data_;
        size_t size_;
        i32 faceIndex_;
    };

    struct OwnedFontFace final: public CountedFontFace {
        OwnedFontFace(Buffer&& data, i32 faceIndex);

        const void* data() const override;
        size_t size() const override;
        i32 faceIndex() const override;

        Buffer data_;
        i32 faceIndex_;
    };

#if defined(_WIN32)
    struct MappedFontFace final: public CountedFontFace {
        MappedFontFace(
            HANDLE file,
            HANDLE mapping,
            void* data,
            size_t size,
            i32 faceIndex
        );
        ~MappedFontFace() noexcept override;

        const void* data() const override;
        size_t size() const override;
        i32 faceIndex() const override;

        HANDLE file_;
        HANDLE mapping_;
        void* data_;
        size_t size_;
        i32 faceIndex_;
    };
#else
    struct MmapFontFace final: public CountedFontFace {
        MmapFontFace(void* data, size_t size, i32 faceIndex);
        ~MmapFontFace() noexcept override;

        const void* data() const override;
        size_t size() const override;
        i32 faceIndex() const override;

        void* data_;
        size_t size_;
        i32 faceIndex_;
    };
#endif
}

u32 nextFontFaceId() noexcept {
    static u32 counter = 0;
    return counter++;
}

FontFace::~FontFace() noexcept {
}

u32 CountedFontFace::id() const noexcept {
    return id_;
}

void CountedFontFace::ref() noexcept {
    arc_.ref();
}

i32 CountedFontFace::unref() noexcept {
    return arc_.unref();
}

i32 CountedFontFace::refCount() const noexcept {
    return arc_.refCount();
}

MemoryFontFace::MemoryFontFace(const void* data, size_t size, i32 faceIndex)
    : data_(data)
    , size_(size)
    , faceIndex_(faceIndex)
{
}

const void* MemoryFontFace::data() const {
    return data_;
}

size_t MemoryFontFace::size() const {
    return size_;
}

i32 MemoryFontFace::faceIndex() const {
    return faceIndex_;
}

OwnedFontFace::OwnedFontFace(Buffer&& data, i32 faceIndex)
    : data_(static_cast<Buffer&&>(data))
    , faceIndex_(faceIndex)
{
}

const void* OwnedFontFace::data() const {
    return data_.data();
}

size_t OwnedFontFace::size() const {
    return data_.length();
}

i32 OwnedFontFace::faceIndex() const {
    return faceIndex_;
}

#if defined(_WIN32)
MappedFontFace::MappedFontFace(
    HANDLE file,
    HANDLE mapping,
    void* data,
    size_t size,
    i32 faceIndex
)
    : file_(file)
    , mapping_(mapping)
    , data_(data)
    , size_(size)
    , faceIndex_(faceIndex)
{
}

MappedFontFace::~MappedFontFace() noexcept {
    UnmapViewOfFile(data_);
    CloseHandle(mapping_);
    CloseHandle(file_);
}

const void* MappedFontFace::data() const {
    return data_;
}

size_t MappedFontFace::size() const {
    return size_;
}

i32 MappedFontFace::faceIndex() const {
    return faceIndex_;
}
#else
MmapFontFace::MmapFontFace(void* data, size_t size, i32 faceIndex)
    : data_(data)
    , size_(size)
    , faceIndex_(faceIndex)
{
}

MmapFontFace::~MmapFontFace() noexcept {
    munmap(data_, size_);
}

const void* MmapFontFace::data() const {
    return data_;
}

size_t MmapFontFace::size() const {
    return size_;
}

i32 MmapFontFace::faceIndex() const {
    return faceIndex_;
}
#endif

FontFace* createMemoryFontFace(const void* data, size_t size, i32 faceIndex) {
    return new MemoryFontFace(data, size, faceIndex);
}

FontFace* createOwnedFontFace(Buffer&& data, i32 faceIndex) {
    return new OwnedFontFace(static_cast<Buffer&&>(data), faceIndex);
}

FontFace* openFontFile(StringView path, i32 faceIndex) {
#if defined(_WIN32)
    if (path.empty() || path.length() > INT_MAX) {
        Errno(EINVAL).raise(StringView(u8"invalid font path"));
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(path.data()),
        static_cast<int>(path.length()),
        nullptr,
        0
    );
    if (length <= 0) {
        Errno(EINVAL).raise(StringView(u8"invalid UTF-8 font path"));
    }
    Buffer name((static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    name.seekAbsolute((static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    auto* const wide = static_cast<wchar_t*>(name.mutData());
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(path.data()),
            static_cast<int>(path.length()),
            wide,
            length
        ) != length) {
        Errno(EINVAL).raise(StringView(u8"failed to convert font path"));
    }
    wide[length] = L'\0';
    const HANDLE file = CreateFileW(
        wide,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        Errno(ENOENT).raise(StringBuilder() << StringView(u8"failed to open font ") << path);
    }
    LARGE_INTEGER measured{};
    if (GetFileSizeEx(file, &measured) == 0
            || measured.QuadPart <= 0
            || static_cast<u64>(measured.QuadPart) > SIZE_MAX) {
        CloseHandle(file);
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"failed to measure font ") << path);
    }
    const HANDLE mapping = CreateFileMappingW(
        file,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr
    );
    if (mapping == nullptr) {
        CloseHandle(file);
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"failed to map font ") << path);
    }
    void* const data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (data == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"failed to map font ") << path);
    }
    return new MappedFontFace(
        file,
        mapping,
        data,
        static_cast<size_t>(measured.QuadPart),
        faceIndex
    );
#else
    Buffer filename(path);
    const int fd = open(filename.cStr(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"failed to open font ") << path);
    }
    struct stat status {};
    if (fstat(fd, &status) != 0 || status.st_size <= 0) {
        const int error = errno;
        close(fd);
        Errno(error == 0 ? EINVAL : error).raise(StringBuilder() << StringView(u8"failed to measure font ") << path);
    }
    void* const data = mmap(nullptr, (size_t)(status.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    const int error = errno;
    close(fd);
    if (data == MAP_FAILED) {
        Errno(error).raise(StringBuilder() << StringView(u8"failed to map font ") << path);
    }
    return new MmapFontFace(data, (size_t)(status.st_size), faceIndex);
#endif
}
