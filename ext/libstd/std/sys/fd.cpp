#include "fd.h"

#include <std/alg/xchg.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/str/builder.h>
#include <std/alg/exchange.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
    #include <sys/uio.h>
#endif

using namespace stl;

size_t FD::read(void* data, size_t len) {
#if defined(_WIN32)
    const auto res = ::_read(fd, data, static_cast<unsigned>(min(len, (size_t)(INT_MAX))));
#else
    const auto res = ::read(fd, data, min(len, (size_t)(0x7ffff000 - 1)));
#endif
    if (res >= 0) {
        return res;
    }

    if (errno == EFAULT && len > 1024) {
        return read(data, len / 2);
    }

    Errno().raise(StringBuilder() << StringView(u8"read() failed"));
}

size_t FD::write(const void* data, size_t len) {
#if defined(_WIN32)
    const auto res = ::_write(fd, data, static_cast<unsigned>(min(len, (size_t)(INT_MAX))));
#else
    const auto res = ::write(fd, data, len);
#endif

    if (res < 0) {
        Errno().raise(StringBuilder() << StringView(u8"write() failed"));
    }

    return res;
}

size_t FD::writeV(iovec* parts, size_t count) {
#if defined(_WIN32)
    size_t total = 0;
    for (size_t index = 0; index != count; ++index) {
        const size_t written = write(parts[index].iov_base, parts[index].iov_len);
        total += written;
        if (written != parts[index].iov_len) {
            break;
        }
    }
    return total;
#else
    const auto res = writev(fd, parts, count);

    if (res < 0) {
        Errno().raise(StringBuilder() << StringView(u8"writev() failed"));
    }

    return res;
#endif
}

void FD::setNonBlocking() {
#if !defined(_WIN32)
    if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"fcntl(O_NONBLOCK) failed"));
    }
#endif
}

void FD::fsync() {
#if defined(_WIN32)
    if (::_commit(fd) < 0) {
#else
    if (::fsync(fd) < 0) {
#endif
        Errno().raise(StringBuilder() << StringView(u8"fsync() failed"));
    }
}

void FD::close() {
    if (fd < 0) {
        return;
    }

#if defined(_WIN32)
    if (::_close(exchange(fd, -1)) < 0) {
#else
    if (::close(exchange(fd, -1)) < 0) {
#endif
        Errno().raise(StringBuilder() << StringView(u8"close() failed"));
    }
}

void FD::xchg(FD& other) noexcept {
    ::stl::xchg(fd, other.fd);
}

ScopedFD::~ScopedFD() {
    close();
}

FD ScopedFD::release() noexcept {
    FD tmp;
    xchg(tmp);
    return tmp;
}

void stl::createPipeFD(ScopedFD& in, ScopedFD& out) {
    int fd[2];

#if defined(_WIN32)
    const auto res = _pipe(fd, 4096, _O_BINARY);
#else
    const auto res = pipe(fd);
#endif
    if (res < 0) {
        Errno().raise(StringBuilder() << StringView(u8"pipe() failed"));
    }

    ScopedFD(fd[0]).xchg(in);
    ScopedFD(fd[1]).xchg(out);
}
