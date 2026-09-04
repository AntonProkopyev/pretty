#pragma once

#if defined(_WIN32)
    #include <stddef.h>

struct iovec {
    void* iov_base;
    size_t iov_len;
};
#else
    #include <sys/uio.h>
#endif
