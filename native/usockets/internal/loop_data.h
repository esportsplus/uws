#ifndef LOOP_DATA_H
#define LOOP_DATA_H

struct us_paused_listen_socket_t {
    struct us_listen_socket_t *listen_socket;
    struct us_paused_listen_socket_t *next;
};

struct us_internal_loop_data_t {
    struct us_timer_t *sweep_timer;
    struct us_timer_t *accept_timer;
    struct us_paused_listen_socket_t *paused_listen_sockets;
    struct us_internal_async *wakeup_async;
    int last_write_failed;
    struct us_socket_context_t *head;
    struct us_socket_context_t *iterator;
    char *recv_buf;
    void *ssl_data;
    void (*pre_cb)(struct us_loop_t *);
    void (*post_cb)(struct us_loop_t *);
    struct us_socket_t *closed_head;
    struct us_socket_t *low_prio_head;
    int low_prio_budget;
    int pending_resolves;
    /* We do not care if this flips or not, it doesn't matter */
    long long iteration_nr;
};

#endif // LOOP_DATA_H
