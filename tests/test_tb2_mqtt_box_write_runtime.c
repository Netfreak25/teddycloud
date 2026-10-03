/* Actual production/vendor function bodies are inserted by the Python runner. */
#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#define clock_gettime test_clock_gettime
/* PRODUCTION_DECLARATIONS */
#undef clock_gettime
#include "tls_record.h"
#include "tls_record_encryption.h"
#include "tls_misc.h"
#include "tls_handshake.h"
#undef TRACE_DEBUG
#undef TRACE_DEBUG_ARRAY
#undef TRACE_INFO
#undef TRACE_WARNING
#undef TRACE_ERROR
#define TRACE_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_DEBUG_ARRAY(...) do {} while (0)
#define TRACE_INFO(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_ERROR(...) do { if (0) printf(__VA_ARGS__); } while (0)
/* PRODUCTION_PROTOTYPES */

#define TEST_FRAGMENT_BYTES 4U
#define TEST_CIPHER_TRAILER_BYTES 4U
#define TEST_CIPHER_MASK 0xa5U
#define TEST_MAX_STEPS 8U
#define TEST_WIRE_BYTES 1024U

typedef struct {
    size_t accepted;
    error_t error;
    uint32_t elapsed;
    bool_t close_context;
} send_step_t;

static tb2_mqtt_passthrough_session_t session;
static settings_t settings;
static TlsContext tls;
static uint8_t tx_buffer[256], wire[TEST_WIRE_BYTES];
static send_step_t steps[TEST_MAX_STEPS];
static size_t step_count, send_calls, wire_length, encryptions;
static unsigned tls_calls, boundary_violation;
static uint32_t monotonic_now, wall_now;
static bool_t clock_available;
static bool_t control_allowed, control_closes;
static cJSON *last_status;

void mqtt_debug_test_reset(bool active, uint64_t now);
unsigned mqtt_debug_test_count(const char *stage);
const cJSON *mqtt_debug_test_last(const char *stage);

settings_t *get_settings(void) { return &settings; }
void osAcquireMutex(OsMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void osReleaseMutex(OsMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{
    (void)type;
    connection->response.contentLength = length;
}
error_t httpWriteResponse(HttpConnection *connection, void *data, size_t size, bool_t free_memory)
{
    (void)connection;
    assert(size == strlen(data));
    cJSON_Delete(last_status);
    last_status = cJSON_Parse(data);
    assert(last_status);
    if (free_memory) free(data);
    return NO_ERROR;
}

int test_clock_gettime(clockid_t clock_id, struct timespec *sample)
{
    assert(clock_id == CLOCK_MONOTONIC);
    if (!clock_available) return -1;
    sample->tv_sec = monotonic_now / 1000U;
    sample->tv_nsec = (monotonic_now % 1000U) * 1000000U;
    return 0;
}

systime_t osGetSystemTime(void) { return wall_now; }

error_t tlsConnect(TlsContext *context)
{
    assert(control_allowed && !session.box_write_active);
    if (control_closes) { context->state = TLS_STATE_CLOSED; return ERROR_TIMEOUT; }
    /* Installed client/server FSM first flushes pending control data, even in
     * APPLICATION_DATA; retain the real record-layer implementation here. */
    return tlsWriteProtocolData(context, NULL, 0, TLS_TYPE_NONE);
}

error_t tlsSendAlert(TlsContext *context, uint8_t level, uint8_t description)
{
    (void)description;
    assert(level == TLS_ALERT_LEVEL_FATAL);
    context->fatalAlertSent = TRUE;
    context->state = TLS_STATE_CLOSED;
    return NO_ERROR;
}

/* Deliberately not a crypto implementation: stable ciphertext + overhead makes
 * re-encryption, skipped bytes and duplicate records observable byte-for-byte. */
error_t tlsEncryptRecord(TlsContext *context, TlsEncryptionEngine *engine, void *data)
{
    (void)context; (void)engine;
    TlsRecord *record = data;
    size_t length = ntohs(record->length);
    for (size_t i = 0; i < length; i++) record->data[i] ^= TEST_CIPHER_MASK;
    memset(record->data + length, 0x5a, TEST_CIPHER_TRAILER_BYTES);
    record->length = htons(length + TEST_CIPHER_TRAILER_BYTES);
    encryptions++;
    return NO_ERROR;
}

static error_t send_callback(TlsSocketHandle handle, const void *data, size_t length,
                             size_t *written, uint_t flags)
{
    assert(handle == &tls && flags == 0);
    *written = 0;
    if (!session.box_io_ops || !session.box_io_bytes) return ERROR_WOULD_BLOCK;
    session.box_io_ops--;
    length = MIN(length, session.box_io_bytes);
    send_step_t step = { .accepted = length, .error = NO_ERROR };
    if (send_calls < step_count) step = steps[send_calls];
    send_calls++;
    *written = MIN(length, step.accepted);
    assert(wire_length + *written <= sizeof(wire));
    memcpy(wire + wire_length, data, *written);
    wire_length += *written;
    session.box_io_bytes -= *written;
    monotonic_now += step.elapsed;
    if (*written) {
        session.box_last_progress = monotonic_now;
        session.box_progress_clock = clock_available;
    }
    wall_now = send_calls % 2 ? UINT32_MAX - 100U : 1U;
    if (step.close_context) tls.state = TLS_STATE_CLOSED;
    return step.error;
}

static error_t receive_callback(TlsSocketHandle handle, void *data, size_t size,
                                size_t *received, uint_t flags)
{
    (void)handle; (void)data; (void)size; (void)received; (void)flags;
    assert(!"No read may be interleaved with a pending write");
    return ERROR_READ_FAILED;
}

/* VENDOR_FUNCTIONS */

/* Defensive application branches cannot arise from the configured vendor TLS
 * implementation. Inject only those explicit contract violations here; every
 * ordinary transport/partial-record case calls the actual tlsWrite above. */
static error_t test_tls_write(TlsContext *context, const void *data, size_t length,
                              size_t *written, uint_t flags)
{
    tls_calls++;
    if (boundary_violation) {
        *written = boundary_violation == 1 ? 0 : length + 1;
        return NO_ERROR;
    }
    return tlsWrite(context, data, length, written, flags);
}
#define tlsWrite test_tls_write
/* PRODUCTION_FUNCTIONS */
#undef tlsWrite

static void initialize(void)
{
    memset(&session, 0, sizeof(session));
    memset(&settings, 0, sizeof(settings));
    memset(&tls, 0, sizeof(tls));
    memset(steps, 0, sizeof(steps));
    memset(tx_buffer, 0, sizeof(tx_buffer));
    step_count = send_calls = wire_length = encryptions = 0;
    tls_calls = boundary_violation = 0;
    monotonic_now = wall_now = 1000;
    clock_available = TRUE;
    control_allowed = control_closes = FALSE;
    settings.commonName = "test-box";
    settings.internal.overlayNumber = 7;
    session.box_tls = &tls;
    session.box_settings = &settings;
    session.owner = 17;
    atomic_init(&session.box_write_error, NO_ERROR);
    atomic_init(&session.upstream_failed, FALSE);
    atomic_init(&session.capture_failed, FALSE);
    tls.state = TLS_STATE_APPLICATION_DATA;
    tls.version = TLS_VERSION_1_2;
    tls.transportProtocol = TLS_TRANSPORT_PROTOCOL_STREAM;
    tls.txBuffer = tx_buffer;
    tls.txBufferSize = sizeof(tx_buffer);
    tls.txBufferMaxLen = TEST_FRAGMENT_BYTES;
    tls.maxFragLen = TEST_FRAGMENT_BYTES;
    tls.recordSizeLimit = TEST_FRAGMENT_BYTES;
    tls.encryptionEngine.recordSizeLimit = TEST_FRAGMENT_BYTES;
    tls.encryptionEngine.cipherMode = CIPHER_MODE_GCM;
    tls.socketHandle = &tls;
    tls.socketSendCallback = send_callback;
    tls.socketReceiveCallback = receive_callback;
}

static void expect_wire(const uint8_t *payload, size_t length)
{
    size_t position = 0, offset = 0, records = 0;
    while (offset < length) {
        size_t count = MIN(length - offset, TEST_FRAGMENT_BYTES);
        const uint8_t header[] = {TLS_TYPE_APPLICATION_DATA, 3, 3, 0,
                                 (uint8_t)(count + TEST_CIPHER_TRAILER_BYTES)};
        assert(position + sizeof(header) + count + TEST_CIPHER_TRAILER_BYTES <= wire_length);
        assert(memcmp(wire + position, header, sizeof(header)) == 0);
        position += sizeof(header);
        for (size_t i = 0; i < count; i++)
            assert(wire[position++] == (uint8_t)(payload[offset + i] ^ TEST_CIPHER_MASK));
        for (size_t i = 0; i < TEST_CIPHER_TRAILER_BYTES; i++) assert(wire[position++] == 0x5a);
        offset += count;
        records++;
    }
    assert(position == wire_length && encryptions == records);
    assert(tls.txBufferLen == 0 && tls.txRecordLen == 0);
    assert(tb2_mqtt_passthrough_box_write_error(&session) == NO_ERROR);
}

static error_t tick(const uint8_t *data, size_t length, size_t *offset)
{
    session.box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    session.box_io_bytes = TB2_MQTT_BOX_IO_BYTES_PER_TICK;
    return tb2_mqtt_tls_write_step(&session, data, length, offset);
}

static void test_retry_and_buffered_ciphertext(void)
{
    const uint8_t payload[] = "abcdefghijk";
    for (unsigned variant = 0; variant < 2; variant++) {
        initialize();
        steps[0] = (send_step_t){0, variant ? ERROR_WOULD_BLOCK : ERROR_TIMEOUT, 500, FALSE};
        step_count = 1;
        size_t offset = 0;
        assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
        assert(offset == 0 && tls.txRecordLen > 0 && tls.txRecordPos == 0);
        assert(tb2_mqtt_passthrough_box_write_error(&session) == NO_ERROR);
        assert(tick(payload, 4, &offset) == NO_ERROR && offset == 4);
        assert(send_calls == 2);
        expect_wire(payload, 4);
    }
    initialize();
    steps[0] = (send_step_t){SIZE_MAX, NO_ERROR, 0, FALSE};
    steps[1] = (send_step_t){3, NO_ERROR, 0, FALSE};
    steps[2] = (send_step_t){2, ERROR_TIMEOUT, 500, FALSE};
    step_count = 3;
    size_t offset = 0;
    assert(tick(payload, sizeof(payload) - 1, &offset) == ERROR_WOULD_BLOCK);
    assert(offset == 4 && tls.txRecordPos == 5 && encryptions == 2);
    assert(tick(payload, sizeof(payload) - 1, &offset) == NO_ERROR);
    assert(send_calls == 5 && tls_calls == 2);
    expect_wire(payload, sizeof(payload) - 1);

    /* Callback progress can reach the end of a record together with a timeout.
     * tlsWrite still owes that record's plaintext credit until the next call. */
    initialize();
    steps[0] = (send_step_t){SIZE_MAX, ERROR_TIMEOUT, 500, FALSE};
    step_count = 1;
    offset = 0;
    assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK && offset == 0);
    assert(tls.txRecordPos == tls.txRecordLen);
    assert(tick(payload, 4, &offset) == NO_ERROR && offset == 4);
    assert(send_calls == 1 && tls_calls == 2);
    expect_wire(payload, 4);
}

static void test_stall_deadline_and_sticky_error(void)
{
    const uint8_t payload[] = "abcd";
    initialize();
    for (size_t i = 0; i < 4; i++) steps[i] = (send_step_t){0, ERROR_TIMEOUT, 0, FALSE};
    step_count = 4;
    size_t offset = 0;
    for (size_t i = 0; i < 3; i++) assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
    monotonic_now += TB2_MQTT_BOX_WRITE_STALL_MS - 1;
    assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
    monotonic_now++;
    assert(tick(payload, 4, &offset) == ERROR_TIMEOUT);
    assert(send_calls == 4);
    assert(tb2_mqtt_passthrough_box_write_error(&session) == ERROR_TIMEOUT);
    tls.state = TLS_STATE_CLOSED;
    assert(tick(payload, 4, &offset) == ERROR_TIMEOUT);
    assert(send_calls == 4);

    initialize();
    offset = 0;
    steps[0] = (send_step_t){1, ERROR_TIMEOUT, 100000, FALSE};
    steps[1] = (send_step_t){1, ERROR_TIMEOUT, 100000, FALSE};
    step_count = 2;
    assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
    assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
    assert(offset == 0 && tls.txRecordPos == 2);
    assert(tick(payload, 4, &offset) == NO_ERROR);
    expect_wire(payload, 4);

    initialize();
    monotonic_now = UINT32_MAX - 10U;
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 20, FALSE};
    step_count = 1;
    offset = 0;
    assert(tick(payload, 4, &offset) == ERROR_WOULD_BLOCK);
    assert(tick(payload, 4, &offset) == NO_ERROR);
    expect_wire(payload, 4);
}

static void test_fatal_context_and_transport_error(void)
{
    const uint8_t payload[] = "abcd";
    initialize();
    steps[0] = (send_step_t){0, ERROR_WRITE_FAILED, 0, FALSE};
    step_count = 1;
    size_t offset = 0;
    assert(tick(payload, 4, &offset) == ERROR_WRITE_FAILED);
    assert(tls.state == TLS_STATE_CLOSED && send_calls == 1);
    assert(tick(payload, 4, &offset) == ERROR_WRITE_FAILED);
    assert(send_calls == 1);

    initialize();
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 0, TRUE};
    step_count = 1;
    offset = 0;
    assert(tick(payload, 4, &offset) == ERROR_TIMEOUT);
    assert(send_calls == 1 && tls.state == TLS_STATE_CLOSED);
}

static void test_defensive_contract_and_unavailable_clock(void)
{
    const uint8_t payload[] = "abcd";
    for (unsigned violation = 1; violation <= 2; violation++) {
        initialize();
        boundary_violation = violation;
        size_t offset = 0;
        assert(tick(payload, 4, &offset) == ERROR_WRITE_FAILED);
        assert(tls_calls == 1 && send_calls == 0);
        assert(tb2_mqtt_passthrough_box_write_error(&session) == ERROR_WRITE_FAILED);
        assert(tick(payload, 4, &offset) == ERROR_WRITE_FAILED);
        assert(tls_calls == 1);
    }
    initialize();
    clock_available = FALSE;
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 0, FALSE};
    step_count = 1;
    size_t offset = 0;
    assert(tick(payload, 4, &offset) == ERROR_FAILURE);
    assert(send_calls == 0 && tls_calls == 0);
}

static void test_native_nonblocking_callbacks(void)
{
    initialize();
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Socket sock = {0};
    sock.descriptor = pair[0];
    session.box_socket = &sock;
    int original_flags = fcntl(pair[0], F_GETFL);
    uint8_t bytes[1024] = {0};
    size_t written, received;
    session.box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    session.box_io_bytes = TB2_MQTT_BOX_IO_BYTES_PER_TICK;
    assert(tb2_mqtt_box_receive(&session, bytes, sizeof(bytes), &received, 0) == ERROR_WOULD_BLOCK);
    assert(!received && !session.box_write_stalled);
    session.box_io_ops = 0;
    assert(tb2_mqtt_box_send(&session, bytes, sizeof(bytes), &written, 0) == ERROR_WOULD_BLOCK);
    assert(!written && !session.box_write_stalled);
    for (size_t i = 0; i < 10000 && !session.box_write_stalled; i++) {
        session.box_io_ops = 1;
        session.box_io_bytes = sizeof(bytes);
        error_t error = tb2_mqtt_box_send(&session, bytes, sizeof(bytes), &written, 0);
        assert(error == NO_ERROR || error == ERROR_WOULD_BLOCK);
    }
    assert(session.box_write_stalled && fcntl(pair[0], F_GETFL) == original_flags);
    assert(recv(pair[1], bytes, sizeof(bytes), MSG_DONTWAIT) > 0);
    close(pair[0]); close(pair[1]);
}

static void test_control_output_deadline_and_closed_state(void)
{
    initialize();
    control_allowed = TRUE;
    const uint8_t alert[] = {1, 100};
    steps[0] = steps[1] = (send_step_t){0, ERROR_WOULD_BLOCK, 0, FALSE};
    step_count = 2;
    session.box_io_ops = 4;
    session.box_io_bytes = 16384;
    assert(tlsWriteProtocolData(&tls, alert, sizeof(alert), TLS_TYPE_ALERT) == ERROR_WOULD_BLOCK);
    assert(tb2_mqtt_box_control_step(&session) == ERROR_WOULD_BLOCK);
    assert(session.box_control_active && session.keepalive == 0);
    monotonic_now += TB2_MQTT_BOX_WRITE_STALL_MS;
    assert(tb2_mqtt_box_control_step(&session) == ERROR_TIMEOUT);
    assert(send_calls == 2 && tb2_mqtt_passthrough_box_write_error(&session) == ERROR_TIMEOUT);
    initialize();
    control_allowed = control_closes = TRUE;
    assert(tb2_mqtt_box_control_step(&session) == ERROR_TIMEOUT);
    assert(tb2_mqtt_passthrough_box_write_error(&session) == ERROR_TIMEOUT);
}

static void test_native_socket_failure_evidence(void)
{
    initialize();
    mqtt_debug_test_reset(true, 1000);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Socket sock = {0};
    sock.descriptor = pair[0];
    session.box_socket = &sock;
    uint8_t byte = 0;
    size_t count;
    session.box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    session.box_io_bytes = TB2_MQTT_BOX_IO_BYTES_PER_TICK;
    assert(tb2_mqtt_box_receive(&session, &byte, 1, &count, 0) == ERROR_WOULD_BLOCK);
    assert(mqtt_debug_test_count("socket_rx_failed") == 0);
    close(pair[1]);
    assert(tb2_mqtt_box_send(&session, &byte, 1, &count, 0) == ERROR_WRITE_FAILED);
    const cJSON *event = mqtt_debug_test_last("socket_tx_failed");
    assert(event && count == 0);
    assert(cJSON_GetObjectItemCaseSensitive(event, "native_error")->valuedouble == EPIPE);
    assert(cJSON_GetObjectItemCaseSensitive(event, "socket_result")->valuedouble == -1);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event,
        "native_error_domain")), "errno") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(event, "tls_state")->valuedouble == TLS_STATE_APPLICATION_DATA);
    /* EOF has no native error, regardless of the previous failing syscall. */
    errno = EACCES;
    assert(tb2_mqtt_box_receive(&session, &byte, 1, &count, 0) == ERROR_END_OF_STREAM);
    event = mqtt_debug_test_last("socket_rx_closed");
    assert(event && cJSON_GetObjectItemCaseSensitive(event, "native_error")->valuedouble == 0);
    assert(cJSON_GetObjectItemCaseSensitive(event, "socket_result")->valuedouble == 0);
    close(pair[0]);
    sock.descriptor = -1;
    session.box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    assert(tb2_mqtt_box_receive(&session, &byte, 1, &count, 0) == ERROR_READ_FAILED);
    event = mqtt_debug_test_last("socket_rx_failed");
    assert(event && cJSON_GetObjectItemCaseSensitive(event, "native_error")->valuedouble == EBADF);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event,
        "operation")), "recv") == 0);
    mqtt_debug_test_reset(false, 0);
    assert(tb2_mqtt_box_receive(&session, &byte, 1, &count, 0) == ERROR_READ_FAILED);
    assert(mqtt_debug_test_count("socket_rx_failed") == 0);
}

static void expect_status(const char *state, const char *error)
{
    HttpConnection connection = {0};
    assert(tb2_mqtt_passthrough_write_status(&connection) == NO_ERROR);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(last_status, "state")), state) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(last_status, "error_code")), error) == 0);
}

static void test_coupled_status_error_origin(void)
{
    initialize();
    settings.mqtt_client_upstream.enabled = true;
    memset(&mqtt_passthrough_status, 0, sizeof(mqtt_passthrough_status));
    assert(pthread_mutex_init(&mqtt_passthrough_status.mutex, NULL) == 0);
    const char *local_reasons[] = {"box_write_failed", "stream_failed", "disabled", "client_replaced"};
    for (size_t i = 0; i < arraysize(local_reasons); i++) {
        session.status_connected = false;
        tb2_mqtt_status_start();
        expect_status("connecting", "");
        tb2_mqtt_status_connected(&session);
        tb2_mqtt_status_connected(&session);
        assert(mqtt_passthrough_status.connected_sessions == 1);
        expect_status("connected", "");
        tb2_mqtt_status_finish(&session, local_reasons[i]);
        expect_status("ready", "");
    }
    tb2_mqtt_passthrough_session_t second = {0};
    atomic_init(&second.upstream_failed, false);
    atomic_init(&second.capture_failed, false);
    session.status_connected = false;
    tb2_mqtt_status_start();
    tb2_mqtt_status_connected(&session);
    tb2_mqtt_status_start();
    expect_status("connected", "");
    second.upstream_failed = true;
    tb2_mqtt_status_finish(&second, "stream_failed");
    expect_status("connected", "");
    assert(strcmp(mqtt_passthrough_status.error_code, "upstream_stream_failed") == 0);
    tb2_mqtt_status_finish(&session, "stream_failed");
    expect_status("ready", "");

    session.status_connected = false;
    tb2_mqtt_status_start();
    session.upstream_failed = true;
    tb2_mqtt_status_finish(&session, "stream_failed");
    expect_status("error", "upstream_stream_failed");
    session.upstream_failed = false;
    tb2_mqtt_status_start();
    session.capture_failed = true;
    tb2_mqtt_status_finish(&session, "stream_failed");
    expect_status("error", "capture_write_failed");
    settings.mqtt_client_upstream.enabled = false;
    expect_status("disabled", "");
    settings.mqtt_client_upstream.enabled = true;
    expect_status("error", "capture_write_failed");
    assert(pthread_mutex_destroy(&mqtt_passthrough_status.mutex) == 0);
    cJSON_Delete(last_status); last_status = NULL;
}

int main(void)
{
    test_retry_and_buffered_ciphertext();
    test_stall_deadline_and_sticky_error();
    test_fatal_context_and_transport_error();
    test_defensive_contract_and_unavailable_clock();
    test_native_nonblocking_callbacks();
    test_control_output_deadline_and_closed_state();
    test_native_socket_failure_evidence();
    test_coupled_status_error_origin();
    puts("TB2 box write runtime: cross-tick vendor records, native nonblocking callbacks, raw progress and sticky errors passed");
    return 0;
}
