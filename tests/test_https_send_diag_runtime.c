/* Real production functions; only send(), clock and log output are controlled. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include "https_send_diag.h"
#include "debug.h"

static _Thread_local uint64_t now_ms = 1000, send_duration;
static _Thread_local int send_result, send_error, send_calls;
static _Thread_local bool clock_fails;
static _Thread_local char last_log[2048];

static int fake_clock_gettime(clockid_t clock_id, struct timespec *sample)
{
    assert(clock_id == CLOCK_MONOTONIC);
    errno = EDOM; /* Deliberately clobber errno: diagnostics must restore it. */
    if (clock_fails)
        return -1;
    sample->tv_sec = (time_t)(now_ms / 1000);
    sample->tv_nsec = (long)(now_ms % 1000) * 1000000;
    return 0;
}

static ssize_t fake_send(int descriptor, const void *data, size_t size, int flags)
{
    assert(descriptor == 17 || descriptor == 18);
    assert(data != NULL && size > 0 && flags == MSG_NOSIGNAL);
    send_calls++;
    now_ms += send_duration;
    if (send_result < 0)
        errno = send_error;
    return send_result;
}

static void capture_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
}

#undef TRACE_INFO
#define TRACE_INFO(...) capture_log(__VA_ARGS__)
#define clock_gettime fake_clock_gettime
/* PRODUCTION_DIAGNOSTICS */
#undef clock_gettime
#define send fake_send
/* PRODUCTION_SOCKET_SEND */
#undef send

/* Pre-instrumentation Linux socketSend semantics (DEV 03e5d785), retained here
 * independently so the test also remains useful after the new code is committed. */
static error_t original_socket_send(Socket *socket, const void *data,
                                    size_t length, size_t *written)
{
    if (!length)
        return NO_ERROR;
    int_t n = fake_send(socket->descriptor, data, length, MSG_NOSIGNAL);
    if (n > 0)
    {
        if (written)
            *written = n;
        return NO_ERROR;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? ERROR_TIMEOUT : ERROR_WRITE_FAILED;
}

static void compare_transport(Socket *socket, int result, int native_error,
                              int initial_errno, size_t length)
{
    send_result = result;
    send_error = native_error;
    send_duration = 23;
    send_calls = 0;
    size_t old_written = 777, new_written = 777;
    errno = initial_errno;
    error_t expected = original_socket_send(socket, "packet", length, &old_written);
    int expected_errno = errno;
    int expected_calls = send_calls;
    https_send_diag_t diag;
    https_send_diag_begin(&diag, socket);
    send_calls = 0;
    errno = initial_errno;
    error_t actual = socketSend(socket, "packet", length, &new_written, 0);
    assert(actual == expected && errno == expected_errno);
    assert(new_written == old_written && send_calls == expected_calls);
    assert(diag.calls == (uint64_t)expected_calls);
    assert(diag.wire_bytes == (length && result > 0 ? (unsigned)result : 0));
    assert(diag.native_error_valid == (length && result < 0));
    assert(diag.native_error == (length && result < 0 ? native_error : 0));
    https_send_diag_end(&diag, actual, 200, length, 0);
}

static void *other_thread(void *parameter)
{
    Socket *socket = parameter;
    send_result = 3;
    size_t written;
    /* Same Socket* in another thread must not touch the parent's context. */
    assert(socketSend(socket, "packet", 3, &written, 0) == NO_ERROR);
    https_send_diag_t own;
    https_send_diag_begin(&own, socket);
    assert(socketSend(socket, "packet", 3, &written, 0) == NO_ERROR);
    assert(own.calls == 1 && own.wire_bytes == 3);
    https_send_diag_end(&own, NO_ERROR, 200, 3, 0);
    return NULL;
}

int main(void)
{
    Socket socket = {0}, unrelated = {0};
    socket.descriptor = 17;
    unrelated.descriptor = 18;
    compare_transport(&socket, 7, 0, EAGAIN, 20);
    compare_transport(&socket, -1, EAGAIN, 0, 20);
    compare_transport(&socket, -1, EWOULDBLOCK, 0, 20);
    compare_transport(&socket, -1, ECONNRESET, 0, 20);
    compare_transport(&socket, -1, EINTR, 0, 20);
    compare_transport(&socket, 0, 0, EAGAIN, 20);
    compare_transport(&socket, 0, 0, EPIPE, 20);
    compare_transport(&socket, -1, ECONNRESET, EPIPE, 0);

    now_ms = 1000;
    https_send_diag_t diag;
    https_send_diag_begin(&diag, &socket);
    size_t written = 0;
    send_result = 7;
    send_duration = 50;
    assert(socketSend(&socket, "packet", 20, &written, 0) == NO_ERROR);
    assert(written == 7 && diag.last_progress_ms == 1050);
    send_result = -1;
    send_error = EAGAIN;
    send_duration = 200;
    assert(socketSend(&socket, "packet", 13, &written, 0) == ERROR_TIMEOUT);
    assert(diag.last_progress_ms == 1050 && diag.native_error == EAGAIN);
    send_result = 13;
    send_duration = 25;
    assert(socketSend(&socket, "packet", 13, &written, 0) == NO_ERROR);
    assert(diag.calls == 3 && diag.wire_bytes == 20 && diag.short_writes == 1);
    assert(diag.last_call_ms == 25 && diag.max_call_ms == 200);
    assert(!diag.native_error_valid && diag.native_error == 0);
    https_send_diag_end(&diag, NO_ERROR, 206, 20, 0);
    assert(strstr(last_log, "elapsed_ms=275 ") != NULL);
    assert(strstr(last_log, "last_progress_age_ms=0 ") != NULL);
    assert(strstr(last_log, "wire_bytes=20 ") != NULL);
    assert(socketSend(&socket, "packet", 13, &written, 0) == NO_ERROR);
    assert(diag.calls == 3); /* End detached the stack-owned context. */

    https_send_diag_t next, nested;
    https_send_diag_begin(&next, &socket);
    assert(next.id != diag.id && next.calls == 0);
    assert(socketSend(&unrelated, "packet", 13, &written, 0) == NO_ERROR);
    assert(next.calls == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, other_thread, &socket) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(next.calls == 0);
    https_send_diag_begin(&nested, &unrelated);
    assert(nested.id > next.id + 1); /* The worker also got a unique ID. */
    assert(socketSend(&unrelated, "packet", 13, &written, 0) == NO_ERROR);
    https_send_diag_end(&nested, NO_ERROR, 200, 13, 0);
    assert(socketSend(&socket, "packet", 13, &written, 0) == NO_ERROR);
    assert(next.calls == 1 && nested.calls == 1);
    send_result = -1;
    send_error = EPIPE;
    assert(socketSend(&socket, "packet", 13, &written, 0) == ERROR_WRITE_FAILED);
    send_result = 0;
    errno = EAGAIN;
    assert(socketSend(&socket, "packet", 13, &written, 0) == ERROR_TIMEOUT);
    assert(!next.native_error_valid && next.native_error == 0);
    https_send_diag_end(&next, ERROR_TIMEOUT, 200, 13, 0);

    clock_fails = true;
    https_send_diag_begin(&diag, &socket);
    send_result = -1;
    send_error = ECONNRESET;
    assert(socketSend(&socket, "packet", 13, &written, 0) == ERROR_WRITE_FAILED);
    assert(errno == ECONNRESET && diag.native_error == ECONNRESET);
    assert(diag.last_call_ms == -1 && diag.max_call_ms == -1);
    https_send_diag_end(&diag, ERROR_WRITE_FAILED, 200, 13, 0);
    assert(strstr(last_log, "elapsed_ms=-1 ") != NULL);
    assert(strstr(last_log, "last_progress_age_ms=-1 ") != NULL);
    puts("PASS HTTPS diagnostics: transport parity, partial writes, native errors, counters, time, socket/thread isolation and cleanup");
    return 0;
}
