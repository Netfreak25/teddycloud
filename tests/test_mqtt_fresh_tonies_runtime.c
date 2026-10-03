/* Production types and freshness functions are inserted verbatim by the runner. */
#include <assert.h>
#include <byteswap.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "mqtt_debug.h"
#include "core/net.h"
#include "core/socket.h"
#include "tls.h"
#include "toniebox_state.h"
#include "tb2_mqtt_passthrough.h"
#include "handler.h"
#include "mqtt_server.h"
#include "mutex_manager.h"
#undef TRACE_DEBUG
#undef TRACE_INFO
#undef TRACE_WARNING
#undef TRACE_ERROR
/* Preserve the project macros' if/block form: missing caller braces must fail. */
#define TRACE_DEBUG(...) if (TRUE) { record_log(0, __VA_ARGS__); }
#define TRACE_INFO(...) if (TRUE) { record_log(1, __VA_ARGS__); }
#define TRACE_WARNING(...) if (TRUE) { record_log(2, __VA_ARGS__); }
#define TRACE_ERROR(...) if (TRUE) { record_log(3, __VA_ARGS__); }
/* SERVER_TYPES */

uint64_t tb2_mqtt_passthrough_debug_owner(const tb2_mqtt_passthrough_session_t *session)
{ (void)session; return 0; }
void mqtt_debug_test_reset(bool active, uint64_t now);
void mqtt_debug_test_time(uint64_t now);
unsigned mqtt_debug_test_count(const char *stage);
const cJSON *mqtt_debug_test_last(const char *stage);

static MqttClientConnection connections[MQTT_MAX_CONNECTIONS];
static MqttFreshToniesPublishState fresh_tonies_publish_state[MAX_OVERLAYS];
static uint64_t fresh_tonies_generation;
static bool_t fresh_tonies_reload_pending;
static bool_t fresh_tonies_ready;
static settings_t settings;
static settings_t other_settings;
static uint64_t cache[32];
static size_t cache_count;
static uint64_t other_cache[32];
static size_t other_cache_count;
static time_t wall_now;
static uint32_t monotonic_now;
static bool_t clock_available, publish_succeeds, lose_clock_on_publish;
static bool_t allocation_succeeds;
static bool_t defer_publish, lock_held;
static tb2_mqtt_local_write_completed_t deferred_completed;
static void *deferred_context;
static uint16_t next_packet_id;
static unsigned warning_count, info_retries, debug_retries;
static struct {
    uint32_t at;
    uint16_t packet_id;
    bool_t duplicate;
    char payload[40];
} publishes[512];
static size_t publish_count;

static void record_log(unsigned severity, const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (severity == 2) warning_count++;
    if (strstr(message, "fresh-tonies retry")) {
        if (severity == 0) debug_retries++;
        if (severity == 1) info_retries++;
    }
}

time_t time(time_t *out)
{
    if (out != NULL) *out = wall_now;
    return wall_now;
}

static bool_t mqtt_monotonic_ms(uint32_t *now)
{
    if (!clock_available) return FALSE;
    *now = monotonic_now;
    return TRUE;
}

void *osAllocMem(size_t size) { return allocation_succeeds ? malloc(size) : NULL; }
void osFreeMem(void *pointer) { free(pointer); }
void mutex_lock(mutex_id_t id) { assert(fresh_tonies_ready && id == MUTEX_MQTT_SESSION && !lock_held); lock_held = TRUE; }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_MQTT_SESSION && lock_held); lock_held = FALSE; }

settings_t *get_settings_id(uint8_t overlay)
{
    if (overlay == 1) return &settings;
    if (overlay == 2) return &other_settings;
    return NULL;
}

uint64_t *settings_get_u64_array_id(const char *name, uint8_t overlay, size_t *length)
{
    assert(!strcmp(name, "internal.freshnessCache"));
    assert(overlay == 1 || overlay == 2);
    *length = overlay == 1 ? cache_count : other_cache_count;
    return overlay == 1 ? cache : other_cache;
}

bool settings_set_u64_array_id(const char *name, const uint64_t *uids, size_t count, uint8_t overlay)
{
    assert(lock_held && !strcmp(name, "internal.freshnessCache") && (overlay == 1 || overlay == 2) && count <= 32);
    if (!allocation_succeeds) return false;
    if (count > 0) memcpy(overlay == 1 ? cache : other_cache, uids, count * sizeof(uint64_t));
    if (overlay == 1) cache_count = count;
    else other_cache_count = count;
    return true;
}

bool settings_set_bool_id(const char *name, bool value, uint8_t overlay)
{ assert(!strcmp(name, "internal.freshnessCacheChanged")); get_settings_id(overlay)->internal.freshnessCacheChanged = value; return true; }
void freshness_cache_sync_source_changed_uids(settings_t *target) { assert((target == &settings || target == &other_settings) && lock_held); }

static bool_t mqtt_fresh_tonies_topic(MqttClientConnection *conn, char *topic, size_t size)
{
    assert(conn->active && size >= sizeof("toniebox/AABBCCDDEEFF/fresh-tonies"));
    strcpy(topic, "toniebox/AABBCCDDEEFF/fresh-tonies");
    return TRUE;
}

static bool_t mqtt_connection_has_sub(MqttClientConnection *conn, const char *topic)
{
    assert(conn->active && strstr(topic, "/fresh-tonies"));
    return TRUE;
}

static mqtt_delivery_result_t mqtt_connection_publish_packet_locked(MqttClientConnection *conn,
    const char *topic, const char *payload, uint8_t qos, bool_t duplicate,
    uint16_t *packet_id, const char *action,
    tb2_mqtt_local_write_completed_t completed, void *context, size_t context_bytes)
{
    assert(conn->active && strstr(topic, "/fresh-tonies") && qos == 1);
    assert(!strcmp(action, "local_freshness_publish"));
    assert(lock_held && context_bytes == sizeof(MqttFreshDelivery));
    if (!publish_succeeds) return mqtt_delivery_result(MQTT_DELIVERY_BUSY, ERROR_WOULD_BLOCK);
    if (duplicate) assert(*packet_id != 0);
    else { assert(*packet_id == 0); *packet_id = ++next_packet_id; }
    assert(publish_count < sizeof(publishes) / sizeof(publishes[0]));
    publishes[publish_count].at = monotonic_now;
    publishes[publish_count].packet_id = *packet_id;
    publishes[publish_count].duplicate = duplicate;
    snprintf(publishes[publish_count].payload, sizeof(publishes[publish_count].payload),
             "%s", payload);
    publish_count++;
    if (lose_clock_on_publish) clock_available = FALSE;
    if (defer_publish) {
        assert(!deferred_completed);
        deferred_completed = completed; deferred_context = context;
        return mqtt_delivery_result(MQTT_DELIVERY_QUEUED, NO_ERROR);
    }
    return mqtt_delivery_result(MQTT_DELIVERY_SENT, NO_ERROR);
}

/* SERVER_FUNCTIONS */

static MqttClientConnection *initialize(size_t entries)
{
    assert(!deferred_completed);
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
        mqtt_fresh_tonies_reset_connection(&connections[i]);
    mqtt_server_freshness_forget_overlay(1);
    mqtt_server_freshness_forget_overlay(2);
    memset(connections, 0, sizeof(connections));
    memset(fresh_tonies_publish_state, 0, sizeof(fresh_tonies_publish_state));
    memset(&settings, 0, sizeof(settings));
    memset(&other_settings, 0, sizeof(other_settings));
    other_cache_count = 0;
    settings.commonName = "AABBCCDDEEFF";
    settings.internal.overlayUniqueId = "AABBCCDDEEFF";
    settings.internal.overlayNumber = 1;
    settings.internal.config_used = TRUE;
    settings.internal.freshnessCacheChanged = TRUE;
    cache[0] = 1; cache[1] = 2; cache[2] = 2; cache_count = entries;
    wall_now = 1000; monotonic_now = 50000;
    clock_available = publish_succeeds = allocation_succeeds = TRUE;
    lose_clock_on_publish = FALSE;
    defer_publish = FALSE;
    warning_count = info_retries = debug_retries = 0;
    publish_count = 0; next_packet_id = 0;
    MqttClientConnection *conn = &connections[0];
    conn->active = true; conn->box_connection = TRUE;
    conn->client_ctx.settings = &settings;
    conn->client_ctx.mqtt_connection = conn;
    mqtt_mark_fresh_tonies_pending(&settings, "test");
    wall_now += MQTT_FRESH_TONIES_DEBOUNCE_SEC;
    return conn;
}

static MqttFreshTonieEntry *find_entry(uint64_t uid)
{ return mqtt_fresh_tonie_find(&fresh_tonies_publish_state[1], uid); }

static void sync_entries(void)
{ mutex_lock(MUTEX_MQTT_SESSION); assert(mqtt_fresh_tonies_sync(&settings)); mutex_unlock(MUTEX_MQTT_SESSION); }

static void pump_at(MqttClientConnection *conn, uint32_t now)
{
    monotonic_now = now;
    assert(mqtt_fresh_tonies_pump(conn));
    assert(conn->active && settings.internal.freshnessCacheChanged);
}

static void test_schedule_and_delayed_ack(void)
{
    MqttClientConnection *conn = initialize(3);
    const uint32_t start = monotonic_now;
    pump_at(conn, start);
    MqttFreshTonieEntry *first = conn->fresh_tonie_inflight;
    assert(first && first->uid == 1 && find_entry(2));
    assert(find_entry(2)->next == NULL); /* Existing cache-order deduplication. */
    const uint16_t id = conn->fresh_tonie_packet_id;
    pump_at(conn, start + 4999); assert(publish_count == 1);
    wall_now = 900000; /* Wall-clock jumps never affect retry scheduling. */
    pump_at(conn, start + 5000); assert(publish_count == 2);
    wall_now = 1;
    pump_at(conn, start + 10000); assert(publish_count == 3);
    pump_at(conn, start + 15000); assert(publish_count == 3 && warning_count == 1);
    pump_at(conn, start + 39999); assert(publish_count == 3 && warning_count == 1);
    pump_at(conn, start + 40000); assert(publish_count == 4);
    pump_at(conn, start + 69999); assert(publish_count == 4);
    pump_at(conn, start + 70000); assert(publish_count == 5);
    assert(warning_count == 1 && info_retries == 2 && debug_retries == 2);
    assert(fresh_tonies_publish_state[1].last_publish_at == (uint32_t)wall_now);
    const uint32_t offsets[] = {0, 5000, 10000, 40000, 70000};
    for (size_t i = 0; i < publish_count; i++) {
        assert(publishes[i].at == start + offsets[i]);
        assert(publishes[i].packet_id == id && publishes[i].duplicate == (i != 0));
        assert(!strcmp(publishes[i].payload, publishes[0].payload));
    }
    assert(!mqtt_handle_fresh_tonies_puback(conn, id + 1));
    assert(conn->fresh_tonie_inflight == first && !first->delivered);
    assert(mqtt_handle_fresh_tonies_puback(conn, id));
    assert(find_entry(1)->delivered && !conn->fresh_tonie_inflight);
    assert(!conn->fresh_tonie_sent_at_valid && !conn->fresh_tonie_slow_retry_logged);
    assert(!conn->fresh_tonie_attempts && !conn->fresh_tonie_packet_id);
    assert(cache_count == 3 && cache[0] == 1 && settings.internal.freshnessCacheChanged);
    wall_now = 1002;
    pump_at(conn, start + 70001);
    assert(publish_count == 6 && conn->fresh_tonie_inflight->uid == 2);
    assert(conn->fresh_tonie_packet_id != id && !publishes[5].duplicate);
    assert(!mqtt_handle_fresh_tonies_puback(conn, id));
}

static void test_counter_saturates_and_reset(void)
{
    MqttClientConnection *conn = initialize(1);
    pump_at(conn, monotonic_now);
    conn->fresh_tonie_attempts = UINT8_MAX - 1;
    pump_at(conn, monotonic_now + 30000);
    assert(conn->fresh_tonie_attempts == UINT8_MAX);
    for (unsigned i = 0; i < 3; i++) pump_at(conn, monotonic_now + 30000);
    assert(conn->fresh_tonie_attempts == UINT8_MAX && warning_count == 1);
    assert(conn->fresh_tonie_packet_id == 1 && !conn->fresh_tonie_inflight->delivered);
    mqtt_fresh_tonies_reset_connection(conn);
    assert(!conn->fresh_tonie_inflight && find_entry(1));
    assert(!conn->fresh_tonie_packet_id && !conn->fresh_tonie_attempts);
    assert(!conn->fresh_tonie_sent_at_valid && !conn->fresh_tonie_slow_retry_logged);
    assert(settings.internal.freshnessCacheChanged && cache_count == 1);
}

static void test_clock_failure_and_wrap(void)
{
    MqttClientConnection *conn = initialize(1);
    clock_available = FALSE;
    pump_at(conn, monotonic_now);
    assert(publish_count == 1 && !conn->fresh_tonie_sent_at_valid);
    pump_at(conn, monotonic_now + 100000); assert(publish_count == 1);
    clock_available = TRUE;
    pump_at(conn, monotonic_now); assert(publish_count == 1 && conn->fresh_tonie_sent_at_valid);
    pump_at(conn, monotonic_now + 4999); assert(publish_count == 1);
    lose_clock_on_publish = TRUE;
    pump_at(conn, monotonic_now + 1);
    assert(publish_count == 2 && !conn->fresh_tonie_sent_at_valid);
    pump_at(conn, monotonic_now + 100000); assert(publish_count == 2);
    lose_clock_on_publish = FALSE; clock_available = TRUE;
    pump_at(conn, monotonic_now); assert(publish_count == 2);
    pump_at(conn, monotonic_now + 5000); assert(publish_count == 3);
    clock_available = FALSE;
    pump_at(conn, monotonic_now + 40000); assert(publish_count == 3);
    clock_available = TRUE;
    pump_at(conn, monotonic_now); assert(publish_count == 4);
    conn = initialize(1);
    const uint32_t start = UINT32_MAX - 2000U;
    pump_at(conn, start);
    pump_at(conn, start + 4999U); assert(publish_count == 1);
    pump_at(conn, start + 5000U); assert(publish_count == 2);
    assert(conn->fresh_tonie_packet_id == 1);
}

static void test_sync_and_targeted_changes_keep_inflight(void)
{
    MqttClientConnection *conn = initialize(2);
    pump_at(conn, monotonic_now);
    pump_at(conn, monotonic_now + 5000);
    pump_at(conn, monotonic_now + 5000);
    pump_at(conn, monotonic_now + 5000);
    MqttFreshTonieEntry *inflight = conn->fresh_tonie_inflight;
    const uint16_t id = conn->fresh_tonie_packet_id;
    const uint32_t sent_at = conn->fresh_tonie_sent_at;
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 2));
    assert(conn->fresh_tonie_inflight == inflight && conn->fresh_tonie_packet_id == id);
    assert(conn->fresh_tonie_attempts == 3 && conn->fresh_tonie_sent_at == sent_at);
    assert(conn->fresh_tonie_slow_retry_logged && !inflight->delivered);
    cache[0] = 2; cache_count = 1;
    sync_entries();
    assert(conn->fresh_tonie_inflight == inflight && find_entry(1) == NULL);
    pump_at(conn, sent_at + 30000);
    assert(publish_count == 4 && publishes[3].packet_id == id);
    assert(!strcmp(publishes[0].payload, publishes[3].payload));
    assert(mqtt_handle_fresh_tonies_puback(conn, id));
    assert(find_entry(2) && !find_entry(2)->next);
    assert(settings.internal.freshnessCacheChanged && cache_count == 1 && cache[0] == 2);
    pump_at(conn, monotonic_now);
    assert(conn->fresh_tonie_inflight->uid == 2 && conn->fresh_tonie_packet_id != id);
}

static void test_unsent_publish_does_not_advance_delivery(void)
{
    MqttClientConnection *conn = initialize(1);
    pump_at(conn, monotonic_now);
    const uint32_t sent_at = conn->fresh_tonie_sent_at;
    MqttFreshTonieEntry *entry = conn->fresh_tonie_inflight;
    publish_succeeds = FALSE;
    monotonic_now += 5000;
    assert(!mqtt_fresh_tonies_pump(conn));
    assert(publish_count == 1 && conn->fresh_tonie_attempts == 1);
    assert(conn->fresh_tonie_sent_at == sent_at && conn->fresh_tonie_inflight == entry);
    assert(conn->active && !entry->delivered && settings.internal.freshnessCacheChanged);
    publish_succeeds = TRUE; allocation_succeeds = FALSE;
    assert(!mqtt_fresh_tonies_pump(conn));
    assert(publish_count == 1 && conn->fresh_tonie_attempts == 1);
    allocation_succeeds = TRUE;
    pump_at(conn, monotonic_now);
    assert(publish_count == 2 && conn->fresh_tonie_packet_id == 1);
}

static void complete_deferred(error_t error)
{
    assert(deferred_completed);
    tb2_mqtt_local_write_completed_t completed = deferred_completed;
    void *context = deferred_context;
    deferred_completed = NULL; deferred_context = NULL;
    completed(context, error);
}

static void test_deferred_completion_retains_uid_and_retry_boundary(void)
{
    MqttClientConnection *conn = initialize(2);
    defer_publish = TRUE;
    pump_at(conn, monotonic_now);
    assert(conn->fresh_tonie_queued && conn->fresh_tonie_queued_uid == 1);
    assert(!conn->fresh_tonie_inflight && !conn->fresh_tonie_attempts);
    assert(!conn->fresh_tonie_sent_at_valid);
    assert(!mqtt_handle_fresh_tonies_puback(conn, 1));
    pump_at(conn, monotonic_now + 10000);
    assert(publish_count == 1);
    cache[0] = 2; cache_count = 1;
    sync_entries();
    assert(find_entry(1) == NULL); /* Transport owns its immutable UID/generation. */
    complete_deferred(NO_ERROR);
    assert(!conn->fresh_tonie_queued && conn->fresh_tonie_inflight->uid == 1);
    assert(conn->fresh_tonie_attempts == 1 && conn->fresh_tonie_sent_at == monotonic_now);
    assert(mqtt_handle_fresh_tonies_puback(conn, 1));
    assert(find_entry(1) == NULL);

    conn = initialize(1);
    defer_publish = TRUE;
    pump_at(conn, monotonic_now);
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    uint64_t newer = find_entry(1)->generation;
    complete_deferred(NO_ERROR);
    assert(mqtt_handle_fresh_tonies_puback(conn, 1));
    assert(!find_entry(1)->delivered && find_entry(1)->generation == newer && settings.internal.freshnessCacheChanged);

    conn = initialize(1);
    defer_publish = TRUE;
    pump_at(conn, monotonic_now);
    complete_deferred(ERROR_WRITE_FAILED);
    assert(!conn->fresh_tonie_queued && !conn->fresh_tonie_inflight);
    assert(!conn->fresh_tonie_attempts && !conn->fresh_tonie_sent_at_valid);
}

static void test_http_notification_and_reconnect(void)
{
    MqttClientConnection *conn = initialize(2);
    mqtt_freshness_snapshot_t *snapshot = mqtt_server_freshness_begin(1);
    size_t count;
    const uint64_t *view = mqtt_server_freshness_cache(snapshot, &count);
    assert(snapshot && count == 2 && view[0] == 1 && view[1] == 2);
    assert(mqtt_server_freshness_prepare(snapshot, view, count) == NO_ERROR);
    pump_at(conn, monotonic_now); /* Reserved before HTTP writes, no MQTT admission. */
    assert(publish_count == 0);
    mqtt_server_freshness_finish(snapshot, TRUE);
    assert(find_entry(1)->delivered && find_entry(2)->delivered);
    for (unsigned i = 0; i < 5; i++) {
        mqtt_fresh_tonies_reset_connection(conn);
        assert(mqtt_server_publish_fresh_tonies(&conn->client_ctx));
        pump_at(conn, monotonic_now + 1000);
    }
    assert(publish_count == 0 && !fresh_tonies_publish_state[1].pending);
    assert(cache_count == 2); /* Notified is not content-updated. */
    mqtt_server_freshness_forget_overlay(1); /* Simulate process-local RAM reset. */
    assert(mqtt_server_publish_fresh_tonies_for_overlay(1));
    wall_now += MQTT_FRESH_TONIES_DEBOUNCE_SEC;
    pump_at(conn, monotonic_now);
    assert(publish_count == 1 && conn->fresh_tonie_inflight->uid == 1);
}

static void test_http_abort_and_concurrent_source_change(void)
{
    MqttClientConnection *conn = initialize(1);
    mqtt_freshness_snapshot_t *snapshot = mqtt_server_freshness_begin(1);
    assert(snapshot && mqtt_server_freshness_prepare(snapshot, cache, 1) == NO_ERROR);
    mqtt_server_freshness_finish(snapshot, FALSE);
    pump_at(conn, monotonic_now);
    assert(publish_count == 1 && !find_entry(1)->delivered);

    conn = initialize(1);
    snapshot = mqtt_server_freshness_begin(1);
    assert(snapshot && mqtt_server_freshness_prepare(snapshot, cache, 1) == NO_ERROR);
    uint64_t old_generation = find_entry(1)->generation;
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    assert(find_entry(1)->generation != old_generation && find_entry(1)->http_claims == 0);
    mqtt_server_freshness_finish(snapshot, TRUE);
    assert(!find_entry(1)->delivered);
    pump_at(conn, monotonic_now);
    assert(publish_count == 1);

    conn = initialize(1);
    snapshot = mqtt_server_freshness_begin(1);
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    assert(mqtt_server_freshness_prepare(snapshot, NULL, 0) == NO_ERROR);
    assert(cache_count == 1 && cache[0] == 1); /* Older fresh answer cannot erase G2. */
    mqtt_server_freshness_finish(snapshot, TRUE);
    assert(!find_entry(1)->delivered);
    pump_at(conn, monotonic_now);
    assert(publish_count == 1);
}

static void test_old_inflight_ack_does_not_confirm_new_generation(void)
{
    MqttClientConnection *conn = initialize(1);
    pump_at(conn, monotonic_now);
    uint64_t first_generation = conn->fresh_tonie_inflight->generation;
    uint16_t first_id = conn->fresh_tonie_packet_id;
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    assert(find_entry(1)->generation != first_generation);
    pump_at(conn, monotonic_now + 5000);
    assert(conn->fresh_tonie_inflight->generation == first_generation);
    assert(mqtt_handle_fresh_tonies_puback(conn, first_id));
    assert(!find_entry(1)->delivered);
    pump_at(conn, monotonic_now);
    assert(publish_count == 3 && conn->fresh_tonie_packet_id != first_id);
    assert(conn->fresh_tonie_inflight->generation == find_entry(1)->generation);
    assert(mqtt_handle_fresh_tonies_puback(conn, conn->fresh_tonie_packet_id));
    assert(find_entry(1)->delivered);
}

static void test_concurrent_http_discovery_and_completion(void)
{
    MqttClientConnection *conn = initialize(0);
    const uint64_t uid = 9;
    mqtt_freshness_snapshot_t *a = mqtt_server_freshness_begin(1);
    mqtt_freshness_snapshot_t *b = mqtt_server_freshness_begin(1);
    assert(a && b && mqtt_server_freshness_prepare(a, &uid, 1) == NO_ERROR);
    uint64_t generation = find_entry(uid)->generation;
    assert(mqtt_server_freshness_prepare(b, &uid, 1) == NO_ERROR);
    assert(find_entry(uid)->http_claims == 2 && find_entry(uid)->generation == generation);
    mqtt_server_freshness_finish(a, FALSE);
    mqtt_server_freshness_finish(b, TRUE);
    assert(find_entry(uid)->delivered && !find_entry(uid)->http_claims);
    pump_at(conn, monotonic_now);
    assert(publish_count == 0);

    conn = initialize(0);
    a = mqtt_server_freshness_begin(1);
    b = mqtt_server_freshness_begin(1);
    assert(mqtt_server_freshness_prepare(a, &uid, 1) == NO_ERROR);
    mqtt_server_freshness_finish(a, TRUE);
    cache_count = 0; /* Successful content/version observation removes stale entry. */
    sync_entries();
    assert(find_entry(uid) && !find_entry(uid)->present); /* Retained tombstone. */
    assert(mqtt_server_freshness_prepare(b, &uid, 1) == NO_ERROR);
    assert(cache_count == 0); /* Late discovery response must not resurrect it. */
    mqtt_server_freshness_finish(b, TRUE);
    assert(find_entry(uid) == NULL);
    assert(mqtt_fresh_tonies_pump(conn) && publish_count == 0);
}

static void test_snapshot_identity_and_allocation_failures(void)
{
    MqttClientConnection *conn = initialize(1);
    mqtt_freshness_snapshot_t *old = mqtt_server_freshness_begin(1);
    mqtt_server_freshness_forget_overlay(1);
    mqtt_freshness_snapshot_t *current = mqtt_server_freshness_begin(1);
    assert(current && current->epoch != old->epoch);
    assert(mqtt_server_freshness_prepare(old, cache, 1) == ERROR_INVALID_PARAMETER);
    mqtt_server_freshness_finish(old, TRUE);
    assert(fresh_tonies_publish_state[1].snapshots == 1);
    allocation_succeeds = FALSE;
    assert(mqtt_server_freshness_prepare(current, cache, 1) == ERROR_OUT_OF_MEMORY);
    assert(cache_count == 1 && !find_entry(1)->delivered && !find_entry(1)->http_claims);
    allocation_succeeds = TRUE;
    mqtt_server_freshness_finish(current, FALSE);
    assert(!fresh_tonies_publish_state[1].snapshots);
    current = mqtt_server_freshness_begin(1);
    assert(mqtt_server_freshness_prepare(current, cache, 1) == NO_ERROR);
    mqtt_server_freshness_begin_reload();
    mqtt_server_freshness_finish(current, TRUE);
    assert(!fresh_tonies_publish_state[1].snapshots && !find_entry(1)->http_claims);
    assert(mqtt_fresh_tonies_pump(conn) && publish_count == 0);
    mqtt_server_freshness_reconcile_overlays();
    wall_now += MQTT_FRESH_TONIES_DEBOUNCE_SEC;
    pump_at(conn, monotonic_now);
    assert(publish_count == 1);
}

static void test_http_keeps_admitted_qos_and_boxes_independent(void)
{
    MqttClientConnection *conn = initialize(1);
    pump_at(conn, monotonic_now);
    uint16_t id = conn->fresh_tonie_packet_id;
    mqtt_freshness_snapshot_t *snapshot = mqtt_server_freshness_begin(1);
    assert(mqtt_server_freshness_prepare(snapshot, cache, 1) == NO_ERROR);
    mqtt_server_freshness_finish(snapshot, TRUE);
    assert(conn->fresh_tonie_inflight && find_entry(1)->delivered);
    pump_at(conn, monotonic_now + 5000);
    assert(publish_count == 2 && conn->fresh_tonie_packet_id == id);
    assert(mqtt_handle_fresh_tonies_puback(conn, id));
    pump_at(conn, monotonic_now);
    assert(publish_count == 2);

    other_settings.commonName = "112233445566";
    other_settings.internal.overlayUniqueId = "112233445566";
    other_settings.internal.overlayNumber = 2;
    other_settings.internal.config_used = TRUE;
    other_settings.internal.freshnessCacheChanged = TRUE;
    other_cache[0] = 1; other_cache_count = 1;
    MqttClientConnection *other = &connections[1];
    other->active = true; other->box_connection = TRUE;
    other->client_ctx.settings = &other_settings;
    assert(mqtt_server_publish_fresh_tonies_for_overlay(2));
    wall_now += MQTT_FRESH_TONIES_DEBOUNCE_SEC;
    assert(mqtt_fresh_tonies_pump(other));
    assert(publish_count == 3 && other->fresh_tonie_inflight->uid == 1);
    assert(mqtt_handle_fresh_tonies_puback(other, other->fresh_tonie_packet_id));
    assert(mqtt_server_publish_fresh_tonie_for_overlay(1, 1));
    assert(!find_entry(1)->delivered && fresh_tonies_publish_state[2].entries->delivered);
    settings.internal.overlayUniqueId = "FFEEDDCCBBAA";
    mqtt_server_freshness_reconcile_overlays();
    assert(!fresh_tonies_publish_state[1].entries && fresh_tonies_publish_state[2].entries->delivered);
}

static void test_cached_diagnostic_owner_has_no_relay_dependency(void)
{
    MqttClientConnection conn = {0};
    assert(mqtt_connection_debug_owner(NULL) == 0);
    assert(mqtt_connection_debug_owner(&conn) == 0);
    atomic_store_explicit(&conn.debug_owner, 42, memory_order_release);
    assert(mqtt_connection_debug_owner(&conn) == 42);
    /* The fixed-slot snapshot remains safe even if no relay can be accessed. */
    conn.passthrough = (tb2_mqtt_passthrough_session_t *)1;
    assert(mqtt_connection_debug_owner(&conn) == 42);
    atomic_store_explicit(&conn.debug_owner, 0, memory_order_release);
    assert(mqtt_connection_debug_owner(&conn) == 0);
}

static void test_hass_diagnostic_throttle_has_no_relay_dependency(void)
{
    MqttClientConnection conn = {0};
    client_ctx_t client_ctx = {0};
    client_ctx.mqtt_connection = &conn;
    conn.passthrough = (tb2_mqtt_passthrough_session_t *)1;
    atomic_store_explicit(&conn.debug_owner, 42, memory_order_release);
    mqtt_debug_test_reset(true, 5000);
    mqtt_server_debug_hass_duration(&client_ctx, 4901);
    assert(mqtt_debug_test_count("hass_call_slow") == 0);
    mqtt_server_debug_hass_duration(&client_ctx, 4900);
    assert(mqtt_debug_test_count("hass_call_slow") == 1);
    mqtt_server_debug_hass_duration(&client_ctx, 4800);
    assert(mqtt_debug_test_count("hass_call_slow") == 1);
    mqtt_debug_test_time(5999);
    mqtt_server_debug_hass_duration(&client_ctx, 5800);
    assert(mqtt_debug_test_count("hass_call_slow") == 1);
    mqtt_debug_test_time(6000);
    mqtt_server_debug_hass_duration(&client_ctx, 5900);
    assert(mqtt_debug_test_count("hass_call_slow") == 2);
    assert(atomic_load_explicit(&conn.debug_hass_report_ms, memory_order_relaxed) == 6000);
    const cJSON *event = mqtt_debug_test_last("hass_call_slow");
    assert(cJSON_GetObjectItemCaseSensitive(event, "message_id")->valuedouble == 0);
    mqtt_debug_test_reset(false, 0);
}

int main(void)
{
    // settings_init/load run before mutex_manager_init during process startup.
    assert(!fresh_tonies_ready);
    mqtt_server_freshness_forget_overlay(1);
    mqtt_server_freshness_begin_reload();
    mqtt_server_freshness_reconcile_overlays();
    assert(!fresh_tonies_reload_pending && !lock_held);
    mqtt_server_freshness_init();
    test_cached_diagnostic_owner_has_no_relay_dependency();
    test_hass_diagnostic_throttle_has_no_relay_dependency();
    test_schedule_and_delayed_ack();
    test_counter_saturates_and_reset();
    test_clock_failure_and_wrap();
    test_sync_and_targeted_changes_keep_inflight();
    test_unsent_publish_does_not_advance_delivery();
    test_deferred_completion_retains_uid_and_retry_boundary();
    test_http_notification_and_reconnect();
    test_http_abort_and_concurrent_source_change();
    test_old_inflight_ack_does_not_confirm_new_generation();
    test_concurrent_http_discovery_and_completion();
    test_snapshot_identity_and_allocation_failures();
    test_http_keeps_admitted_qos_and_boxes_independent();
    mqtt_debug_test_reset(true, 5000);
    connections[0].debug_fresh_first_ms = 1000;
    connections[0].debug_fresh_last_ms = 2000;
    mqtt_debug_fresh_event(&connections[0], "freshness_puback", UINT64_C(0x0102030405060708), 42, FALSE);
    const cJSON *diagnostic = mqtt_debug_test_last("freshness_puback");
    assert(diagnostic != NULL);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(diagnostic, "ruid")), "0807060504030201"));
    assert(cJSON_GetObjectItemCaseSensitive(diagnostic, "since_first_write_ms")->valuedouble == 4000);
    assert(cJSON_GetObjectItemCaseSensitive(diagnostic, "since_last_write_ms")->valuedouble == 3000);
    mqtt_debug_test_reset(false, 0);
    mqtt_fresh_tonies_reset_connection(&connections[0]);
    puts("MQTT fresh delivery PASS: unchanged retries/QoS, HTTP claims and dedupe, reconnect/restart, concurrent generations/discoveries, overlay identity/isolation and reload/allocation failures");
    return 0;
}
