/* Production declarations and functions are inserted verbatim by the runner. */
#include <assert.h>
#include <stdlib.h>
#define clock_gettime test_clock_gettime
/* SESSION_DECLARATIONS */
#undef clock_gettime
#undef TRACE_DEBUG
#undef TRACE_INFO
#undef TRACE_WARNING
#undef TRACE_ERROR
#define TRACE_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_INFO(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define TRACE_ERROR(...) do { if (0) printf(__VA_ARGS__); } while (0)
/* SESSION_PROTOTYPES */

static settings_t test_settings;
/* Diagnostic observers read TLS state fields directly; keep real storage even
 * though transport functions remain deterministic boundary stubs. */
static TlsContext test_tls;
static Socket test_socket;
static tb2_mqtt_passthrough_session_t *read_session;
/* Two records each hold eight fully qualified filtered test publishes. */
static struct { uint8_t bytes[512]; size_t length; error_t result; } read_records[8];
static size_t read_record_count, read_record_next;
static unsigned read_calls, socket_checks;
static bool read_header, read_would_block;
static bool filter_allowed = true, privacy_blocked, capture_fails, consume_once;
static bool box_write_fails, capture_fails_after_write;
static bool box_write_pauses;
static error_t box_write_failure = ERROR_WRITE_FAILED;
static unsigned observed, local_acks, worker_resets, worker_sends;
static unsigned publish_completions;
static int subscription_grant = 2;
static systime_t now = 1000;
static int32_t wall_shift;
static struct { uint8_t bytes[2048]; size_t length; } box_writes[128], cloud_writes[128];
static size_t box_write_count, cloud_write_count;
static char capture_action[64];
static cJSON *last_global_status;

void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{
    (void)type;
    connection->response.contentLength = length;
}
error_t httpWriteResponse(HttpConnection *connection, void *data, size_t size, bool_t free_memory)
{
    (void)connection;
    assert(size == strlen(data));
    cJSON_Delete(last_global_status);
    last_global_status = cJSON_Parse(data);
    assert(last_global_status);
    if (free_memory) free(data);
    return NO_ERROR;
}

void mqtt_debug_test_reset(bool active, uint64_t time);
void mqtt_debug_test_time(uint64_t time);
unsigned mqtt_debug_test_count(const char *stage);
const cJSON *mqtt_debug_test_last(const char *stage);

settings_t *get_settings(void) { return &test_settings; }
bool settings_get_bool(const char *key)
{
    assert(strcmp(key, "mqtt_client_upstream.filters_enabled") == 0);
    return true;
}
setting_item_t *settings_get_by_name_ovl(const char *key, const char *overlay)
{
    (void)overlay;
    assert(strncmp(key, "mqtt_client_upstream.forward.", 29) == 0);
    static setting_item_t option;
    option.type = TYPE_BOOL;
    option.ptr = &filter_allowed;
    return &option;
}
systime_t osGetSystemTime(void) { return now + wall_shift; }
int test_clock_gettime(clockid_t id, struct timespec *sample)
{
    assert(id == CLOCK_MONOTONIC);
    sample->tv_sec = now / 1000U;
    sample->tv_nsec = (now % 1000U) * 1000000U;
    return 0;
}
void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *memory) { free(memory); }
bool_t osCreateMutex(OsMutex *mutex) { return pthread_mutex_init(mutex, NULL) == 0; }
void osDeleteMutex(OsMutex *mutex) { assert(pthread_mutex_destroy(mutex) == 0); }
void osAcquireMutex(OsMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void osReleaseMutex(OsMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
static pthread_mutex_t lifetime_mutex = PTHREAD_MUTEX_INITIALIZER;
void mutex_lock(mutex_id_t id)
{
    assert(id == MUTEX_MQTT_SESSION);
    assert(pthread_mutex_lock(&lifetime_mutex) == 0);
}
void mutex_unlock(mutex_id_t id)
{
    assert(id == MUTEX_MQTT_SESSION);
    assert(pthread_mutex_unlock(&lifetime_mutex) == 0);
}

/* Boundary only: the privacy implementation has its own focused suite. */
void mqtt_nocloud_filter_publish(settings_t *settings, bool_t box_to_upstream,
    const char *topic, const uint8_t *payload, size_t length,
    mqtt_nocloud_filter_result_t *result)
{
    (void)settings; (void)box_to_upstream; (void)topic; (void)payload; (void)length;
    memset(result, 0, sizeof(*result));
    if (privacy_blocked) {
        result->action = MQTT_NOCLOUD_BLOCK;
        result->filter_id = "test.protected";
    }
}
void mqtt_nocloud_filter_result_free(mqtt_nocloud_filter_result_t *result)
{
    free(result->payload);
}

bool_t tlsIsTxReady(TlsContext *context) { assert(context == &test_tls); return FALSE; }
bool_t tlsIsRxReady(TlsContext *context) { assert(context == &test_tls); return FALSE; }
TlsState tlsGetState(TlsContext *context) { assert(context == &test_tls); return context->state; }
error_t tlsConnect(TlsContext *context) { (void)context; assert(false); return ERROR_FAILURE; }

uint_t tcpWaitForEvents(Socket *socket, uint_t events, systime_t timeout)
{
    assert(socket == &test_socket && events == SOCKET_EVENT_RX_READY && timeout == 0);
    socket_checks++;
    return read_record_next < read_record_count || read_would_block ? events : 0;
}

/* Controlled TLS transport boundary: each record needs one header and one
 * body socket operation. The production drain, parser and ACK paths are real. */
error_t tlsRead(TlsContext *context, void *data, size_t capacity, size_t *received, uint_t flags)
{
    assert(context == &test_tls && read_session && flags == 0);
    assert(!read_session->box_write_active && read_session->box_write_head == NULL);
    read_calls++;
    *received = 0;
    if (read_would_block) {
        assert(read_session->box_io_ops);
        read_session->box_io_ops--;
        return ERROR_WOULD_BLOCK;
    }
    assert(read_record_next < read_record_count);
    if (!read_header) {
        if (!read_session->box_io_ops) return ERROR_WOULD_BLOCK;
        read_session->box_io_ops--;
        read_header = true;
    }
    if (!read_session->box_io_ops) return ERROR_WOULD_BLOCK;
    read_session->box_io_ops--;
    size_t length = read_records[read_record_next].length;
    assert(length <= capacity && length <= read_session->box_io_bytes);
    memcpy(data, read_records[read_record_next].bytes, length);
    read_session->box_io_bytes -= length;
    *received = length;
    read_header = false;
    return read_records[read_record_next++].result;
}

static error_t tb2_mqtt_tls_write_step(tb2_mqtt_passthrough_session_t *session, const uint8_t *data,
                                     size_t length, size_t *offset)
{
    assert(session->box_tls == &test_tls);
    error_t previous = atomic_load(&session->box_write_error);
    if (previous) return previous;
    if (!session->box_io_ops || !session->box_io_bytes) return ERROR_WOULD_BLOCK;
    if (box_write_pauses) { session->box_write_active = true; return ERROR_WOULD_BLOCK; }
    if (box_write_fails) {
        atomic_store(&session->box_write_error, box_write_failure);
        return box_write_failure;
    }
    assert(box_write_count < 128 && length <= sizeof(box_writes[0].bytes));
    memcpy(box_writes[box_write_count].bytes, data, length);
    box_writes[box_write_count++].length = length;
    *offset = length;
    session->box_write_active = false;
    session->box_io_ops--;
    assert(length <= session->box_io_bytes);
    session->box_io_bytes -= length;
    if (capture_fails_after_write) capture_fails = true;
    return NO_ERROR;
}

static error_t tb2_mqtt_capture_packet_ex(tb2_mqtt_capture_t *capture,
    const char *direction, const uint8_t *data, size_t length,
    const uint8_t *wire, size_t wire_length, uint8_t type, const char *topic,
    bool_t forwarded, const char *filter, bool_t generated, bool_t complete,
    const char *action, uint16_t id, uint16_t wire_id, size_t removed,
    const tb2_mqtt_publish_origin_t *origin)
{
    (void)capture; (void)direction; (void)data; (void)length; (void)wire;
    (void)wire_length; (void)type; (void)topic; (void)forwarded; (void)filter;
    (void)generated; (void)complete; (void)id; (void)wire_id; (void)removed; (void)origin;
    snprintf(capture_action, sizeof(capture_action), "%s", action ? action : "");
    return capture_fails ? ERROR_WRITE_FAILED : NO_ERROR;
}

error_t tb2_mqtt_upstream_send(tb2_mqtt_upstream_worker_t *worker,
    uint64_t epoch, uint64_t token, uint8_t *packet, size_t length)
{
    assert(worker && epoch && token);
    assert(cloud_write_count < 128 && length <= sizeof(cloud_writes[0].bytes));
    memcpy(cloud_writes[cloud_write_count].bytes, packet, length);
    cloud_writes[cloud_write_count++].length = length;
    free(packet);
    worker_sends++;
    return NO_ERROR;
}
void tb2_mqtt_upstream_reconnect(tb2_mqtt_upstream_worker_t *worker, error_t reason)
{
    (void)reason; assert(worker); worker_resets++;
}
void tb2_mqtt_upstream_disable(tb2_mqtt_upstream_worker_t *worker) { (void)worker; }
void tb2_mqtt_upstream_confirm_session(tb2_mqtt_upstream_worker_t *worker, uint64_t epoch)
{
    assert(worker && epoch);
}
void tb2_mqtt_upstream_shutdown(tb2_mqtt_upstream_worker_t *worker, bool_t reconnect, error_t reason)
{
    (void)reconnect; (void)reason; assert(worker);
}
bool_t tb2_mqtt_upstream_poll(tb2_mqtt_upstream_worker_t *worker, tb2_mqtt_upstream_event_t *event)
{
    (void)event; assert(worker);
    return false;
}
void tb2_mqtt_upstream_event_free(tb2_mqtt_upstream_event_t *event)
{
    free(event->data);
}
static error_t tb2_mqtt_configure_worker(tb2_mqtt_passthrough_session_t *session)
{
    (void)session;
    assert(false); /* Tests inject an already acquired transport boundary. */
    return ERROR_FAILURE;
}

static error_t observe(void *context, bool_t incoming, const char *topic,
    const uint8_t *payload, size_t length, uint8_t qos, tb2_mqtt_observer_result_t *result)
{
    (void)context; (void)incoming; (void)topic; (void)payload; (void)length; (void)qos;
    observed++;
    result->locally_processed = true;
    if (consume_once) {
        consume_once = false;
        result->action = TB2_MQTT_OBSERVER_CONSUME;
        result->capture_action = "local_response_consume";
        result->filter_id = "local_control.app_reply";
    }
    return NO_ERROR;
}
static void observe_control(void *context, tb2_mqtt_control_event_t event,
    uint16_t id, const uint8_t *payload, size_t length)
{
    (void)context; (void)id; (void)payload; (void)length;
    if (event == TB2_MQTT_CONTROL_LOCAL_PUBACK) local_acks++;
}
static void publish_completed(void *context, bool_t incoming, const char *topic,
    const uint8_t *payload, size_t length)
{
    (void)context;
    assert(!incoming && strcmp(topic, "x") == 0);
    assert(length == 2 && memcmp(payload, "{}", 2) == 0);
    publish_completions++;
}
static error_t apply_subscriptions(void *context, bool_t unsubscribe,
    const uint8_t *payload, size_t length, uint8_t *codes, size_t capacity, size_t *count)
{
    (void)context; (void)payload; (void)length;
    /* Server callback validation/table limits are tested in its own real-code suite. */
    *count = unsubscribe ? 1 : 2;
    if (!unsubscribe) {
        assert(capacity >= 2);
        codes[0] = 1;
        codes[1] = 0x80;
    }
    return NO_ERROR;
}
static size_t snapshot_subscriptions(void *context, tb2_mqtt_subscription_t *items, size_t capacity)
{
    (void)context; (void)items; (void)capacity;
    return 0;
}
static int subscription_qos(void *context, const char *topic)
{
    (void)context; assert(topic);
    return subscription_grant;
}

/* SESSION_FUNCTIONS */

static void initialize(tb2_mqtt_passthrough_session_t *session)
{
    static uint64_t owner_serial;
    memset(session, 0, sizeof(*session));
    atomic_init(&session->box_write_error, NO_ERROR);
    memset(&test_tls, 0, sizeof(test_tls));
    test_tls.state = TLS_STATE_APPLICATION_DATA;
    session->box_tls = &test_tls;
    session->box_socket = &test_socket;
    session->packets_remaining = TB2_MQTT_PACKETS_PER_TICK;
    session->box_settings = &test_settings;
    session->observer = observe;
    session->control_observer = observe_control;
    session->subscription_apply = apply_subscriptions;
    session->subscription_snapshot = snapshot_subscriptions;
    session->subscription_qos = subscription_qos;
    session->capture_opened = true;
    session->owner = ++owner_serial;
    assert(osCreateMutex(&session->io_mutex));
    assert(osCreateMutex(&session->ids_mutex));
    observed = local_acks = worker_resets = worker_sends = 0;
    box_write_count = cloud_write_count = 0;
    filter_allowed = true;
    privacy_blocked = capture_fails = consume_once = false;
    box_write_fails = capture_fails_after_write = false;
    box_write_pauses = false;
    box_write_failure = ERROR_WRITE_FAILED;
    publish_completions = 0;
    subscription_grant = 2;
    session->saved_slot = -1;
    wall_shift = 0;
    read_session = session;
    read_record_count = read_record_next = read_calls = socket_checks = 0;
    read_header = read_would_block = false;
}

static error_t drain_box(tb2_mqtt_passthrough_session_t *session)
{
    session->box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    session->box_io_bytes = TB2_MQTT_BOX_IO_BYTES_PER_TICK;
    session->packets_remaining = TB2_MQTT_PACKETS_PER_TICK;
    return tb2_mqtt_box_write_pump(session);
}

static size_t packet(uint8_t *output, uint8_t flags, const uint8_t *body, size_t length)
{
    output[0] = flags;
    size_t remaining = length, offset = 1;
    do {
        uint8_t digit = remaining % 128;
        remaining /= 128;
        output[offset++] = digit | (remaining ? 0x80 : 0);
    } while (remaining);
    memcpy(output + offset, body, length);
    return offset + length;
}

static void receive(tb2_mqtt_passthrough_session_t *session, bool_t incoming,
    const uint8_t *data, size_t length)
{
    session->packets_remaining = TB2_MQTT_PACKETS_PER_TICK;
    assert(tb2_mqtt_process_stream(session, incoming, data, length) == NO_ERROR);
    for (size_t i = 0; i < 128; i++) {
        tb2_mqtt_stream_t *stream = incoming ? &session->box_stream : &session->upstream_stream;
        size_t previous = stream->length;
        assert(drain_box(session) == NO_ERROR);
        assert(tb2_mqtt_resume_one(session) == NO_ERROR);
        assert(tb2_mqtt_process_stream(session, incoming, NULL, 0) == NO_ERROR);
        if (!session->box_write_head && !session->resume_next && stream->length == previous) break;
    }
}

static void expect_last(uint8_t type, uint16_t id)
{
    assert(box_write_count);
    const uint8_t *bytes = box_writes[box_write_count - 1].bytes;
    assert((bytes[0] >> 4) == type);
    if (id) assert(((unsigned)bytes[2] << 8 | bytes[3]) == id);
}

static void connect_local(tb2_mqtt_passthrough_session_t *session, bool_t clean)
{
    uint8_t body[] = {0,4,'M','Q','T','T',4,clean ? 2 : 0,0,30,0,4,'t','e','s','t'};
    uint8_t data[64];
    size_t length = packet(data, 0x10, body, sizeof(body));
    size_t previous = box_write_count;
    receive(session, true, data, 3); /* Fragmented CONNECT. */
    assert(box_write_count == previous);
    receive(session, true, data + 3, length - 3);
    assert(session->established && !session->worker);
    expect_last(2, 0);
    assert(box_writes[previous].length == 4 && box_writes[previous].bytes[3] == 0);
}

static void send_publish_direction(tb2_mqtt_passthrough_session_t *session, uint8_t qos,
    uint16_t id, bool_t duplicate, bool_t incoming)
{
    static const char topic[] = "toniebox/AABBCCDDEEFF/playback/state";
    uint8_t body[256], data[260];
    size_t n = strlen(topic), offset = 2;
    body[0] = n >> 8; body[1] = n;
    memcpy(body + offset, topic, n); offset += n;
    if (qos) { body[offset++] = id >> 8; body[offset++] = id; }
    memcpy(body + offset, "{}", 2); offset += 2;
    size_t length = packet(data, 0x30 | (qos << 1) | (duplicate ? 8 : 0), body, offset);
    receive(session, incoming, data, length);
}

static void send_publish(tb2_mqtt_passthrough_session_t *session, uint8_t qos,
    uint16_t id, bool_t duplicate)
{
    send_publish_direction(session, qos, id, duplicate, true);
}

static void test_offline_transport(tb2_mqtt_passthrough_session_t *session)
{
    connect_local(session, true);
    uint8_t subscriptions[] = {0x82,12,0,7,0,3,'a','/','+',1,0,1,'b',2};
    receive(session, true, subscriptions, sizeof(subscriptions));
    expect_last(9, 7);
    assert(box_writes[box_write_count - 1].length == 6);
    assert(box_writes[box_write_count - 1].bytes[4] == 1);
    assert(box_writes[box_write_count - 1].bytes[5] == 0x80);
    uint8_t unsubscribe[] = {0xa2,5,0,8,0,1,'b'};
    receive(session, true, unsubscribe, sizeof(unsubscribe));
    expect_last(11, 8);
    const uint8_t pings[] = {0xc0,0,0xc0,0};
    size_t previous = box_write_count;
    receive(session, true, pings, sizeof(pings));
    assert(box_write_count == previous + 2 && !worker_sends);
    expect_last(13, 0);
}

static void test_offline_qos(tb2_mqtt_passthrough_session_t *session)
{
    send_publish(session, 1, 40, false);
    expect_last(4, 40);
    assert(observed == 1);
    send_publish(session, 1, 40, true);
    expect_last(4, 40);
    assert(observed >= 1); /* Generic QoS1 permits repeated application delivery. */
    unsigned before_reuse = observed;
    send_publish(session, 1, 40, false); /* Non-DUP packet ID reuse is a new message. */
    assert(observed == before_reuse + 1);
    send_publish(session, 2, 41, false);
    expect_last(5, 41);
    unsigned before_duplicate = observed;
    send_publish(session, 2, 41, true);
    expect_last(5, 41);
    assert(observed == before_duplicate);
    const uint8_t release[] = {0x62,2,0,41};
    receive(session, true, release, sizeof(release));
    expect_last(7, 41);
    unsigned after_release = observed;
    receive(session, true, release, sizeof(release));
    expect_last(7, 41);
    assert(observed == after_release && !worker_sends);
}

static void test_cloud_epoch_and_local_freshness(tb2_mqtt_passthrough_session_t *session)
{
    uint16_t freshness_id = 0;
    assert(tb2_mqtt_passthrough_reserve_local_packet_id(session, &freshness_id) == NO_ERROR);
    assert(freshness_id);
    session->worker = (tb2_mqtt_upstream_worker_t *)1;
    session->epoch = 1;
    session->cloud_phase = TB2_MQTT_CLOUD_READY;
    send_publish(session, 1, 70, false);
    assert(worker_sends == 1 && session->cloud_ids);
    assert(session->writes_bytes > 0);
    tb2_mqtt_upstream_event_t failed = {.type = TB2_MQTT_UPSTREAM_TX_FAILED,
        .owner_serial = session->owner, .epoch = 1, .token = session->writes[0].token};
    tb2_mqtt_complete_write(session, &failed); /* Worker contract: TX failure precedes DOWN. */
    tb2_mqtt_cloud_reset(session, "test_disconnect");
    assert(session->established && !session->cloud_ids && !session->writes_bytes);
    uint8_t ack[] = {0x40,2,freshness_id >> 8,freshness_id};
    receive(session, true, ack, sizeof(ack));
    assert(local_acks == 1); /* Cloud disconnect did not delete local freshness IDs. */
    size_t before = cloud_write_count;
    send_publish(session, 1, 71, false);
    expect_last(4, 71);
    assert(cloud_write_count == before && !session->writes_bytes);
    session->epoch = 2;
    session->cloud_phase = TB2_MQTT_CLOUD_READY;
    assert(cloud_write_count == before); /* No offline telemetry replay. */
    send_publish(session, 1, 72, false);
    assert(cloud_write_count == before + 1);
}

static void test_policy_and_capture_failure(tb2_mqtt_passthrough_session_t *session)
{
    unsigned before = observed, sent = worker_sends;
    filter_allowed = false;
    send_publish(session, 1, 80, false);
    expect_last(4, 80);
    assert(observed == before + 1 && worker_sends == sent);
    filter_allowed = true; privacy_blocked = true;
    send_publish(session, 1, 81, false);
    expect_last(4, 81);
    assert(observed == before + 2 && worker_sends == sent);
    privacy_blocked = false; capture_fails = true;
    send_publish(session, 1, 82, false);
    expect_last(4, 82);
    assert(session->capture_failed && session->established && worker_sends == sent);
    const uint8_t ping[] = {0xc0,0};
    receive(session, true, ping, sizeof(ping));
    expect_last(13, 0); /* Capture failure cannot kill the local connection. */
}

static void test_local_response_duplicate(tb2_mqtt_passthrough_session_t *session)
{
    unsigned sent = worker_sends, before = observed;
    consume_once = true;
    send_publish(session, 1, 79, false);
    expect_last(4, 79);
    assert(observed == before + 1 && worker_sends == sent);
    send_publish(session, 1, 79, true); /* Lost PUBACK must not leak a consumed reply. */
    expect_last(4, 79);
    assert(observed == before + 1 && worker_sends == sent);
    send_publish(session, 1, 79, false); /* New non-DUP use must not be swallowed. */
    assert(observed == before + 2 && worker_sends == sent + 1);
}

static void test_clean_session_resume(void)
{
    static tb2_mqtt_passthrough_session_t persistent, replacement;
    initialize(&persistent);
    connect_local(&persistent, false);
    /* The prior active session was clean; it must not be claimed as resumed. */
    assert(box_writes[0].bytes[2] == 0);
    send_publish(&persistent, 2, 91, false);
    expect_last(5, 91);
    consume_once = true;
    send_publish(&persistent, 1, 92, false);
    expect_last(4, 92);
    initialize(&replacement);
    connect_local(&replacement, false);
    assert(box_writes[0].bytes[2] == 1);
    send_publish(&replacement, 2, 91, true);
    expect_last(5, 91);
    assert(observed == 0); /* The pending incoming QoS2 receipt survived. */
    replacement.worker = (tb2_mqtt_upstream_worker_t *)1;
    replacement.epoch = 1;
    replacement.cloud_phase = TB2_MQTT_CLOUD_READY;
    send_publish(&replacement, 1, 92, true);
    expect_last(4, 92);
    assert(observed == 0 && worker_sends == 0); /* Consumed reply history survives too. */
}

typedef struct {
    bool subscribed;
    unsigned restored;
} takeover_subscriptions_t;

static size_t takeover_snapshot(void *context, tb2_mqtt_subscription_t *items, size_t capacity)
{
    takeover_subscriptions_t *state = context;
    if (!state->subscribed) return 0;
    assert(capacity > 0);
    strcpy(items[0].topic, "toniebox/FFEEDDCCBBAA/#");
    items[0].qos = 1;
    return 1;
}

static error_t takeover_restore(void *context, bool_t unsubscribe,
    const uint8_t *payload, size_t length, uint8_t *codes, size_t capacity, size_t *count)
{
    takeover_subscriptions_t *state = context;
    const char topic[] = "toniebox/FFEEDDCCBBAA/#";
    assert(!unsubscribe && capacity > 0 && length == 5 + strlen(topic));
    assert(payload[2] == 0 && payload[3] == strlen(topic));
    assert(memcmp(payload + 4, topic, strlen(topic)) == 0 && payload[length - 1] == 1);
    state->subscribed = true;
    state->restored++;
    codes[0] = 1;
    *count = 1;
    return NO_ERROR;
}

static void dispose_takeover_fixture(tb2_mqtt_passthrough_session_t *session)
{
    mutex_lock(MUTEX_MQTT_SESSION);
    tb2_mqtt_box_writes_cancel(session, ERROR_NOT_CONNECTED);
    tb2_mqtt_cancel_connect(session);
    mutex_unlock(MUTEX_MQTT_SESSION);
    tb2_mqtt_qos2_free(&session->blocked_qos2_box);
    tb2_mqtt_packet_ids_clear(&session->packet_ids);
    mqtt_response_history_reset(&session->response_history);
    free(session->connect_packet);
    free(session->box_stream.data);
    osDeleteMutex(&session->ids_mutex);
    osDeleteMutex(&session->io_mutex);
}

static void test_takeover_waits_for_written_connack(void)
{
    tb2_mqtt_passthrough_session_t old, failed, replacement;
    takeover_subscriptions_t old_subs = {.subscribed = true}, new_subs = {0};
    char *previous_box = test_settings.commonName;
    test_settings.commonName = "FFEEDDCCBBAA";
    initialize(&old);
    old.observer_context = &old_subs;
    old.subscription_snapshot = takeover_snapshot;
    old.subscription_apply = takeover_restore;
    connect_local(&old, false);
    send_publish(&old, 2, 95, false);
    tb2_mqtt_qos2_entry_t *incoming = old.blocked_qos2_box;
    assert(incoming != NULL);
    const uint8_t publish[] = {0x32,7,0,1,'x',0,210,'{','}'};
    assert(tb2_mqtt_packet_id_add(&old, 110, 210, 1, FALSE) == NO_ERROR);
    tb2_mqtt_packet_id_entry_t *outgoing = tb2_mqtt_id_find(old.packet_ids, 210);
    assert(outgoing != NULL);
    assert(tb2_mqtt_id_store_packet(old.packet_ids, outgoing, publish, sizeof(publish)) == NO_ERROR);
    outgoing->transmitted = TRUE;
    uint16_t local_id = 0;
    assert(tb2_mqtt_passthrough_reserve_local_packet_id(&old, &local_id) == NO_ERROR);
    tb2_mqtt_saved_session_t *saved = &saved_sessions[old.saved_slot];
    assert(saved->active == &old && saved->subscription_count == 1);
    const uint8_t body[] = {0,4,'M','Q','T','T',4,0,0,30,0,4,'t','e','s','t'};
    uint8_t connect[64];
    size_t length = packet(connect, 0x10, body, sizeof(body));

    initialize(&failed);
    failed.observer_context = &new_subs;
    failed.subscription_snapshot = takeover_snapshot;
    failed.subscription_apply = takeover_restore;
    assert(tb2_mqtt_process_stream(&failed, TRUE, connect, length) == NO_ERROR);
    assert(!failed.connack_sent && failed.box_write_count == 1 && box_write_count == 0);
    assert(tb2_mqtt_passthrough_box_write_error(&old) == NO_ERROR);
    assert(saved->active == &old && saved->owner == old.owner);
    assert(old.blocked_qos2_box == incoming && tb2_mqtt_id_find(old.packet_ids, 210) == outgoing);
    assert(tb2_mqtt_id_find(old.packet_ids, local_id) != NULL);
    const uint8_t ping[] = {0xc0,0};
    receive(&old, TRUE, ping, sizeof(ping));
    expect_last(13, 0);
    box_write_pauses = true;
    assert(drain_box(&failed) == NO_ERROR);
    assert(failed.box_write_active && !failed.connack_sent);
    assert(tb2_mqtt_passthrough_box_write_error(&old) == NO_ERROR);
    assert(saved->active == &old && saved->subscription_count == 1);
    box_write_pauses = false;
    box_write_fails = true;
    assert(drain_box(&failed) == ERROR_WRITE_FAILED);
    assert(!failed.connack_sent && tb2_mqtt_passthrough_box_write_error(&old) == NO_ERROR);
    assert(old.blocked_qos2_box == incoming && tb2_mqtt_id_find(old.packet_ids, 210) == outgoing);
    assert(saved->active == &old && saved->owner == old.owner);
    dispose_takeover_fixture(&failed);
    box_write_fails = false;
    receive(&old, TRUE, ping, sizeof(ping));
    expect_last(13, 0);

    initialize(&replacement);
    new_subs = (takeover_subscriptions_t){0};
    replacement.observer_context = &new_subs;
    replacement.subscription_snapshot = takeover_snapshot;
    replacement.subscription_apply = takeover_restore;
    assert(tb2_mqtt_process_stream(&replacement, TRUE, connect, length) == NO_ERROR);
    assert(saved->active == &old && !replacement.connack_sent);
    assert(tb2_mqtt_passthrough_box_write_error(&old) == NO_ERROR);
    assert(drain_box(&replacement) == NO_ERROR);
    assert(replacement.connack_sent && box_write_count == 1);
    assert(box_writes[0].length == 4 && box_writes[0].bytes[2] == 1);
    assert(tb2_mqtt_passthrough_box_write_error(&old) == ERROR_NOT_CONNECTED);
    assert(saved->active == &replacement && saved->owner == replacement.owner);
    assert(replacement.blocked_qos2_box == incoming && old.blocked_qos2_box == NULL);
    assert(tb2_mqtt_id_find(replacement.packet_ids, 210) == outgoing && old.packet_ids == NULL);
    assert(tb2_mqtt_id_find(replacement.packet_ids, local_id) == NULL);
    assert(new_subs.subscribed && new_subs.restored == 1);
    assert(drain_box(&replacement) == NO_ERROR);
    assert(box_write_count == 1 && new_subs.restored == 1);
    send_publish(&replacement, 2, 95, true);
    expect_last(TB2_MQTT_PACKET_PUBREC, 95);
    assert(observed == 0); /* Resumed receipt is not processed twice. */
    tb2_mqtt_saved_clear(saved);
    dispose_takeover_fixture(&replacement);
    dispose_takeover_fixture(&old);
    test_settings.commonName = previous_box;
}

static void test_reset_during_cloud_packet(void)
{
    static tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    connect_local(&session, true);
    session.worker = (tb2_mqtt_upstream_worker_t *)1;
    session.epoch = 1;
    session.cloud_phase = TB2_MQTT_CLOUD_READY;
    for (size_t i = 0; i < TB2_MQTT_INFLIGHT_MAX; i++) {
        uint16_t id = 0;
        assert(tb2_mqtt_passthrough_reserve_local_packet_id(&session, &id) == NO_ERROR);
    }
    /* Destination inflight exhaustion resets cloud inside process_packet. The
     * enclosing stream loop must not subtract from the now-cleared length. */
    send_publish_direction(&session, 1, 93, false, false);
    assert(session.cloud_phase == TB2_MQTT_CLOUD_DOWN);
    assert(session.upstream_stream.length == 0 && session.established);
    const uint8_t ping[] = {0xc0,0};
    receive(&session, true, ping, sizeof(ping));
    expect_last(13, 0);
}

static void expect_runtime(tb2_mqtt_passthrough_session_t *session,
    bool local, bool cloud, const char *state, const char *error)
{
    (void)state;
    assert(tb2_mqtt_passthrough_is_established(session) == local);
    assert(tb2_mqtt_passthrough_is_upstream_connected(session) == cloud);
    if (session) assert(strcmp(session->upstream_error, error) == 0);
}

static void test_per_box_runtime(void)
{
    tb2_mqtt_passthrough_session_t first, second;
    initialize(&first);
    initialize(&second);
    first.established = second.established = true;
    first.connack_sent = second.connack_sent = true;
    first.worker = second.worker = (tb2_mqtt_upstream_worker_t *)1;
    first.epoch = 1;
    first.cloud_phase = TB2_MQTT_CLOUD_READY;
    strcpy(second.upstream_error, "upstream_unavailable");
    tb2_mqtt_runtime_update(&first);
    tb2_mqtt_runtime_update(&second);
    expect_runtime(&first, true, true, "connected", "");
    expect_runtime(&second, true, false, "error", "upstream_unavailable");
    second.epoch = 2;
    second.cloud_phase = TB2_MQTT_CLOUD_READY;
    second.upstream_error[0] = 0;
    tb2_mqtt_runtime_update(&second);
    tb2_mqtt_cloud_reset(&first, "upstream_protocol_error");
    tb2_mqtt_runtime_update(&first);
    expect_runtime(&first, true, false, "error", "upstream_protocol_error");
    expect_runtime(&second, true, true, "connected", "");
    first.established = second.established = false;
    second.cloud_phase = TB2_MQTT_CLOUD_DOWN;
    tb2_mqtt_runtime_update(&first);
    tb2_mqtt_runtime_update(&second);
    osDeleteMutex(&first.ids_mutex); osDeleteMutex(&first.io_mutex);
    osDeleteMutex(&second.ids_mutex); osDeleteMutex(&second.io_mutex);
}

static void test_independent_keepalives(void)
{
    static tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    connect_local(&session, true);
    session.worker = (tb2_mqtt_upstream_worker_t *)1;
    session.epoch = 1;
    session.cloud_phase = TB2_MQTT_CLOUD_READY;
    session.upstream_configured = true;
    session.subscriptions_dirty = false;
    session.last_cloud_tx = now;
    assert(tb2_mqtt_build_cloud_connect(&session, &session.cloud_connect,
        &session.cloud_connect_length) == NO_ERROR);
    now += session.keepalive * 500U;
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(session.ping_pending && cloud_write_count == 1);
    assert(cloud_writes[0].length == 2 && cloud_writes[0].bytes[0] == 0xc0);
    now += session.keepalive * 1000U;
    const uint8_t local_ping[] = {0xc0,0};
    receive(&session, true, local_ping, sizeof(local_ping));
    expect_last(13, 0);
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    expect_runtime(&session, true, false, "error", "upstream_keepalive_timeout");
    now += session.keepalive * 1500U + 1;
    assert(tb2_mqtt_passthrough_task(&session) == ERROR_TIMEOUT);
    now = 1000;
}

static void test_downlink_completion(void)
{
    const uint8_t publish[] = {0x30,5,0,1,'x','{','}'};
    for (unsigned variant = 0; variant < 5; variant++) {
        tb2_mqtt_passthrough_session_t session;
        initialize(&session);
        session.established = true;
        session.publish_completed = publish_completed;
        box_write_fails = variant == 1 || variant == 4;
        box_write_failure = variant == 4 ? ERROR_TIMEOUT : ERROR_WRITE_FAILED;
        capture_fails_after_write = variant == 2;
        capture_fails = variant == 3;
        error_t result = tb2_mqtt_process_stream(&session, false, publish, sizeof(publish));
        assert(result == NO_ERROR);
        result = drain_box(&session);
        assert(result == (box_write_fails ? box_write_failure : NO_ERROR));
        unsigned delivered = variant == 0 || variant == 2 ? 1 : 0;
        assert(box_write_count == delivered && publish_completions == delivered);
        if (box_write_fails) {
            box_write_fails = false;
            assert(tb2_mqtt_passthrough_box_write_error(&session) == box_write_failure);
            assert(tb2_mqtt_passthrough_write_local_publish(&session, publish,
                sizeof(publish), "x", 0, "local_test") == box_write_failure);
            assert(tb2_mqtt_passthrough_task(&session) == box_write_failure);
            assert(box_write_count == 0 && publish_completions == 0);
        }
        if (variant == 2) assert(session.capture_failed);
        /* A post-write capture failure cannot erase actual delivery; a failed
         * write or pre-write capture block must not adopt TONIES settings. */
        free(session.upstream_stream.data);
        osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
    }
}

static void test_downlink_subscription_qos(void)
{
    for (int grant = -1; grant <= 1; grant++) {
        tb2_mqtt_passthrough_session_t session;
        initialize(&session);
        session.established = true;
        session.worker = (tb2_mqtt_upstream_worker_t *)1;
        session.epoch = 1;
        session.cloud_phase = TB2_MQTT_CLOUD_READY;
        subscription_grant = grant;
        const uint16_t cloud_id = 65000;
        send_publish_direction(&session, 2, cloud_id, false, false);
        assert(observed == 1);
        assert(box_write_count == (grant < 0 ? 0U : 1U));
        assert(cloud_write_count == 1 && cloud_writes[0].length == 4);
        assert(cloud_writes[0].bytes[0] == 0x50);
        assert(LOAD16BE(cloud_writes[0].bytes + 2) == cloud_id);
        uint16_t local_id = 0;
        if (grant >= 0) {
            const uint8_t *wire = box_writes[0].bytes;
            assert(((wire[0] >> 1) & 3) == grant);
            size_t after_topic = 4 + LOAD16BE(wire + 2);
            if (grant == 1) {
                local_id = LOAD16BE(wire + after_topic);
                assert(local_id && local_id != cloud_id);
                after_topic += 2;
            }
            assert(box_writes[0].length == after_topic + 2);
            assert(memcmp(wire + after_topic, "{}", 2) == 0);
        }
        /* ACK the cloud's original QoS independently of the granted local QoS. */
        uint8_t pubrel[] = {0x62,2,0,0};
        STORE16BE(cloud_id, pubrel + 2);
        receive(&session, false, pubrel, sizeof(pubrel));
        assert(cloud_write_count == 2 && cloud_writes[1].bytes[0] == 0x70);
        assert(LOAD16BE(cloud_writes[1].bytes + 2) == cloud_id);
        if (grant == 1) {
            uint8_t puback[] = {0x40,2,0,0};
            STORE16BE(local_id, puback + 2);
            receive(&session, true, puback, sizeof(puback));
            assert(cloud_write_count == 2 && session.packet_ids == NULL);
        }
        free(session.box_stream.data);
        free(session.upstream_stream.data);
        osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
    }
}

typedef struct {
    tb2_mqtt_passthrough_session_t *session;
    unsigned sent, failed;
} local_completion_t;

static void local_completed(void *opaque, error_t error)
{
    local_completion_t *completion = opaque;
    assert(pthread_mutex_trylock(&completion->session->io_mutex) == 0);
    assert(pthread_mutex_unlock(&completion->session->io_mutex) == 0);
    if (error) completion->failed++;
    else completion->sent++;
}

static void test_atomic_queue_and_owned_completion(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = true;
    uint8_t publish[] = {0x30,5,0,1,'x','{','}'};
    local_completion_t completion = { .session = &session };
    tb2_mqtt_local_publish_t items[TB2_MQTT_INFLIGHT_MAX];
    for (size_t i = 0; i < TB2_MQTT_INFLIGHT_MAX; i++) items[i] = (tb2_mqtt_local_publish_t){
        publish, sizeof(publish), "x", 0, "local_test", local_completed, &completion, sizeof(completion)
    };
    items[1].packet = NULL;
    mqtt_delivery_result_t result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 2);
    assert(result.status == MQTT_DELIVERY_FAILED && session.box_write_count == 0);
    assert(completion.sent == 0 && completion.failed == 0);
    items[1].packet = publish;
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, TB2_MQTT_INFLIGHT_MAX);
    assert(result.status == MQTT_DELIVERY_QUEUED && session.box_write_count == TB2_MQTT_INFLIGHT_MAX);
    assert(completion.sent == 0 && box_write_count == 0);
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    assert(result.status == MQTT_DELIVERY_BUSY);
    publish[5] = '!'; /* immutable admission copy must not follow caller mutation */
    while (session.box_write_head != NULL) assert(drain_box(&session) == NO_ERROR);
    assert(completion.sent == TB2_MQTT_INFLIGHT_MAX && completion.failed == 0);
    assert(box_writes[0].bytes[5] == '{' && session.box_write_bytes == 0);
    session.box_write_stalled = true;
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    assert(result.status == MQTT_DELIVERY_BUSY && completion.sent == TB2_MQTT_INFLIGHT_MAX);
    session.box_write_stalled = false;
    session.processing_packet = true;
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    assert(result.status == MQTT_DELIVERY_BUSY);
    parser_session = &session;
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    parser_session = NULL;
    session.processing_packet = false;
    assert(result.status == MQTT_DELIVERY_QUEUED);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(completion.failed == 1 && session.box_write_count == 0);
    /* Completion metadata is included, not just encoded MQTT bytes. */
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    assert(result.status == MQTT_DELIVERY_QUEUED);
    size_t packet_budget = session.box_write_bytes - sizeof(completion);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    items[0].context_bytes = TB2_MQTT_BUFFER_LIMIT - packet_budget;
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items, 1);
    assert(result.status == MQTT_DELIVERY_QUEUED && session.box_write_bytes == TB2_MQTT_BUFFER_LIMIT);
    result = tb2_mqtt_passthrough_submit_local_batch(&session, items + 1, 1);
    assert(result.status == MQTT_DELIVERY_BUSY);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(completion.failed == 3);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_coalesced_protocol_backpressure_once(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = true;
    const uint8_t input[] = {0x34,7,0,1,'x',0,41,'{','}', 0x34,7,0,1,'x',0,42,'{','}'};
    assert(tb2_mqtt_process_stream(&session, TRUE, input, sizeof(input)) == NO_ERROR);
    assert(observed == 1 && session.box_write_count == 1 && session.box_stream.length == sizeof(input) / 2);
    box_write_pauses = true;
    assert(drain_box(&session) == NO_ERROR);
    for (unsigned i = 0; i < 3; i++)
        assert(tb2_mqtt_process_stream(&session, TRUE, NULL, 0) == NO_ERROR);
    assert(observed == 1 && session.box_write_count == 1);
    box_write_pauses = false;
    assert(drain_box(&session) == NO_ERROR);
    assert(tb2_mqtt_process_stream(&session, TRUE, NULL, 0) == NO_ERROR);
    assert(observed == 2 && session.box_write_count == 1 && session.box_stream.length == 0);
    assert(drain_box(&session) == NO_ERROR);
    assert(box_write_count == 2);
    tb2_mqtt_qos2_free(&session.blocked_qos2_box);
    free(session.box_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_cancel_preserves_only_transmitted_exchanges(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    const uint8_t publish[] = {0x32,7,0,1,'x',0,7,'{','}'};
    assert(tb2_mqtt_packet_id_add(&session, 7, 7, 1, FALSE) == NO_ERROR);
    assert(tb2_mqtt_record_packet_ex(&session, FALSE, publish, sizeof(publish), NULL, 0,
        3, "x", TRUE, NULL, FALSE, TRUE, "cloud", 7, 7, FALSE, FALSE, 0, NULL) == NO_ERROR);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(session.packet_ids == NULL);
    assert(tb2_mqtt_packet_id_add(&session, 7, 7, 2, FALSE) == NO_ERROR);
    session.packet_ids->transmitted = TRUE;
    assert(tb2_mqtt_record_packet_ex(&session, FALSE, publish, sizeof(publish), NULL, 0,
        3, "x", TRUE, NULL, TRUE, TRUE, "local_session_resume", 7, 7, FALSE, FALSE, 0, NULL) == NO_ERROR);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(session.packet_ids != NULL && session.packet_ids->transmitted);
    const uint8_t pubrel[] = {0x62,2,0,7};
    assert(tb2_mqtt_record_packet_ex(&session, FALSE, pubrel, sizeof(pubrel), NULL, 0,
        6, NULL, TRUE, NULL, TRUE, TRUE, "local_pubrel", 7, 7, FALSE, FALSE, 0, NULL) == NO_ERROR);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(session.packet_ids != NULL);
    session.packet_ids->transmitted = FALSE;
    const uint8_t puback[] = {0x40,2,0,7};
    assert(tb2_mqtt_record_packet_ex(&session, FALSE, puback, sizeof(puback), NULL, 0,
        4, NULL, TRUE, NULL, TRUE, TRUE, "local_puback", 7, 7, FALSE, FALSE, 0, NULL) == NO_ERROR);
    tb2_mqtt_box_writes_cancel(&session, ERROR_NOT_CONNECTED);
    assert(session.packet_ids != NULL); /* Different packet-ID direction. */
    tb2_mqtt_packet_ids_clear(&session.packet_ids);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_stalled_box_does_not_block_second_box(void)
{
    tb2_mqtt_passthrough_session_t first, second;
    initialize(&first);
    initialize(&second);
    first.established = second.established = true;
    first.connack_sent = second.connack_sent = true;
    first.upstream_configured = second.upstream_configured = true;
    const uint8_t publish[] = {0x30,5,0,1,'x','{','}'};
    assert(tb2_mqtt_passthrough_write_local_publish(&first, publish, sizeof(publish), "x", 0, "local_test") == NO_ERROR);
    box_write_pauses = true;
    assert(tb2_mqtt_passthrough_task(&first) == NO_ERROR);
    assert(first.box_write_active && first.box_write_count == 1 && box_write_count == 0);
    box_write_pauses = false;
    const uint8_t ping[] = {0xc0,0};
    assert(tb2_mqtt_process_stream(&second, TRUE, ping, sizeof(ping)) == NO_ERROR);
    assert(tb2_mqtt_passthrough_task(&second) == NO_ERROR);
    assert(box_write_count == 1 && box_writes[0].bytes[0] == 0xd0);
    assert(first.box_write_active && first.box_write_count == 1);
    assert(tb2_mqtt_passthrough_task(&first) == NO_ERROR);
    assert(first.box_write_count == 0 && box_write_count == 2);
    free(second.box_stream.data);
    osDeleteMutex(&first.ids_mutex); osDeleteMutex(&first.io_mutex);
    osDeleteMutex(&second.ids_mutex); osDeleteMutex(&second.io_mutex);
}

static void test_diagnostic_correlation_without_protocol_changes(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = true;
    mqtt_debug_test_reset(true, 1000);
    test_tls.fatalAlertReceived = TRUE;
    test_tls.closeNotifySent = TRUE;
    tb2_mqtt_debug_io(&session, 0, "test", "tls_fixture_flags", NO_ERROR, 0, 0, 1000);
    const cJSON *tls_flags = mqtt_debug_test_last("tls_fixture_flags");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(tls_flags, "tls_fatal_alert_received")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(tls_flags, "tls_close_notify_sent")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(tls_flags, "tls_fatal_alert_sent")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(tls_flags, "tls_close_notify_received")));
    assert(test_tls.fatalAlertReceived && test_tls.closeNotifySent);
    test_tls.fatalAlertReceived = FALSE;
    test_tls.closeNotifySent = FALSE;
    uint16_t id = 0;
    assert(tb2_mqtt_passthrough_reserve_local_packet_id(&session, &id) == NO_ERROR);
    uint8_t publish[] = {0x32,7,0,1,'x',(uint8_t)(id >> 8),(uint8_t)id,'{','}'};
    local_completion_t completion = {.session = &session};
    mqtt_delivery_result_t delivery = tb2_mqtt_passthrough_submit_local_publish(&session,
        publish, sizeof(publish), "x", id, "test_debug", local_completed, &completion, sizeof(completion));
    assert(delivery.status == MQTT_DELIVERY_QUEUED);
    assert(mqtt_debug_test_count("packet_created") == 1 && mqtt_debug_test_count("tx_complete") == 0);
    const cJSON *created = mqtt_debug_test_last("packet_created");
    double message = cJSON_GetNumberValue(cJSON_GetObjectItem(created, "message_id"));
    box_write_pauses = true;
    assert(drain_box(&session) == NO_ERROR);
    assert(!completion.sent && mqtt_debug_test_count("tx_complete") == 0);
    mqtt_debug_test_time(1300);
    box_write_pauses = false;
    assert(drain_box(&session) == NO_ERROR);
    assert(completion.sent == 1 && mqtt_debug_test_count("packet_created") == 1);
    const cJSON *sent = mqtt_debug_test_last("tx_complete");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(sent, "message_id")) == message);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(sent, "queue_to_fullwrite_ms")) == 300);
    publish[0] |= 0x08U;
    delivery = tb2_mqtt_passthrough_submit_local_publish(&session, publish, sizeof(publish),
        "x", id, "test_debug_retry", local_completed, &completion, sizeof(completion));
    assert(delivery.status == MQTT_DELIVERY_QUEUED);
    assert(mqtt_debug_test_count("packet_created") == 1);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("box_queued"), "retry_of")) == message);
    mqtt_debug_test_time(1350);
    assert(drain_box(&session) == NO_ERROR);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("tx_complete"), "retry_of")) == message);
    message = cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt_debug_test_last("tx_complete"), "message_id"));
    const uint8_t ack[] = {0x40,2,(uint8_t)(id >> 8),(uint8_t)id};
    mqtt_debug_test_time(1400);
    assert(tb2_mqtt_process_stream(&session, true, ack, 1) == NO_ERROR);
    assert(mqtt_debug_test_count("parser_wait") == 1 && mqtt_debug_test_count("packet_received") == 0);
    mqtt_debug_test_time(1500);
    assert(tb2_mqtt_process_stream(&session, true, ack + 1, sizeof(ack) - 1) == NO_ERROR);
    assert(local_acks == 1 && mqtt_debug_test_count("packet_received") == 1);
    const cJSON *received = mqtt_debug_test_last("mqtt_ack_received");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(received, "linked_message_id")) == message);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(received, "since_fullwrite_ms")) == 150);
    assert(tb2_mqtt_process_stream(&session, true, ack, sizeof(ack)) == NO_ERROR);
    assert(cJSON_GetObjectItem(mqtt_debug_test_last("mqtt_ack_received"), "linked_message_id") == NULL);
    assert(local_acks == 1); /* Diagnostics cannot replay the protocol observer. */
    tb2_mqtt_debug_slow(&session, "tx_io_mutex_wait", 1300);
    assert(mqtt_debug_test_count("tx_io_mutex_wait") == 1);
    mqtt_debug_test_time(1600);
    tb2_mqtt_debug_slow(&session, "tx_io_mutex_wait", 1300);
    tb2_mqtt_debug_slow(&session, "rx_io_mutex_wait", 1300);
    assert(mqtt_debug_test_count("tx_io_mutex_wait") == 1);
    assert(mqtt_debug_test_count("rx_io_mutex_wait") == 1);
    mqtt_debug_test_time(2500);
    tb2_mqtt_debug_slow(&session, "tx_io_mutex_wait", 2300);
    assert(mqtt_debug_test_count("tx_io_mutex_wait") == 2);
    free(session.box_stream.data);
    tb2_mqtt_packet_ids_clear(&session.packet_ids);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
    mqtt_debug_test_reset(false, 0);
}

static void queue_read_record(const uint8_t *bytes, size_t length, error_t result)
{
    assert(read_record_count < arraysize(read_records));
    assert(length <= sizeof(read_records[0].bytes));
    memcpy(read_records[read_record_count].bytes, bytes, length);
    read_records[read_record_count].length = length;
    read_records[read_record_count++].result = result;
}

static void test_two_record_publish_same_visit(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = session.upstream_configured = true;
    const uint8_t publish[] = {0x32,7,0,1,'x',0,41,'{','}'};
    queue_read_record(publish, 7, NO_ERROR);
    /* Returned bytes must survive a simultaneous retryable TLS result. */
    queue_read_record(publish + 7, 2, ERROR_WOULD_BLOCK);
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(read_calls == 2 && read_record_next == 2);
    assert(observed == 1 && session.box_stream.length == 0);
    assert(session.box_io_ops == 0 && session.box_write_count == 1);
    assert(box_write_count == 0); /* Four receive operations consumed the visit. */
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(observed == 1 && box_write_count == 1 && session.box_write_count == 0);
    expect_last(TB2_MQTT_PACKET_PUBACK, 41);
    free(session.box_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_shared_packet_limit_and_cloud_priority(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = session.upstream_configured = true;
    const uint8_t publish[] = {0x30,5,0,1,'x','{','}'};
    uint8_t batch[6 * sizeof(publish)];
    for (size_t i = 0; i < 6; i++) memcpy(batch + i * sizeof(publish), publish, sizeof(publish));
    queue_read_record(batch, sizeof(batch), NO_ERROR);
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(observed == TB2_MQTT_PACKETS_PER_TICK && session.packets_remaining == 0);
    assert(session.box_stream.length == 2 * sizeof(publish));
    /* Re-entering the parser does not grant a fresh per-call allowance. */
    assert(tb2_mqtt_process_stream(&session, TRUE, NULL, 0) == NO_ERROR);
    assert(observed == TB2_MQTT_PACKETS_PER_TICK);
    session.cloud_phase = TB2_MQTT_CLOUD_READY;
    assert(tb2_mqtt_process_stream(&session, FALSE, publish, sizeof(publish)) == NO_ERROR);
    assert(session.upstream_stream.length == sizeof(publish));
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(observed == 7 && session.box_stream.length == 0 && session.upstream_stream.length == 0);
    assert(box_write_count == 1); /* Buffered Cloud publish was not starved. */
    assert(session.packets_remaining == 1); /* Shared across both directions. */
    free(session.box_stream.data); free(session.upstream_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_receive_directions_cannot_starve_each_other(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = session.upstream_configured = true;
    session.cloud_phase = TB2_MQTT_CLOUD_READY;
    filter_allowed = false; /* Filtered Cloud packets still consume parser time. */
    /* Use an existing filter category: this target deliberately has no new
     * catch-all filter for the unrelated synthetic topic "x". */
    const uint8_t body[] = "\x00\x24" "toniebox/AABBCCDDEEFF/playback/state{}";
    uint8_t publish[64];
    size_t publish_length = packet(publish, 0x30, body, sizeof(body) - 1);
    uint8_t batch[16 * publish_length];
    for (size_t i = 0; i < 16; i++) memcpy(batch + i * publish_length, publish, publish_length);
    session.packets_remaining = 0;
    assert(tb2_mqtt_process_stream(&session, FALSE, batch, sizeof(batch)) == NO_ERROR);
    queue_read_record(batch, sizeof(batch) / 2, NO_ERROR);
    queue_read_record(batch, sizeof(batch) / 2, NO_ERROR);
    for (unsigned visit = 0; visit < 6; visit++) {
        size_t cloud_before = session.upstream_stream.length;
        size_t box_before = session.box_stream.length;
        unsigned reads_before = read_calls;
        assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
        assert(observed == (visit + 1) * TB2_MQTT_PACKETS_PER_TICK);
        assert(session.packets_remaining == 0);
        if (visit % 2 == 0) {
            assert(session.upstream_stream.length == cloud_before);
            assert(read_calls > reads_before || session.box_stream.length < box_before);
        } else {
            assert(session.upstream_stream.length == cloud_before - 4 * publish_length);
            assert(session.box_stream.length == box_before && read_calls == reads_before);
        }
    }
    assert(read_calls == 2 && box_write_count == 0);
    assert(session.box_stream.length == 4 * publish_length);
    assert(session.upstream_stream.length == 4 * publish_length);
    free(session.box_stream.data); free(session.upstream_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_drain_stops_on_no_progress_and_prioritizes_write(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    session.established = session.connack_sent = session.upstream_configured = true;
    read_would_block = true;
    for (unsigned i = 1; i <= 3; i++) {
        assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
        assert(read_calls == i); /* No loop through repeated WOULD_BLOCK. */
    }
    read_would_block = false;
    unsigned checks = socket_checks;
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(read_calls == 3 && socket_checks == checks + 1);
    const uint8_t local[] = {0x30,5,0,1,'x','{','}'};
    const uint8_t ping[] = {0xc0,0};
    assert(tb2_mqtt_passthrough_write_local_publish(&session,
        local, sizeof(local), "x", 0, "drain_order") == NO_ERROR);
    queue_read_record(ping, sizeof(ping), NO_ERROR);
    box_write_pauses = true;
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(read_calls == 3 && session.box_write_active && box_write_count == 0);
    box_write_pauses = false;
    assert(tb2_mqtt_passthrough_task(&session) == NO_ERROR);
    assert(read_calls == 4 && box_write_count == 2);
    assert(box_writes[0].bytes[0] == 0x30 && box_writes[1].bytes[0] == 0xd0);
    assert(session.box_io_ops == 0); /* One write + two reads + PINGRESP. */
    free(session.box_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void test_shutdown_log_does_not_close_transport(void)
{
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    connect_local(&session, true);
    const char *topics[] = {"toniebox/AABBCCDDEEFF/logs", "toniebox/AABBCCDDEEFF/metrics/fleet"};
    const char *payloads[] = {"{\"source\":\"main_sm\",\"message\":\"setting state STATE_SHUT_DOWN (51)\"}", "{}"};
    for (size_t i = 0; i < arraysize(topics); i++) {
        uint8_t body[256], data[260];
        size_t topic_length = strlen(topics[i]);
        size_t payload_length = strlen(payloads[i]);
        body[0] = topic_length >> 8; body[1] = topic_length;
        memcpy(body + 2, topics[i], topic_length);
        size_t offset = topic_length + 2;
        body[offset++] = 0; body[offset++] = 71 + i;
        memcpy(body + offset, payloads[i], payload_length);
        size_t length = packet(data, 0x32, body, offset + payload_length);
        receive(&session, true, data, length);
        expect_last(TB2_MQTT_PACKET_PUBACK, 71 + i);
        assert(session.established && !session.clean_disconnect);
    }
    const uint8_t ping[] = {0xc0, 0};
    receive(&session, true, ping, sizeof(ping));
    expect_last(13, 0);
    assert(observed == 2 && session.established && !session.clean_disconnect);
    free(session.box_stream.data);
    osDeleteMutex(&session.ids_mutex); osDeleteMutex(&session.io_mutex);
}

static void expect_global_status(const char *state, const char *error)
{
    HttpConnection connection = {0};
    assert(tb2_mqtt_passthrough_write_status(&connection) == NO_ERROR);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(last_global_status,
        "state")), state) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(last_global_status,
        "error_code")), error) == 0);
}

static void test_local_close_not_cloud_error(void)
{
    tb2_mqtt_passthrough_session_t first, second;
    initialize(&first);
    initialize(&second);
    mqtt_passthrough_status.active_sessions = 0;
    mqtt_passthrough_status.upstream_sessions = 0;
    mqtt_passthrough_status.error_code[0] = 0;
    tb2_mqtt_status_start();
    first.established = first.connack_sent = true;
    first.worker = (tb2_mqtt_upstream_worker_t *)1;
    first.epoch = 1;
    first.cloud_phase = TB2_MQTT_CLOUD_READY;
    tb2_mqtt_runtime_update(&first);
    expect_global_status("connected", "");

    tb2_mqtt_status_start();
    second.established = second.connack_sent = true;
    atomic_store(&second.box_write_error, ERROR_WRITE_FAILED);
    tb2_mqtt_status_finish(&second);
    expect_global_status("connected", "");
    expect_runtime(&first, true, true, "connected", "");
    /* A regular local DISCONNECT is not a failed Cloud attempt either. */
    tb2_mqtt_cloud_clear(&first, "local_disconnect");
    assert(first.upstream_error[0] == 0 && mqtt_passthrough_status.error_code[0] == 0);
    tb2_mqtt_status_finish(&first);
    expect_global_status("ready", "");
    strcpy(mqtt_passthrough_status.error_code, "upstream_unavailable");
    expect_global_status("ready", "");
    expect_runtime(NULL, false, false, "ready", "");

    first.runtime_cloud = false;
    tb2_mqtt_status_start();
    tb2_mqtt_cloud_clear(&first, "upstream_unavailable");
    tb2_mqtt_runtime_update(&first);
    expect_runtime(&first, true, false, "error", "upstream_unavailable");
    expect_global_status("error", "upstream_unavailable");
    tb2_mqtt_status_start();
    expect_global_status("error", "upstream_unavailable");
    tb2_mqtt_status_finish(&second);
    expect_global_status("error", "upstream_unavailable");

    first.capture_failed = true;
    strcpy(first.upstream_error, "capture_open_failed");
    tb2_mqtt_runtime_update(&first);
    expect_runtime(&first, true, false, "error", "capture_open_failed");
    expect_global_status("error", "capture_open_failed");
    test_settings.mqtt_client_upstream.enabled = false;
    expect_global_status("disabled", "");
    expect_runtime(NULL, false, false, "disabled", "");
    test_settings.mqtt_client_upstream.enabled = true;
    expect_global_status("error", "capture_open_failed");
    tb2_mqtt_status_finish(&first);
    expect_global_status("ready", "");
    osDeleteMutex(&first.ids_mutex); osDeleteMutex(&first.io_mutex);
    osDeleteMutex(&second.ids_mutex); osDeleteMutex(&second.io_mutex);
    cJSON_Delete(last_global_status); last_global_status = NULL;
}

int main(void)
{
    test_settings.commonName = "AABBCCDDEEFF";
    test_settings.internal.overlayUniqueId = "AABBCCDDEEFF";
    test_settings.internal.overlayNumber = 1;
    test_settings.mqtt_client_upstream.enabled = true;
    assert(osCreateMutex(&mqtt_passthrough_status.mutex));
    tb2_mqtt_passthrough_session_t session;
    initialize(&session);
    test_offline_transport(&session);
    test_offline_qos(&session);
    test_cloud_epoch_and_local_freshness(&session);
    test_local_response_duplicate(&session);
    test_policy_and_capture_failure(&session);
    test_clean_session_resume();
    test_takeover_waits_for_written_connack();
    test_reset_during_cloud_packet();
    test_per_box_runtime();
    test_independent_keepalives();
    test_downlink_completion();
    test_downlink_subscription_qos();
    test_atomic_queue_and_owned_completion();
    test_coalesced_protocol_backpressure_once();
    test_cancel_preserves_only_transmitted_exchanges();
    test_stalled_box_does_not_block_second_box();
    test_diagnostic_correlation_without_protocol_changes();
    test_two_record_publish_same_visit();
    test_shared_packet_limit_and_cloud_priority();
    test_receive_directions_cannot_starve_each_other();
    test_drain_stops_on_no_progress_and_prioritizes_write();
    test_local_close_not_cloud_error();
    test_shutdown_log_does_not_close_transport();
    puts("MQTT session PASS: offline protocol, QoS/reply duplicates, epochs, freshness IDs, capture, Clean0, connection state, keepalive and bounded receive drain");
    return 0;
}
