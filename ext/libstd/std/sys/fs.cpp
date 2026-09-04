#include "fs.h"

#include <std/alg/defer.h>
#include <std/sys/throw.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>

#include <errno.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <string>
#else
    #include <dirent.h>
    #include <sys/types.h>
#endif

using namespace stl;

void stl::listDirImpl(StringView path, VisitorFace&& vis) {
#if defined(_WIN32)
    const int wideLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(path.data()),
        static_cast<int>(path.length()),
        nullptr,
        0
    );
    if (wideLength <= 0) {
        Errno(EINVAL).raise(StringBuilder() << StringView(u8"invalid directory path ") << path);
    }
    std::wstring pattern(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(path.data()),
        static_cast<int>(path.length()),
        pattern.data(),
        wideLength
    );
    if (!pattern.empty() && pattern.back() != L'/' && pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        Errno(ENOENT).raise(StringBuilder() << StringView(u8"FindFirstFileW failed for ") << path);
    }
    struct Search final {
        ~Search() noexcept {
            FindClose(value);
        }
        HANDLE value;
    } owner{search};
    do {
        if (entry.cFileName[0] == L'.'
                && (entry.cFileName[1] == L'\0'
                    || (entry.cFileName[1] == L'.' && entry.cFileName[2] == L'\0'))) {
            continue;
        }
        const int bytes = WideCharToMultiByte(
            CP_UTF8,
            0,
            entry.cFileName,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
        );
        std::string name(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            entry.cFileName,
            -1,
            name.data(),
            bytes,
            nullptr,
            nullptr
        );
        name.pop_back();
        TPathInfo info = {
            .item = StringView(name.c_str()),
            .isDir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        };
        vis.visit(&info);
    } while (FindNextFileW(search, &entry) != 0);
#else
    DIR* dir = opendir(Buffer(path).cStr());

    if (!dir) {
        Errno().raise(StringBuilder() << StringView(u8"opendir() failed for ") << path);
    }

    STD_DEFER {
        if (closedir(dir) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"closedir() failed for ") << path);
        }
    };

    while (struct dirent* entry = readdir(dir)) {
        StringView name((const char*)entry->d_name);

        if (name == StringView(u8".") || name == StringView(u8"..")) {
            continue;
        }

        TPathInfo pi = {
            .item = name,
            .isDir = entry->d_type == DT_DIR,
        };

        // name points into entry->d_name, which is valid until the next readdir call —
        // vis.visit() completes before that, so no dangling reference here.
        vis.visit(&pi);
    }
#endif
}
