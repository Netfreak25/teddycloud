/* Actual production/vendor function bodies are inserted by the Python runner. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
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
static bool_t mutex_created, require_io_mutex;
static size_t capture_count, forwarded_messages, forwarded_bytes;
static struct {
    bool_t forwarded;
    size_t sends;
    char action[48];
} captures[16];

void osAcquireMutex(OsMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void osReleaseMutex(OsMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }

static error_t tb2_mqtt_capture_packet_ex(tb2_mqtt_capture_t *capture,
    const char *direction, const uint8_t *data, size_t length,
    const uint8_t *wire_data, size_t wire_length, uint8_t packet_type,
    const char *topic, bool_t forwarded, const char *filter_id,
    bool_t generated, bool_t packet_complete, const char *action,
    uint16_t packet_id, uint16_t wire_packet_id, size_t removed_count)
{
    (void)data; (void)length; (void)wire_data; (void)wire_length;
    (void)packet_type; (void)topic; (void)filter_id; (void)generated;
    (void)packet_complete; (void)packet_id; (void)wire_packet_id; (void)removed_count;
    assert(capture == &session.capture);
    assert(strcmp(direction, "upstream_to_box") == 0);
    assert(pthread_mutex_trylock(&session.io_mutex) == EBUSY);
    assert(capture_count < sizeof(captures) / sizeof(captures[0]));
    captures[capture_count].forwarded = forwarded;
    captures[capture_count].sends = send_calls;
    snprintf(captures[capture_count].action, sizeof(captures[0].action), "%s", action ? action : "");
    capture_count++;
    return NO_ERROR;
}

static void tb2_mqtt_status_add_bytes(bool_t box_to_upstream, size_t length)
{
    assert(!box_to_upstream);
    forwarded_bytes += length;
}

static void tb2_mqtt_status_add_message(bool_t box_to_upstream, bool_t blocked)
{
    assert(!box_to_upstream && !blocked);
    forwarded_messages++;
}

static void tb2_mqtt_add_nocloud_stats(tb2_mqtt_passthrough_session_t *context,
    bool_t box_to_upstream, bool_t rewritten, size_t removed_count)
{
    assert(context == &session && !box_to_upstream);
    (void)rewritten;
    assert(removed_count == 0);
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
    (void)context;
    assert(!"The established-session write must not start a TLS handshake");
    return ERROR_NOT_CONNECTED;
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
    if (require_io_mutex) assert(pthread_mutex_trylock(&session.io_mutex) == EBUSY);
    send_step_t step = { .accepted = length, .error = NO_ERROR };
    if (send_calls < step_count) step = steps[send_calls];
    send_calls++;
    *written = MIN(length, step.accepted);
    assert(wire_length + *written <= sizeof(wire));
    memcpy(wire + wire_length, data, *written);
    wire_length += *written;
    monotonic_now += step.elapsed;
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
    if (mutex_created) assert(pthread_mutex_destroy(&session.io_mutex) == 0);
    memset(&session, 0, sizeof(session));
    assert(pthread_mutex_init(&session.io_mutex, NULL) == 0);
    mutex_created = TRUE;
    require_io_mutex = FALSE;
    memset(&settings, 0, sizeof(settings));
    memset(&tls, 0, sizeof(tls));
    memset(steps, 0, sizeof(steps));
    memset(tx_buffer, 0, sizeof(tx_buffer));
    step_count = send_calls = wire_length = encryptions = 0;
    capture_count = forwarded_messages = forwarded_bytes = 0;
    memset(captures, 0, sizeof(captures));
    tls_calls = boundary_violation = 0;
    monotonic_now = wall_now = 1000;
    clock_available = TRUE;
    settings.commonName = "test-box";
    settings.internal.overlayNumber = 7;
    session.box_tls = &tls;
    session.box_settings = &settings;
    session.capture_opened = TRUE;
    strcpy(session.capture.session_id, "test-session-17");
    atomic_init(&session.box_write_error, NO_ERROR);
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

static void test_retry_and_buffered_ciphertext(void)
{
    const uint8_t payload[] = "abcdefghijk";
    for (unsigned variant = 0; variant < 2; variant++) {
        initialize();
        steps[0] = (send_step_t){0, variant ? ERROR_WOULD_BLOCK : ERROR_TIMEOUT, 500, FALSE};
        step_count = 1;
        assert(tb2_mqtt_box_write_all(&session, payload, 4) == NO_ERROR);
        assert(send_calls == 2);
        expect_wire(payload, 4);
    }
    initialize();
    steps[0] = (send_step_t){SIZE_MAX, NO_ERROR, 0, FALSE};
    steps[1] = (send_step_t){3, NO_ERROR, 0, FALSE};
    steps[2] = (send_step_t){2, ERROR_TIMEOUT, 500, FALSE};
    step_count = 3;
    assert(tb2_mqtt_box_write_all(&session, payload, sizeof(payload) - 1) == NO_ERROR);
    assert(send_calls == 5 && tls_calls == 2);
    expect_wire(payload, sizeof(payload) - 1);

    /* Callback progress can reach the end of a record together with a timeout.
     * tlsWrite still owes that record's plaintext credit until the next call. */
    initialize();
    steps[0] = (send_step_t){SIZE_MAX, ERROR_TIMEOUT, 500, FALSE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == NO_ERROR);
    assert(send_calls == 1 && tls_calls == 2);
    expect_wire(payload, 4);
}

static void test_retry_limits_and_sticky_error(void)
{
    const uint8_t payload[] = "abcd";
    initialize();
    for (size_t i = 0; i < 4; i++) steps[i] = (send_step_t){0, ERROR_TIMEOUT, 100, FALSE};
    step_count = 4;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_TIMEOUT);
    assert(send_calls == 1 + TB2_MQTT_BOX_WRITE_MAX_RETRIES);
    assert(tb2_mqtt_passthrough_box_write_error(&session) == ERROR_TIMEOUT);
    tls.state = TLS_STATE_CLOSED;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_TIMEOUT);
    assert(send_calls == 3);

    initialize();
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, TB2_MQTT_BOX_WRITE_RETRY_BUDGET_MS, FALSE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_TIMEOUT);
    assert(send_calls == 1);

    initialize();
    monotonic_now = UINT32_MAX - 10U;
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 20, FALSE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == NO_ERROR);
    expect_wire(payload, 4);
}

static void test_fatal_context_and_transport_error(void)
{
    const uint8_t payload[] = "abcd";
    initialize();
    steps[0] = (send_step_t){0, ERROR_WRITE_FAILED, 0, FALSE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_WRITE_FAILED);
    assert(tls.state == TLS_STATE_CLOSED && send_calls == 1);
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_WRITE_FAILED);
    assert(send_calls == 1);

    initialize();
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 0, TRUE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_TIMEOUT);
    assert(send_calls == 1 && tls.state == TLS_STATE_CLOSED);
}

static void test_defensive_contract_and_unavailable_clock(void)
{
    const uint8_t payload[] = "abcd";
    for (unsigned violation = 1; violation <= 2; violation++) {
        initialize();
        boundary_violation = violation;
        assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_WRITE_FAILED);
        assert(tls_calls == 1 && send_calls == 0);
        assert(tb2_mqtt_passthrough_box_write_error(&session) == ERROR_WRITE_FAILED);
        assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_WRITE_FAILED);
        assert(tls_calls == 1);
    }
    initialize();
    clock_available = FALSE;
    steps[0] = (send_step_t){0, ERROR_TIMEOUT, 0, FALSE};
    step_count = 1;
    assert(tb2_mqtt_box_write_all(&session, payload, 4) == ERROR_TIMEOUT);
    assert(send_calls == 1 && tls_calls == 1);
}

static void test_capture_follows_actual_delivery(void)
{
    const uint8_t original[] = {1, 2, 3, 4};
    const uint8_t outgoing[] = {5, 6, 7, 8};
    for (unsigned failed = 0; failed <= 1; failed++) {
        initialize();
        require_io_mutex = TRUE;
        step_count = failed ? 3 : 1;
        for (size_t i = 0; i < step_count; i++)
            steps[i] = (send_step_t){0, ERROR_TIMEOUT, 100, FALSE};
        error_t error = tb2_mqtt_record_packet_ex(&session, FALSE,
            original, sizeof(original), outgoing, sizeof(outgoing),
            TB2_MQTT_PACKET_PUBLISH, "test", TRUE, NULL, FALSE, TRUE,
            "test_delivery", 0, 0, FALSE, TRUE, 0);
        assert(error == (failed ? ERROR_TIMEOUT : NO_ERROR));
        assert(capture_count == 2);
        assert(!captures[0].forwarded && captures[0].sends == 0);
        assert(strcmp(captures[0].action, "box_write_pending") == 0);
        assert(captures[1].forwarded == !failed && captures[1].sends == send_calls);
        assert(strcmp(captures[1].action, failed ? "box_write_failed" : "test_delivery") == 0);
        assert(forwarded_messages == !failed);
        assert(forwarded_bytes == (failed ? 0 : sizeof(outgoing)));
        assert(session.capture.messages_forwarded_upstream_to_box == !failed);
        assert(session.capture.bytes_upstream_to_box == (failed ? 0 : sizeof(outgoing)));
        assert(pthread_mutex_trylock(&session.io_mutex) == 0);
        assert(pthread_mutex_unlock(&session.io_mutex) == 0);
        if (!failed) expect_wire(outgoing, sizeof(outgoing));
    }
}

int main(void)
{
    test_retry_and_buffered_ciphertext();
    test_retry_limits_and_sticky_error();
    test_fatal_context_and_transport_error();
    test_defensive_contract_and_unavailable_clock();
    test_capture_follows_actual_delivery();
    assert(pthread_mutex_destroy(&session.io_mutex) == 0);
    puts("TB2 box write runtime: vendor buffering, bounded retries, sticky errors and capture completion passed");
    return 0;
}
