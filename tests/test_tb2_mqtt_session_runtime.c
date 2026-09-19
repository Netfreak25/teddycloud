/* Production declarations and functions are inserted verbatim by the runner. */
#include <assert.h>
#include <stdlib.h>
/* SESSION_DECLARATIONS */
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
static bool filter_allowed = true, privacy_blocked, capture_fails, consume_once;
static bool box_write_fails, capture_fails_after_write;
static unsigned observed, local_acks, worker_resets, worker_sends;
static unsigned publish_completions;
static int subscription_grant = 2;
static systime_t now = 1000;
static struct { uint8_t bytes[2048]; size_t length; } box_writes[128], cloud_writes[128];
static size_t box_write_count, cloud_write_count;
static char capture_action[64];
static mqtt_forward_route_t capture_route;

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
systime_t osGetSystemTime(void) { return now; }
void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *memory) { free(memory); }
bool_t osCreateMutex(OsMutex *mutex) { return pthread_mutex_init(mutex, NULL) == 0; }
void osDeleteMutex(OsMutex *mutex) { assert(pthread_mutex_destroy(mutex) == 0); }
void osAcquireMutex(OsMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void osReleaseMutex(OsMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }

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

static error_t tb2_mqtt_tls_write_all(TlsContext *destination, const uint8_t *data,
                                     size_t length)
{
    assert(destination == (TlsContext *)1);
    if (box_write_fails) return ERROR_WRITE_FAILED;
    assert(box_write_count < 128 && length <= sizeof(box_writes[0].bytes));
    memcpy(box_writes[box_write_count].bytes, data, length);
    box_writes[box_write_count++].length = length;
    if (capture_fails_after_write) capture_fails = true;
    return NO_ERROR;
}

static error_t tb2_mqtt_capture_packet_ex(tb2_mqtt_capture_t *capture,
    const char *direction, const uint8_t *data, size_t length,
    const uint8_t *wire, size_t wire_length, uint8_t type, const char *topic,
    bool_t forwarded, const char *filter, bool_t generated, bool_t complete,
    const char *action, uint16_t id, uint16_t wire_id, size_t removed,
    const mqtt_forward_filter_result_t *decision)
{
    (void)capture; (void)direction; (void)data; (void)length; (void)wire;
    (void)wire_length; (void)type; (void)topic; (void)forwarded; (void)filter;
    (void)generated; (void)complete; (void)id; (void)wire_id; (void)removed;
    snprintf(capture_action, sizeof(capture_action), "%s", action ? action : "");
    if (decision) capture_route = decision->route;
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
static error_t tb2_mqtt_forward_ready(tb2_mqtt_passthrough_session_t *session, bool_t incoming)
{
    (void)session; assert(incoming);
    return NO_ERROR; /* No additional socket bytes; test feeds the real stream parser. */
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
    session->box_tls = (TlsContext *)1;
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
    publish_completions = 0;
    subscription_grant = 2;
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
    assert(tb2_mqtt_process_stream(session, incoming, data, length) == NO_ERROR);
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
    assert(tb2_mqtt_passthrough_is_established(session) == local);
    assert(tb2_mqtt_passthrough_is_upstream_connected(session) == cloud);
    cJSON *json = cJSON_CreateObject();
    assert(json);
    tb2_mqtt_passthrough_add_runtime_status(session, json);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "localConnected")) == local);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "upstreamState")), state) == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "upstreamError")), error) == 0);
    cJSON_Delete(json);
}

static void test_per_box_runtime(void)
{
    tb2_mqtt_passthrough_session_t first, second;
    initialize(&first);
    initialize(&second);
    first.established = second.established = true;
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
    for (unsigned variant = 0; variant < 4; variant++) {
        tb2_mqtt_passthrough_session_t session;
        initialize(&session);
        session.established = true;
        session.publish_completed = publish_completed;
        box_write_fails = variant == 1;
        capture_fails_after_write = variant == 2;
        capture_fails = variant == 3;
        error_t result = tb2_mqtt_process_stream(&session, false, publish, sizeof(publish));
        assert(result == (variant == 1 ? ERROR_WRITE_FAILED : NO_ERROR));
        unsigned delivered = variant == 0 || variant == 2 ? 1 : 0;
        assert(box_write_count == delivered && publish_completions == delivered);
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
    test_reset_during_cloud_packet();
    test_per_box_runtime();
    test_independent_keepalives();
    test_downlink_completion();
    test_downlink_subscription_qos();
    puts("MQTT session PASS: offline protocol, QoS/reply duplicates, epochs, freshness IDs, capture, Clean0, per-box status and keepalive");
    return 0;
}
