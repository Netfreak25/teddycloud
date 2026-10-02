/* Transport boundaries only; selected production functions are inserted below. */
#include <assert.h>
#include <stdlib.h>
#define clock_gettime test_clock_gettime
/* DECLARATIONS */
#undef clock_gettime
#undef TRACE_DEBUG
#undef TRACE_INFO
#undef TRACE_WARNING
#undef TRACE_ERROR
#define TRACE_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_INFO(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_ERROR(...) do { if (0) printf(__VA_ARGS__); } while (0)
/* PROTOTYPES */

static uint32_t now = 1000;
static unsigned callbacks, cancelled, sends, processed, captures, captured_sent;
static error_t write_error;
static tb2_mqtt_passthrough_session_t *completing;
static const uint8_t publish[] = {0x30, 3, 0, 1, 'a'};
static uint64_t expected_debug_message;
void mqtt_debug_test_reset(bool active, uint64_t now);
void mqtt_debug_test_time(uint64_t now);
unsigned mqtt_debug_test_count(const char *stage);
const cJSON *mqtt_debug_test_last(const char *stage);

int test_clock_gettime(clockid_t id, struct timespec *sample)
{
    assert(id == CLOCK_MONOTONIC);
    sample->tv_sec = now / 1000;
    sample->tv_nsec = (now % 1000) * 1000000;
    return 0;
}

void *osAllocMem(size_t length) { return malloc(length); }
void osFreeMem(void *data) { free(data); }
void osAcquireMutex(OsMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void osReleaseMutex(OsMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
bool_t tlsIsTxReady(TlsContext *tls) { (void)tls; return FALSE; }
TlsState tlsGetState(TlsContext *tls) { return tls->state; }
error_t tlsConnect(TlsContext *tls) { (void)tls; return NO_ERROR; }
error_t tlsWrite(TlsContext *tls, const void *data, size_t size, size_t *written, uint_t flags)
{
    (void)tls; (void)data; (void)flags;
    sends++;
    *written = write_error ? 0 : size;
    return write_error;
}

static error_t tb2_mqtt_capture_packet_ex(tb2_mqtt_capture_t *capture,
    const char *direction, const uint8_t *data, size_t length,
    const uint8_t *wire, size_t wire_length, uint8_t type, const char *topic,
    bool_t forwarded, const char *filter, bool_t generated, bool_t complete,
    const char *action, uint16_t id, uint16_t wire_id, size_t removed)
{
    captures++;
    if (forwarded) captured_sent++;
    return NO_ERROR;
}
static void tb2_mqtt_status_add_bytes(bool_t direction, size_t length) {}
static void tb2_mqtt_status_add_message(bool_t direction, bool_t blocked) {}
static void tb2_mqtt_add_nocloud_stats(tb2_mqtt_passthrough_session_t *s,
    bool_t direction, bool_t rewritten, size_t removed) {}
static tb2_mqtt_packet_id_entry_t *tb2_mqtt_packet_id_find_wire(
    tb2_mqtt_passthrough_session_t *s, uint16_t id) { return NULL; }
bool_t tb2_mqtt_passthrough_is_enabled(void) { return TRUE; }
static error_t tb2_mqtt_forward_ready(tb2_mqtt_passthrough_session_t *s, bool_t direction)
{
    /* Actual socket and TLS boundaries are covered by the vendor runtime test. */
    return NO_ERROR;
}

static void complete(void *context, error_t error)
{
    assert(context == completing);
    if (expected_debug_message && !error)
    {
        assert(tb2_mqtt_passthrough_debug_message(completing) == expected_debug_message);
        assert(tb2_mqtt_passthrough_debug_epoch(completing) == completing->debug_epoch);
    }
    assert(pthread_mutex_trylock(&completing->io_mutex) == 0);
    assert(pthread_mutex_unlock(&completing->io_mutex) == 0);
    if (error) cancelled++;
    else callbacks++;
}

static tb2_mqtt_local_publish_t item(tb2_mqtt_passthrough_session_t *s)
{
    const tb2_mqtt_local_publish_t result = {
        publish, sizeof(publish), "a", 0, "test", complete, s, 0
    };
    return result;
}

static error_t tb2_mqtt_process_packet(tb2_mqtt_passthrough_session_t *s,
    bool_t direction, const uint8_t *packet, size_t size, size_t header)
{
    processed++;
    tb2_mqtt_local_publish_t replies[] = {item(s), item(s)};
    assert(tb2_mqtt_passthrough_submit_local_batch(s, replies, 2).status == MQTT_DELIVERY_QUEUED);
    return NO_ERROR;
}

/* PRODUCTION */

static void init(tb2_mqtt_passthrough_session_t *s, TlsContext *tls, settings_t *settings)
{
    memset(s, 0, sizeof(*s));
    memset(tls, 0, sizeof(*tls));
    memset(settings, 0, sizeof(*settings));
    settings->commonName = "queue-test";
    s->box_tls = tls;
    s->box_settings = settings;
    s->capture_opened = TRUE;
    s->box_io_ops = 4;
    s->box_io_bytes = 16384;
    s->last_box_rx = now;
    tls->state = TLS_STATE_APPLICATION_DATA;
    pthread_mutex_init(&s->io_mutex, NULL);
    atomic_init(&s->box_write_error, NO_ERROR);
    atomic_init(&s->local_publish_error, NO_ERROR);
}

static void finish(tb2_mqtt_passthrough_session_t *s)
{
    completing = s;
    tb2_mqtt_box_writes_cancel(s, ERROR_NOT_CONNECTED);
    free(s->box_stream.data);
    free(s->upstream_stream.data);
    pthread_mutex_destroy(&s->io_mutex);
}

int main(void)
{
    tb2_mqtt_passthrough_session_t s, other;
    TlsContext tls, tls_other;
    settings_t settings, settings_other;
    init(&s, &tls, &settings);
    completing = &s;
    tb2_mqtt_local_publish_t pair[] = {item(&s), item(&s)};
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 2).status == MQTT_DELIVERY_QUEUED);
    assert(s.box_write_count == 2 && callbacks == 0 && sends == 0 && captured_sent == 0);
    write_error = ERROR_WOULD_BLOCK;
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && callbacks == 0);
    s.box_write_stalled = TRUE;
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 2).status == MQTT_DELIVERY_BUSY);
    assert(s.box_write_count == 2);
    init(&other, &tls_other, &settings_other);
    completing = &other;
    tb2_mqtt_local_publish_t parallel = item(&other);
    write_error = NO_ERROR;
    assert(tb2_mqtt_passthrough_submit_local_batch(&other, &parallel, 1).status == MQTT_DELIVERY_QUEUED);
    assert(tb2_mqtt_box_write_pump(&other) == NO_ERROR && callbacks == 1);
    completing = &s;
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && callbacks == 3 && captured_sent == 3);
    assert(!s.box_write_count && !s.box_write_bytes);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && callbacks == 3);
    for (unsigned i = 0; i < 32; i++)
        assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 1).status == MQTT_DELIVERY_QUEUED);
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 1).status == MQTT_DELIVERY_BUSY);
    tb2_mqtt_box_writes_cancel(&s, ERROR_NOT_CONNECTED);
    assert(cancelled == 32 && !s.box_write_count && !s.box_write_bytes);
    pair[0].context_bytes = TB2_MQTT_BUFFER_LIMIT;
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 1).status == MQTT_DELIVERY_FAILED);
    assert(!s.box_write_count && cancelled == 32);
    pair[0] = item(&s);
    const uint8_t input[] = {0xc0, 0, 0xc0, 0};
    assert(tb2_mqtt_process_stream(&s, TRUE, input, sizeof(input)) == NO_ERROR);
    assert(processed == 1 && s.box_write_count == 2 && s.box_stream.length == 2);
    assert(tb2_mqtt_process_stream(&s, TRUE, NULL, 0) == NO_ERROR && processed == 1);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR);
    assert(tb2_mqtt_process_stream(&s, TRUE, NULL, 0) == NO_ERROR && processed == 2);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && callbacks == 7);
    assert(!tb2_mqtt_passthrough_is_established(&s));
    const uint8_t connack[] = {0x20, 2, 0, 0};
    tb2_mqtt_box_write_t *write = NULL;
    assert(tb2_mqtt_box_write_create(connack, sizeof(connack), NULL, 0,
        2, NULL, NULL, FALSE, "connack", 0, 0, FALSE, 0, &write) == NO_ERROR);
    assert(tb2_mqtt_box_write_append(&s, write) == NO_ERROR);
    assert(!tb2_mqtt_passthrough_is_established(&s));
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && tb2_mqtt_passthrough_is_established(&s));
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, pair, 1).status == MQTT_DELIVERY_QUEUED);
    s.keepalive = 10;
    s.last_box_rx = now;
    write_error = ERROR_WOULD_BLOCK;
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    now += 15001;
    assert(tb2_mqtt_passthrough_task(&s) == ERROR_TIMEOUT);
    assert(tb2_mqtt_passthrough_box_write_error(&s) == NO_ERROR);
    finish(&s);
    finish(&other);
    assert(cancelled == 33);
    /* Diagnostics only: queued is not written, and callback context references
     * the exact completed message without mutating any delivery decision. */
    init(&s, &tls, &settings);
    completing = &s;
    s.owner = 17;
    s.debug_epoch = 3;
    mqtt_debug_test_reset(true, 1000);
    tb2_mqtt_local_publish_t diagnostic = item(&s);
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, &diagnostic, 1).status == MQTT_DELIVERY_QUEUED);
    assert(mqtt_debug_test_count("box_queued") == 1);
    assert(mqtt_debug_test_count("packet_created") == 1);
    assert(mqtt_debug_test_count("tx_complete") == 0);
    expected_debug_message = s.box_write_head->packet.debug_message;
    assert(expected_debug_message != 0);
    write_error = ERROR_WOULD_BLOCK;
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR);
    assert(mqtt_debug_test_count("tx_complete") == 0);
    write_error = NO_ERROR;
    mqtt_debug_test_time(1200);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR);
    assert(mqtt_debug_test_count("tx_complete") == 1);
    assert(tb2_mqtt_passthrough_debug_message(&s) == 0);
    expected_debug_message = 0;

    const uint8_t partial[] = {0x30, 3, 0};
    const uint8_t remaining[] = {1, 'a'};
    unsigned before = processed;
    assert(tb2_mqtt_process_stream(&s, TRUE, partial, sizeof(partial)) == NO_ERROR);
    assert(processed == before && mqtt_debug_test_count("parser_wait") == 1);
    const cJSON *wait = mqtt_debug_test_last("parser_wait");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(wait, "expected_packet_bytes")) == 5);
    uint64_t message = s.box_stream.debug_message;
    mqtt_debug_test_time(1300);
    assert(tb2_mqtt_process_stream(&s, TRUE, remaining, sizeof(remaining)) == NO_ERROR);
    assert(processed == before + 1 && mqtt_debug_test_count("packet_received") == 1);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("packet_received"), "message_id")) == message);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR);

    /* Reusable protocol IDs do not reuse diagnostic message IDs. Unknown ACKs
     * remain unmatched; a retry is linked only while its exact write is open. */
    uint8_t qos[] = {0x32, 5, 0, 1, 'a', 0, 17};
    tb2_mqtt_pending_write_t sent = {0};
    sent.wire = qos; sent.wire_length = sizeof(qos);
    sent.packet_type = 3; sent.debug_epoch = 3;
    sent.debug_message = 41; sent.debug_enqueued_ms = 1300;
    sent.debug_origin = "tonies";
    tb2_mqtt_debug_sent(&s, &sent, FALSE, 1300);
    const uint8_t ack[] = {0x40, 2, 0, 17};
    mqtt_debug_test_time(1400);
    tb2_mqtt_debug_ack(&s, TRUE, ack, sizeof(ack), 2);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("mqtt_ack_received"), "linked_message_id")) == 41);
    tb2_mqtt_debug_ack(&s, TRUE, ack, sizeof(ack), 2);
    assert(cJSON_GetObjectItem(mqtt_debug_test_last("mqtt_ack_received"), "linked_message_id") == NULL);
    sent.debug_message = 42;
    tb2_mqtt_debug_sent(&s, &sent, FALSE, 1400);
    qos[0] |= 8;
    sent.debug_message = 43;
    tb2_mqtt_debug_sent(&s, &sent, FALSE, 1500);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("tx_complete"), "retry_of")) == 42);
    qos[4] = 'b';
    sent.debug_message = 44;
    tb2_mqtt_debug_sent(&s, &sent, FALSE, 1500);
    assert(cJSON_GetObjectItem(mqtt_debug_test_last("tx_complete"), "retry_of") == NULL);
    finish(&s);
    mqtt_debug_test_reset(false, 0);
    puts("TB2 Box queue: deferred completion, FIFO limits, atomic admission, independent boxes, observer reservation, CONNACK and keepalive passed");
    puts("TB2 diagnostics: queue/write separation, callback identity, partial parser and ACK/retry correlation passed");
    return 0;
}
