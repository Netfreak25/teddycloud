#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "https_send_diag.h"
#include "debug.h"

static _Thread_local https_send_diag_t *active_send;
static atomic_uint_fast64_t transfer_sequence;

static uint64_t https_send_diag_now(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec sample;
    if (clock_gettime(CLOCK_MONOTONIC, &sample) != 0)
        return 0;
    return (uint64_t)sample.tv_sec * 1000U + (uint64_t)sample.tv_nsec / 1000000U;
#endif
}

static int64_t https_send_diag_elapsed(uint64_t start, uint64_t end)
{
    return start != 0 && end >= start ? (int64_t)(end - start) : -1;
}

void https_send_diag_begin(https_send_diag_t *diag, Socket *socket)
{
    memset(diag, 0, sizeof(*diag));
    diag->socket = socket;
    diag->previous = active_send;
    diag->id = atomic_fetch_add_explicit(&transfer_sequence, 1, memory_order_relaxed) + 1;
    diag->started_ms = https_send_diag_now();
    diag->last_call_ms = -1;
    diag->max_call_ms = -1;
    active_send = diag;
}

uint64_t https_send_diag_before(Socket *socket)
{
    return active_send != NULL && active_send->socket == socket ? https_send_diag_now() : 0;
}

/** No logging/I/O here; retain the native error before any clock or log call. */
void https_send_diag_after(Socket *socket, size_t requested, int result,
                           int native_error, uint64_t started_ms)
{
    https_send_diag_t *diag = active_send;
    if (diag == NULL || diag->socket != socket)
        return;
    uint64_t now = https_send_diag_now();
    int64_t elapsed = https_send_diag_elapsed(started_ms, now);
    diag->calls++;
    diag->last_requested = requested;
    diag->last_result = result;
    diag->last_call_ms = elapsed;
    if (diag->last_call_ms > diag->max_call_ms)
        diag->max_call_ms = diag->last_call_ms;
    diag->native_error_valid = result < 0;
    diag->native_error = result < 0 ? native_error : 0;
    if (result > 0)
    {
        diag->wire_bytes += (unsigned)result;
        diag->last_progress_ms = now;
        if ((size_t)result < requested)
            diag->short_writes++;
    }
    // send()==0 has no defined errno: never attach a stale native error to it.
}

void https_send_diag_end(https_send_diag_t *diag, error_t error,
                         uint_t http_status, size_t response_bytes, int tls_state)
{
    uint64_t now = https_send_diag_now();
    active_send = diag->previous;
#ifdef _WIN32
    const char *native_kind = "winsock";
#else
    const char *native_kind = "errno";
#endif
    // Only complete response emission is success; wire_bytes is not a receipt.
    TRACE_INFO("HTTPS chapter end transfer=%" PRIu64 " result=%d http_status=%u response_bytes=%" PRIuSIZE
               " wire_bytes=%" PRIu64 " send_calls=%" PRIu64 " short_writes=%" PRIu64
               " elapsed_ms=%" PRId64 " last_progress_age_ms=%" PRId64
               " last_requested=%" PRIuSIZE " last_result=%d last_call_ms=%" PRId64
               " max_call_ms=%" PRId64 " native_kind=%s native_valid=%u native_error=%d tls_state=%d\r\n",
               diag->id, error, http_status, response_bytes, diag->wire_bytes, diag->calls,
               diag->short_writes, https_send_diag_elapsed(diag->started_ms, now),
               https_send_diag_elapsed(diag->last_progress_ms, now), diag->last_requested,
               diag->last_result, diag->last_call_ms, diag->max_call_ms, native_kind,
               (unsigned)diag->native_error_valid, diag->native_error, tls_state);
}
