#ifndef HTTPS_SEND_DIAG_H
#define HTTPS_SEND_DIAG_H

#include <stdint.h>
#include "core/net.h"

/** Request-scoped observation only: wire bytes accepted by send(), not audio
 * bytes received by the box. Owned by the synchronous HTTP worker's stack. */
typedef struct https_send_diag
{
    Socket *socket;
    struct https_send_diag *previous;
    uint64_t id, started_ms, last_progress_ms, wire_bytes, calls, short_writes;
    int64_t last_call_ms, max_call_ms;
    size_t last_requested;
    int last_result, native_error;
    bool_t native_error_valid;
} https_send_diag_t;

void https_send_diag_begin(https_send_diag_t *diag, Socket *socket);
uint64_t https_send_diag_before(Socket *socket);
void https_send_diag_after(Socket *socket, size_t requested, int result,
                           int native_error, uint64_t started_ms);
void https_send_diag_end(https_send_diag_t *diag, error_t error,
                         uint_t http_status, size_t response_bytes, int tls_state);

#endif
