/* Production delivery functions are inserted by the existing native-test runner. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <ctype.h>
#include "core/net.h"
#include "core/socket.h"
#include "tls.h"
#include "mqtt_server.h"
#include "tb2_mqtt_passthrough.h"
#include "toniebox_state.h"
#include "mutex_manager.h"
#include "cJSON.h"
#include "debug.h"
#undef TRACE_INFO
#undef TRACE_WARNING
#define TRACE_INFO(...) do { if (false) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (false) printf(__VA_ARGS__); } while (0)
/* SERVER_TYPES */
static MqttClientConnection connections[MQTT_MAX_CONNECTIONS];
static MqttAppControlStlState app_control_stl_state[MAX_OVERLAYS];
static MqttAppControlPingState app_control_ping_state[MAX_OVERLAYS];
static settings_t boxes[MAX_OVERLAYS];
static bool subscribed = true, defer_publish = true, publish_busy;
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned writes, allocations, volume_commands, playback_commands;
static unsigned fail_at_write;
static uint32_t volume_revision = 7, volume_level;
static tb2_mqtt_local_write_completed_t deferred_completed[2];
static void *deferred_context[2];
static size_t deferred_count;
static uint64_t settings_revisions[MQTT_TB2_SETTING_COUNT];
static unsigned settings_writes;
static bool relay_established = true, monotonic_valid = true;
static uint32_t monotonic_now;

bool_t tb2_mqtt_passthrough_is_established(const tb2_mqtt_passthrough_session_t *session)
{ assert(session); return relay_established; }
static bool_t mqtt_monotonic_ms(uint32_t *value)
{ *value = monotonic_now; return monotonic_valid; }

static void mqtt_copy_u64_settings_array(settings_t *settings, const char *key, uint64_t *out)
{ assert(settings == &boxes[1] && strstr(key, "Revisions")); memcpy(out, settings_revisions, sizeof(settings_revisions)); }
bool settings_set_unsigned_id(const char *key, uint32_t value, uint8_t overlay)
{
    assert(overlay == 1); settings_writes++;
    if (strstr(key, "Attempts")) boxes[1].internal.toniebox2SettingsDesiredAttempts = value;
    else assert(strstr(key, "LastAttempt"));
    return true;
}

void *osAllocMem(size_t size) { void *p = malloc(size); if (p) allocations++; return p; }
void osFreeMem(void *p) { if (p) { assert(allocations); allocations--; } free(p); }
void tbs_toniebox2_volume_snapshot(uint8_t overlay, toniebox_state_volume_t *volume)
{ assert(overlay == 1); memset(volume, 0, sizeof(*volume)); volume->revision = volume_revision; }
void tbs_toniebox2_volume_command(uint8_t overlay, uint32_t level, uint32_t revision)
{ assert(overlay == 1 && revision == volume_revision); volume_revision++; volume_level = level; volume_commands++; }
void tbs_toniebox2_playback_command_state(client_ctx_t *ctx, toniebox_state_tb2_playback_status_t status)
{ assert(ctx && status == TBS_TB2_PLAYBACK_STATUS_PAUSED); playback_commands++; }
static const char *mqtt_connection_common_name(MqttClientConnection *conn) { return conn->box_common_name; }
settings_t *get_settings_id(uint8_t id) { return &boxes[id]; }
bool_t settings_canonicalize_box_id(const char *value, char *out, size_t capacity)
{
    if (!value || strlen(value) != 12 || capacity < 13) return false;
    for (size_t i = 0; i < 12; i++) out[i] = toupper((unsigned char)value[i]);
    out[12] = 0; return true;
}
systime_t osGetSystemTime(void) { return 1000; }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_MQTT_SESSION); assert(!pthread_mutex_lock(&session_mutex)); }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_MQTT_SESSION); assert(!pthread_mutex_unlock(&session_mutex)); }
static bool_t mqtt_connection_has_sub(MqttClientConnection *conn, const char *topic)
{ (void)conn; assert(strstr(topic, "/app-control/")); return subscribed; }
static mqtt_delivery_result_t mqtt_connection_publish_packet_locked(MqttClientConnection *conn,
    const char *topic, const char *payload, uint8_t qos, bool_t duplicate,
    uint16_t *packet_id, const char *capture_action,
    tb2_mqtt_local_write_completed_t completed, void *context, size_t context_bytes)
{
    assert(conn && payload && qos == 0 && !duplicate && packet_id);
    assert(strcmp(capture_action, "local_app_control") == 0);
    assert(strstr(topic, "/app-control/") && pthread_mutex_trylock(&session_mutex) == EBUSY);
    assert(context_bytes >= sizeof(MqttControlDelivery));
    if (publish_busy) return mqtt_delivery_result(MQTT_DELIVERY_BUSY, ERROR_WOULD_BLOCK);
    writes++;
    if (fail_at_write == writes) return mqtt_delivery_result(MQTT_DELIVERY_FAILED, ERROR_WRITE_FAILED);
    if (defer_publish) {
        assert(deferred_count < 2);
        deferred_completed[deferred_count] = completed;
        deferred_context[deferred_count++] = context;
        return mqtt_delivery_result(MQTT_DELIVERY_QUEUED, NO_ERROR);
    }
    return mqtt_delivery_result(MQTT_DELIVERY_SENT, NO_ERROR);
}
mqtt_delivery_result_t tb2_mqtt_passthrough_submit_local_batch(
    tb2_mqtt_passthrough_session_t *session, const tb2_mqtt_local_publish_t *batch, size_t count)
{
    assert(session && count == 2 && deferred_count == 0);
    assert(pthread_mutex_trylock(&session_mutex) == EBUSY);
    if (publish_busy) return mqtt_delivery_result(MQTT_DELIVERY_BUSY, ERROR_WOULD_BLOCK);
    assert(strstr(batch[0].topic, "/stl") && strstr(batch[1].topic, "/sleep"));
    for (size_t i = 0; i < count; i++) {
        assert(batch[i].packet && batch[i].packet_size && batch[i].context_bytes);
        deferred_completed[deferred_count] = batch[i].completed;
        deferred_context[deferred_count++] = batch[i].context;
    }
    return mqtt_delivery_result(MQTT_DELIVERY_QUEUED, NO_ERROR);
}
/* SERVER_FUNCTIONS */
static void test_poll_and_maintenance_cadence(void)
{
    assert(mqtt_server_poll_interval() == 250);
    MqttClientConnection *conn = &connections[MQTT_MAX_CONNECTIONS - 1];
    conn->active = true; conn->passthrough = (void *)1;
    assert(mqtt_server_poll_interval() == 250);
    conn->established = true;
    assert(mqtt_server_poll_interval() == 10);
    relay_established = false;
    assert(mqtt_server_poll_interval() == 250);
    relay_established = true;
    conn->active = false;
    assert(mqtt_server_poll_interval() == 250);
    uint32_t last = 0;
    bool_t valid = FALSE;
    monotonic_now = 1000;
    assert(server_maintenance_due(&last, &valid));
    for (monotonic_now = 1010; monotonic_now < 1250; monotonic_now += 10)
        assert(!server_maintenance_due(&last, &valid));
    assert(server_maintenance_due(&last, &valid));
    last = UINT32_MAX - 100; monotonic_now = 148;
    assert(!server_maintenance_due(&last, &valid));
    monotonic_now++;
    assert(server_maintenance_due(&last, &valid));
    monotonic_valid = false;
    assert(server_maintenance_due(&last, &valid) && !valid);
    monotonic_valid = true;
    memset(connections, 0, sizeof(connections));
}

static void complete_all(error_t error)
{
    size_t count = deferred_count;
    deferred_count = 0;
    for (size_t i = 0; i < count; i++) deferred_completed[i](deferred_context[i], error);
}
int main(void)
{
    test_poll_and_maintenance_cadence();
    boxes[1].commonName = "AABBCCDDEEFF";
    boxes[1].internal.config_used = true;
    boxes[1].internal.overlayNumber = 1;
    boxes[1].toniebox.boxGeneration = GENERATION_TB2;
    MqttClientConnection *conn = &connections[0];
    conn->active = true; conn->box_connection = true; conn->box_overlay_id = 1;
    conn->client_ctx.settings = &boxes[1]; conn->passthrough = (void *)1;
    strcpy(conn->box_common_name, boxes[1].commonName);
    char first[64], second[64];
    assert(mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)).status == MQTT_DELIVERY_FAILED);
    assert(!writes && !allocations); /* Existing upstream local-control permission unchanged. */
    boxes[1].mqtt_client_upstream.local_control_enabled = true;
    subscribed = false;
    assert(mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)).status == MQTT_DELIVERY_FAILED);
    assert(!writes); subscribed = true;
    assert(mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)).status == MQTT_DELIVERY_QUEUED);
    assert(first[0] && !app_control_ping_state[1].valid);
    complete_all(NO_ERROR);
    assert(app_control_ping_state[1].valid && !strcmp(app_control_ping_state[1].request_id, first));
    app_control_ping_state[1].valid = false;
    publish_busy = true;
    assert(mqtt_server_publish_ping_for_overlay(1, second, sizeof(second)).status == MQTT_DELIVERY_BUSY);
    assert(!second[0] && !app_control_ping_state[1].valid && !allocations);
    publish_busy = false;
    assert(mqtt_server_publish_ping_for_overlay(1, second, sizeof(second)).status == MQTT_DELIVERY_QUEUED);
    assert(strcmp(first, second));
    mutex_lock(MUTEX_MQTT_SESSION); complete_all(ERROR_ABORTED); mutex_unlock(MUTEX_MQTT_SESSION);
    assert(!app_control_ping_state[1].valid && !allocations);

    assert(mqtt_server_publish_app_control_for_overlay(1, "volume", "{\"level\":7}").status == MQTT_DELIVERY_QUEUED);
    assert(mqtt_server_publish_app_control_for_overlay(1, "volume", "{\"level\":9}").status == MQTT_DELIVERY_QUEUED);
    assert(!volume_commands); complete_all(NO_ERROR);
    assert(volume_commands == 2 && volume_level == 9 && volume_revision == 9);
    assert(mqtt_server_publish_app_control_for_overlay(1, "playback", "{\"action\":\"pause\"}").status == MQTT_DELIVERY_QUEUED);
    assert(!playback_commands); complete_all(NO_ERROR); assert(playback_commands == 1);
    bool_t bedtime_sent = TRUE;
    const char *bedtime = "{\"state\":\"on\",\"duration\":600}";
    publish_busy = true;
    assert(mqtt_server_publish_shutdown_for_overlay(1, bedtime, &bedtime_sent).status == MQTT_DELIVERY_BUSY);
    assert(!bedtime_sent && !deferred_count && !allocations);
    publish_busy = false;
    assert(mqtt_server_publish_shutdown_for_overlay(1, bedtime, &bedtime_sent).status == MQTT_DELIVERY_QUEUED);
    assert(!bedtime_sent && deferred_count == 2 && !app_control_stl_state[1].valid);
    complete_all(NO_ERROR); assert(app_control_stl_state[1].valid && !allocations);

    /* Completion for an old owner cannot update a replacement connection. */
    app_control_ping_state[1].valid = false;
    assert(mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)).status == MQTT_DELIVERY_QUEUED);
    conn->passthrough = (void *)2; complete_all(NO_ERROR);
    assert(!app_control_ping_state[1].valid && !allocations);
    defer_publish = false; conn->passthrough = NULL;
    fail_at_write = writes + 2;
    assert(mqtt_server_publish_shutdown_for_overlay(1, bedtime, &bedtime_sent).status == MQTT_DELIVERY_FAILED);
    assert(bedtime_sent && writes == fail_at_write && !allocations);
    /* An old in-flight settings packet must not spend a newer revision's retries. */
    conn->passthrough = (void *)1;
    boxes[1].internal.toniebox2SettingsDesiredPending = true;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++) settings_revisions[i] = 2;
    MqttSettingsDelivery *settings_delivery = osAllocMem(sizeof(*settings_delivery));
    memset(settings_delivery, 0, sizeof(*settings_delivery));
    settings_delivery->conn = conn; settings_delivery->session = conn->passthrough;
    settings_delivery->track_attempt = true; conn->settings_delivery_queued = true;
    mqtt_local_settings_completed(settings_delivery, NO_ERROR);
    assert(!settings_writes && !conn->settings_delivery_queued && !allocations);
    settings_delivery = osAllocMem(sizeof(*settings_delivery));
    memset(settings_delivery, 0, sizeof(*settings_delivery));
    settings_delivery->conn = conn; settings_delivery->session = conn->passthrough;
    settings_delivery->track_attempt = true;
    memcpy(settings_delivery->revisions, settings_revisions, sizeof(settings_revisions));
    mqtt_local_settings_completed(settings_delivery, NO_ERROR);
    assert(settings_writes == 2 && boxes[1].internal.toniebox2SettingsDesiredAttempts == 1 && !allocations);
    puts("MQTT runtime PASS: existing permissions, deferred completions, BUSY/cancel, atomic shutdown and lifetime isolation");
    return 0;
}
