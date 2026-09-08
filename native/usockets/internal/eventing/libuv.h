#ifndef LIBUV_H
#define LIBUV_H

#include "internal/loop_data.h"

#include <uv.h>
#define LIBUS_SOCKET_READABLE UV_READABLE
#define LIBUS_SOCKET_WRITABLE UV_WRITABLE

struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;

    uv_loop_t *uv_loop;

    uv_prepare_t *uv_pre;
    uv_check_t *uv_check;
};

// it is no longer valid to cast a pointer to us_poll_t to a pointer of uv_poll_t
struct us_poll_t {
    /* We need to hold a pointer to this uv_poll_t since we need to be able to resize our block */
    uv_poll_t *uv_p;
    LIBUS_SOCKET_DESCRIPTOR fd;
    unsigned char poll_type;
};

#endif // LIBUV_H
