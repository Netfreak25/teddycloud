/* The runner inserts production structs/functions verbatim at the two markers. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <ctype.h>
#include "core/net.h"
#include "core/socket.h"
#include "tls.h"
#include "mqtt_server.h"
#include "mqtt_settings.h"
#include "mqtt_forward_filter.h"
#include "mqtt_app_control.h"
#include "tb2_mqtt_passthrough.h"
#include "toniebox_state.h"
#include "mutex_manager.h"
#include "debug.h"
#undef TRACE_INFO
#undef TRACE_WARNING
#define TRACE_INFO(...) do { if (false) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (false) printf(__VA_ARGS__); } while (0)
/* SERVER_TYPES */
static MqttClientConnection connections[MQTT_MAX_CONNECTIONS];
static settings_t boxes[MAX_OVERLAYS];
static bool upstream = true, master = true, subscribed = true, write_ok = true;
static bool global_values[5], box_values[5];
static setting_item_t global_options[5], box_options[5];
static const char *commands[] = {"playback", "volume", "ping", "stl", "sleep"};
static pthread_mutex_t app_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned writes;
static pthread_t reply_thread;
static atomic_bool reply_attempted, reply_finished;
static bool inject_reply, reply_matched;
static char reply_id[64];
static MqttClientConnection *reply_connection;

settings_t *get_settings_id(uint8_t id) { return &boxes[id]; }
bool settings_get_bool(const char *key)
{
    assert(strcmp(key, "mqtt_client_upstream.filters_enabled") == 0);
    return master;
}
setting_item_t *settings_get_by_name_ovl(const char *key, const char *overlay)
{
    for (size_t i = 0; i < 5; i++) {
        char expected[96];
        snprintf(expected, sizeof(expected), "mqtt_client_upstream.forward.app_control.%s", commands[i]);
        if (strcmp(key, expected) == 0) return overlay ? &box_options[i] : &global_options[i];
    }
    assert(false); return NULL;
}
bool_t settings_canonicalize_box_id(const char *value, char *out, size_t capacity)
{
    if (!value || strlen(value) != 12 || capacity < 13) return false;
    for (size_t i = 0; i < 12; i++) out[i] = toupper((unsigned char)value[i]);
    out[12] = 0; return true;
}
bool_t tb2_mqtt_passthrough_is_enabled(void) { return upstream; }
systime_t osGetSystemTime(void) { return 1000; }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_MQTT_APP_CONTROL); assert(!pthread_mutex_lock(&app_mutex)); }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_MQTT_APP_CONTROL); assert(!pthread_mutex_unlock(&app_mutex)); }
static bool_t mqtt_connection_has_sub(MqttClientConnection *conn, const char *topic)
{ (void)conn; assert(strstr(topic, "/app-control/")); return subscribed; }
static void *reply_during_write(void *unused)
{
    (void)unused;
    assert(pthread_mutex_trylock(&app_mutex) == EBUSY);
    atomic_store(&reply_attempted, true);
    mutex_lock(MUTEX_MQTT_APP_CONTROL);
    reply_matched = mqtt_app_control_match_pong(&reply_connection->app_control, reply_id, 1001, NULL);
    atomic_store(&reply_finished, true);
    mutex_unlock(MUTEX_MQTT_APP_CONTROL);
    return NULL;
}
static bool_t mqtt_connection_publish_packet_internal(MqttClientConnection *conn,
    const char *topic, const char *payload, uint8_t qos, bool_t duplicate,
    uint16_t *packet_id, const char *capture_action, bool_t app_control_locked)
{
    assert(app_control_locked && qos == 0 && !duplicate && packet_id);
    assert(strcmp(capture_action, "local_app_control") == 0);
    assert(strstr(topic, "/app-control/") && pthread_mutex_trylock(&app_mutex) == EBUSY);
    writes++;
    if (inject_reply) {
        cJSON *json = cJSON_Parse(payload);
        snprintf(reply_id, sizeof(reply_id), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(json, "requestId")));
        cJSON_Delete(json);
        reply_connection = conn;
        assert(!pthread_create(&reply_thread, NULL, reply_during_write, NULL));
        while (!atomic_load(&reply_attempted)) sched_yield();
        assert(!atomic_load(&reply_finished));
    }
    return write_ok;
}
/* SERVER_FUNCTIONS */

static void connect_box(size_t slot, uint8_t overlay)
{
    MqttClientConnection *conn = &connections[slot];
    memset(conn, 0, sizeof(*conn));
    conn->active = true;
    conn->box_connection = true;
    conn->client_ctx.settings = &boxes[overlay];
    conn->box_overlay_id = overlay;
    strcpy(conn->box_common_name, boxes[overlay].commonName);
    conn->passthrough = (void *)1;
}
int main(void)
{
    boxes[1].commonName = "AABBCCDDEEFF";
    boxes[1].internal.config_used = true;
    boxes[1].internal.overlayNumber = 1;
    boxes[1].internal.overlayUniqueId = "AABBCCDDEEFF";
    boxes[1].toniebox.boxGeneration = GENERATION_TB2;
    for (size_t i = 0; i < 5; i++) {
        global_values[i] = true;
        global_options[i] = (setting_item_t){.type = TYPE_BOOL, .ptr = &global_values[i]};
        box_options[i] = (setting_item_t){.type = TYPE_BOOL, .ptr = &box_values[i]};
    }
    connect_box(0, 1);
    for (size_t i = 0; i < 5; i++) {
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_CLOUD_CONTROLLED);
        global_values[i] = false;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_ALLOWED);
        box_options[i].overlayed = true; box_values[i] = true;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_CLOUD_CONTROLLED);
        box_options[i].overlayed = false;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_ALLOWED);
        master = false;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_CLOUD_CONTROLLED);
        boxes[1].mqtt_client_upstream.local_control_enabled = true;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_ALLOWED);
        boxes[1].mqtt_client_upstream.local_control_enabled = false; master = true;
        global_values[i] = true;
        upstream = false;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_ALLOWED);
        upstream = true; connections[0].passthrough = NULL;
        assert(mqtt_server_control_availability(1, commands[i]) == MQTT_CONTROL_ALLOWED);
        connections[0].passthrough = (void *)1;
    }
    assert(!mqtt_server_publish_app_control_for_overlay(1, "volume", "{\"level\":7}"));
    assert(!writes);
    subscribed = false;
    assert(mqtt_server_control_availability(1, "ping") == MQTT_CONTROL_NOT_SUBSCRIBED);
    subscribed = true; connections[0].active = false;
    assert(mqtt_server_control_availability(1, "ping") == MQTT_CONTROL_OFFLINE);
    assert(strcmp(mqtt_server_control_reason(MQTT_CONTROL_OFFLINE), "offline") == 0);
    connect_box(0, 1); global_values[2] = global_values[3] = false;
    char first[64], second[64];
    write_ok = false;
    assert(!mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)));
    assert(!first[0] && !connections[0].app_control.ping.pending);
    assert(!mqtt_server_publish_app_control_for_overlay(1, "stl", "{\"state\":\"on\",\"duration\":600}"));
    assert(!connections[0].app_control.stl.pending);
    write_ok = true;
    assert(mqtt_server_publish_ping_for_overlay(1, first, sizeof(first)));
    assert(connections[0].app_control.ping.pending && !connections[1].app_control.ping.pending);
    mqtt_app_control_reset(&connections[0].app_control);
    connect_box(0, 1); /* Connection reset must not rewind the process ID sequence. */
    assert(!mqtt_app_control_match_pong(&connections[0].app_control, first, 1000, NULL));
    inject_reply = true;
    assert(mqtt_server_publish_ping_for_overlay(1, second, sizeof(second)));
    assert(!pthread_join(reply_thread, NULL));
    assert(strcmp(first, second) && reply_matched && atomic_load(&reply_finished));
    inject_reply = false;
    assert(mqtt_server_publish_app_control_for_overlay(1, "stl", "{\"state\":\"on\",\"duration\":600}"));
    assert(connections[0].app_control.stl.pending && connections[0].app_control.stl.matchable);
    puts("MQTT runtime PASS: policy matrix, send outcomes, connection ledger and reply/write serialization");
    return 0;
}
