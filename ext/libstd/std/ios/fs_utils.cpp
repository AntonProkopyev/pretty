#include "fs_utils.h"

#include "in_fd.h"

#include <std/sys/fd.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>

#include <errno.h>
#include <fcntl.h>
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <io.h>
    #include <climits>
    #include <string>
#else
    #include <unistd.h>
#endif

using namespace stl;

void stl::readFileContent(Buffer& path, Buffer& out) {
#if defined(_WIN32)
    const StringView encoded(path);
    if (encoded.empty() || encoded.length() > INT_MAX) {
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"invalid file path ") << encoded);
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.length()),
        nullptr,
        0
    );
    if (length <= 0) {
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"invalid UTF-8 file path ") << encoded);
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.length()),
            wide.data(),
            length
        ) != length) {
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"failed to convert file path ") << encoded);
    }
    const int rawFd = ::_wopen(wide.c_str(), _O_RDONLY | _O_BINARY);
#else
    int rawFd = ::open(path.cStr(), O_RDONLY);
#endif

    if (rawFd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"can not open ") << path);
    }

    ScopedFD fd(rawFd);
    FDInput(fd).readAll(out);
}
