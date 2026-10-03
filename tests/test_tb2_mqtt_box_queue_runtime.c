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
static tb2_mqtt_passthrough_session_t *read_session;
static TlsContext cloud_tls;
static Socket box_socket, cloud_socket;
static struct { uint8_t bytes[256]; size_t length; error_t result; } read_records[2][8];
static size_t read_count[2], read_next[2];
static unsigned read_calls[2], readiness_checks, processed_box, processed_cloud;
static bool read_header, read_would_block;
static unsigned replies_per_packet = 2;
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
bool_t tlsIsRxReady(TlsContext *tls) { (void)tls; return FALSE; }
TlsState tlsGetState(TlsContext *tls) { return tls->state; }
error_t tlsConnect(TlsContext *tls) { (void)tls; return NO_ERROR; }
error_t tlsWrite(TlsContext *tls, const void *data, size_t size, size_t *written, uint_t flags)
{
    (void)tls; (void)data; (void)flags;
    sends++;
    if (read_session && tls == read_session->box_tls) {
        if (!read_session->box_io_ops || !read_session->box_io_bytes) {
            *written = 0;
            return ERROR_WOULD_BLOCK;
        }
        read_session->box_io_ops--;
        if (!write_error) {
            assert(size <= read_session->box_io_bytes);
            read_session->box_io_bytes -= size;
        }
    }
    *written = write_error ? 0 : size;
    return write_error;
}

uint_t tcpWaitForEvents(Socket *socket, uint_t events, systime_t timeout)
{
    assert(events == SOCKET_EVENT_RX_READY && timeout == 0);
    readiness_checks++;
    if (!read_session) return 0;
    unsigned side = socket == &cloud_socket;
    return read_next[side] < read_count[side] || (!side && read_would_block) ? events : 0;
}

/* Controlled TLS boundary: Box records need header/body socket operations;
 * the coupled Cloud read intentionally does not consume the Box budget. */
error_t tlsRead(TlsContext *tls, void *data, size_t capacity, size_t *received, uint_t flags)
{
    assert(read_session && flags == 0);
    assert(!read_session->box_write_head && !read_session->box_write_active);
    unsigned side = tls == &cloud_tls;
    read_calls[side]++;
    *received = 0;
    if (!side) {
        if (read_would_block) {
            assert(read_session->box_io_ops);
            read_session->box_io_ops--;
            return ERROR_WOULD_BLOCK;
        }
        if (!read_header) {
            if (!read_session->box_io_ops) return ERROR_WOULD_BLOCK;
            read_session->box_io_ops--;
            read_header = true;
        }
        if (!read_session->box_io_ops) return ERROR_WOULD_BLOCK;
        read_session->box_io_ops--;
    }
    assert(read_next[side] < read_count[side]);
    size_t length = read_records[side][read_next[side]].length;
    assert(length <= capacity);
    if (!side) {
        assert(length <= read_session->box_io_bytes);
        read_session->box_io_bytes -= length;
        read_header = false;
    }
    memcpy(data, read_records[side][read_next[side]].bytes, length);
    *received = length;
    return read_records[side][read_next[side]++].result;
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
    if (direction) processed_box++;
    else processed_cloud++;
    tb2_mqtt_local_publish_t replies[] = {item(s), item(s)};
    if (replies_per_packet)
        assert(tb2_mqtt_passthrough_submit_local_batch(s, replies, replies_per_packet).status == MQTT_DELIVERY_QUEUED);
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
    s->packets_remaining = TB2_MQTT_PACKETS_PER_TICK;
    s->last_box_rx = now;
    tls->state = TLS_STATE_APPLICATION_DATA;
    pthread_mutex_init(&s->io_mutex, NULL);
    atomic_init(&s->box_write_error, NO_ERROR);
    atomic_init(&s->local_publish_error, NO_ERROR);
    atomic_init(&s->upstream_failed, FALSE);
    atomic_init(&s->capture_failed, FALSE);
}

static void finish(tb2_mqtt_passthrough_session_t *s)
{
    completing = s;
    tb2_mqtt_box_writes_cancel(s, ERROR_NOT_CONNECTED);
    free(s->box_stream.data);
    free(s->upstream_stream.data);
    pthread_mutex_destroy(&s->io_mutex);
    if (read_session == s) read_session = NULL;
}

static void prepare_drain(tb2_mqtt_passthrough_session_t *s, TlsContext *tls, settings_t *settings)
{
    init(s, tls, settings);
    completing = read_session = s;
    s->box_socket = &box_socket;
    memset(&cloud_tls, 0, sizeof(cloud_tls));
    cloud_tls.state = TLS_STATE_APPLICATION_DATA;
    s->upstream.tlsContext = &cloud_tls;
    s->upstream.socket = &cloud_socket;
    memset(read_count, 0, sizeof(read_count));
    memset(read_next, 0, sizeof(read_next));
    memset(read_calls, 0, sizeof(read_calls));
    read_header = read_would_block = false;
    readiness_checks = processed_box = processed_cloud = 0;
    write_error = NO_ERROR;
    replies_per_packet = 0;
}

static void queue_record(unsigned side, const uint8_t *bytes, size_t length, error_t result)
{
    assert(side < 2 && read_count[side] < arraysize(read_records[side]));
    assert(length <= sizeof(read_records[side][0].bytes));
    memcpy(read_records[side][read_count[side]].bytes, bytes, length);
    read_records[side][read_count[side]].length = length;
    read_records[side][read_count[side]++].result = result;
}

static void test_bounded_ready_reads_and_write_priority(void)
{
    tb2_mqtt_passthrough_session_t s;
    TlsContext tls;
    settings_t settings;
    prepare_drain(&s, &tls, &settings);
    replies_per_packet = 1;
    unsigned sent_before = sends;
    queue_record(0, publish, 3, NO_ERROR);
    queue_record(0, publish + 3, sizeof(publish) - 3, ERROR_WOULD_BLOCK);
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[0] == 2 && processed_box == 1 && s.box_io_ops == 0);
    assert(s.box_stream.length == 0 && s.box_write_count == 1 && sends == sent_before);
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(s.box_write_count == 0 && sends == sent_before + 1 && processed_box == 1);

    read_would_block = true;
    for (unsigned i = 1; i <= 3; i++) {
        assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
        assert(read_calls[0] == 2 + i); /* One attempt, no idle spin. */
    }
    read_would_block = false;
    unsigned checks_before = readiness_checks;
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[0] == 5 && readiness_checks == checks_before + 2);

    tb2_mqtt_local_publish_t pending = item(&s);
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, &pending, 1).status == MQTT_DELIVERY_QUEUED);
    queue_record(0, publish, sizeof(publish), NO_ERROR);
    write_error = ERROR_WOULD_BLOCK;
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[0] == 5 && s.box_write_active && processed_box == 1);
    write_error = NO_ERROR;
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[0] == 6 && processed_box == 2 && !s.box_write_count && s.box_io_ops == 0);
    finish(&s);
}

static void test_shared_packet_budget_and_direction_fairness(void)
{
    tb2_mqtt_passthrough_session_t s;
    TlsContext tls;
    settings_t settings;
    prepare_drain(&s, &tls, &settings);
    uint8_t batch[16 * sizeof(publish)];
    for (size_t i = 0; i < 16; i++) memcpy(batch + i * sizeof(publish), publish, sizeof(publish));
    s.packets_remaining = 0;
    assert(tb2_mqtt_process_stream(&s, FALSE, batch, sizeof(batch)) == NO_ERROR);
    queue_record(0, batch, sizeof(batch) / 2, NO_ERROR);
    queue_record(0, batch, sizeof(batch) / 2, NO_ERROR);
    for (unsigned visit = 0; visit < 6; visit++) {
        size_t cloud_before = s.upstream_stream.length;
        size_t box_before = s.box_stream.length;
        unsigned reads_before = read_calls[0];
        assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
        assert(processed_box + processed_cloud == (visit + 1) * TB2_MQTT_PACKETS_PER_TICK);
        assert(s.packets_remaining == 0);
        assert(tb2_mqtt_process_stream(&s, TRUE, NULL, 0) == NO_ERROR);
        assert(processed_box + processed_cloud == (visit + 1) * TB2_MQTT_PACKETS_PER_TICK);
        if (visit % 2 == 0) {
            assert(s.upstream_stream.length == cloud_before);
            assert(read_calls[0] > reads_before || s.box_stream.length < box_before);
        } else {
            assert(s.upstream_stream.length == cloud_before - 4 * sizeof(publish));
            assert(s.box_stream.length == box_before && read_calls[0] == reads_before);
        }
    }
    assert(processed_box == 12 && processed_cloud == 12);
    finish(&s);
}

static void test_coupled_cloud_read_bound_and_failure_origin(void)
{
    tb2_mqtt_passthrough_session_t s;
    TlsContext tls;
    settings_t settings;
    prepare_drain(&s, &tls, &settings);
    queue_record(1, publish, sizeof(publish), ERROR_TIMEOUT);
    queue_record(1, publish, sizeof(publish), NO_ERROR);
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[1] == 1 && processed_cloud == 1 && !s.upstream_failed);
    assert(tb2_mqtt_passthrough_task(&s) == NO_ERROR);
    assert(read_calls[1] == 2 && processed_cloud == 2 && !s.upstream_failed);
    finish(&s);

    for (unsigned side = 0; side < 2; side++) {
        for (unsigned failure = 0; failure < 2; failure++) {
            prepare_drain(&s, &tls, &settings);
            queue_record(side, publish, 0, failure ? ERROR_READ_FAILED : NO_ERROR);
            error_t expected = failure ? ERROR_READ_FAILED : ERROR_END_OF_STREAM;
            assert(tb2_mqtt_passthrough_task(&s) == expected);
            assert(s.upstream_failed == (side == 1));
            finish(&s);
        }
        prepare_drain(&s, &tls, &settings);
        const uint8_t invalid_length[] = {0x30,0x80,0x80,0x80,0x80};
        queue_record(side, invalid_length, sizeof(invalid_length), NO_ERROR);
        assert(tb2_mqtt_passthrough_task(&s) == ERROR_INVALID_LENGTH);
        assert(s.upstream_failed == (side == 1));
        finish(&s);
    }
    prepare_drain(&s, &tls, &settings);
    tb2_mqtt_local_publish_t pending = item(&s);
    assert(tb2_mqtt_passthrough_submit_local_batch(&s, &pending, 1).status == MQTT_DELIVERY_QUEUED);
    write_error = ERROR_WRITE_FAILED;
    assert(tb2_mqtt_passthrough_task(&s) == ERROR_WRITE_FAILED);
    assert(!s.upstream_failed && !read_calls[0] && !read_calls[1]);
    finish(&s);
}

int main(void)
{
    assert(pthread_mutex_init(&mqtt_passthrough_status.mutex, NULL) == 0);
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
    assert(!s.status_connected && mqtt_passthrough_status.connected_sessions == 0);
    write_error = ERROR_WOULD_BLOCK;
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR);
    assert(!s.status_connected && mqtt_passthrough_status.connected_sessions == 0);
    write_error = NO_ERROR;
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && tb2_mqtt_passthrough_is_established(&s));
    assert(s.status_connected && mqtt_passthrough_status.connected_sessions == 1);
    assert(tb2_mqtt_box_write_pump(&s) == NO_ERROR && mqtt_passthrough_status.connected_sessions == 1);
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
    test_bounded_ready_reads_and_write_priority();
    test_shared_packet_budget_and_direction_fairness();
    test_coupled_cloud_read_bound_and_failure_origin();
    puts("TB2 Box queue: deferred completion, FIFO limits, atomic admission, independent boxes, observer reservation, CONNACK and keepalive passed");
    puts("TB2 diagnostics: queue/write separation, callback identity, partial parser and ACK/retry correlation passed");
    puts("TB2 receive drain: shared budgets, partial returns, direction fairness, idle waits and coupled Cloud read limit passed");
    assert(pthread_mutex_destroy(&mqtt_passthrough_status.mutex) == 0);
    return 0;
}
