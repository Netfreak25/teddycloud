/* Production callbacks are inserted verbatim; only surrounding I/O is stubbed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/net.h"
#include "core/socket.h"
#include "tls.h"
#include "mqtt_settings.h"
#include "mqtt_app_control.h"
#include "tb2_mqtt_passthrough.h"
#include "mutex_manager.h"
#include "handler.h"
#undef TRACE_INFO
#undef TRACE_WARNING
#undef TRACE_ERROR
#define TRACE_INFO(...) do { if (false) printf(__VA_ARGS__); } while (0)
#define TRACE_WARNING(...) do { if (false) printf(__VA_ARGS__); } while (0)
#define TRACE_ERROR(...) do { if (false) printf(__VA_ARGS__); } while (0)
/* SERVER_TYPES */
static bool locked, connected = true, response_ok = true;
static systime_t now;
static unsigned responses;
static unsigned closes, locked_closes;
static bool write_fails, box_write_failed;
void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *pointer) { free(pointer); }
char *error2text(error_t error) { (void)error; return "test"; }
static int mqtt_connection_slot(MqttClientConnection *conn) { (void)conn; return 0; }
static uint8_t mqtt_connection_overlay_id(MqttClientConnection *conn) { (void)conn; return 1; }
static void mqtt_connection_close_locked(MqttClientConnection *conn, const char *reason)
{ assert(conn->passthrough == NULL && reason); locked_closes++; conn->active = false; }
static void mqtt_connection_close(MqttClientConnection *conn, const char *reason)
{ assert(conn->passthrough == NULL && reason); closes++; conn->active = false; }
static void mqtt_trace_full_publish(const char *direction, const char *topic,
                                   const uint8_t *payload, size_t size, uint8_t qos)
{ (void)direction; (void)topic; (void)payload; (void)size; (void)qos; }
error_t tb2_mqtt_passthrough_reserve_local_packet_id(tb2_mqtt_passthrough_session_t *session,
                                                   uint16_t *packet_id)
{ assert(session); *packet_id = 42; return NO_ERROR; }
void tb2_mqtt_passthrough_release_local_packet_id(tb2_mqtt_passthrough_session_t *session,
                                                 uint16_t packet_id)
{ assert(session && packet_id); }
error_t tb2_mqtt_passthrough_write_local_publish(tb2_mqtt_passthrough_session_t *session,
    const uint8_t *packet, size_t size, const char *topic, uint16_t packet_id, const char *action)
{
    assert(session && packet && size && topic && action); (void)packet_id;
    if (write_fails) { box_write_failed = true; return ERROR_WRITE_FAILED; }
    return NO_ERROR;
}
error_t socketSend(Socket *socket, const void *data, size_t size, size_t *written, uint_t flags)
{ (void)socket; (void)data; (void)flags; *written = write_fails ? 0 : size;
  return write_fails ? ERROR_WRITE_FAILED : NO_ERROR; }
error_t tlsWrite(TlsContext *context, const void *data, size_t size, size_t *written, uint_t flags)
{ (void)context; return socketSend(NULL, data, size, written, flags); }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_MQTT_APP_CONTROL && !locked); locked = true; }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_MQTT_APP_CONTROL && locked); locked = false; }
systime_t osGetSystemTime(void) { return now; }
bool_t tb2_mqtt_passthrough_is_upstream_connected(const tb2_mqtt_passthrough_session_t *session)
{ assert(session != NULL); return connected; }
/* UTF-8/filter grammar is covered by the broker test; all fixtures here use valid filters. */
bool_t tb2_mqtt_topic_filter_valid(const uint8_t *text, size_t length)
{ assert(length && memchr(text, 0, length) == NULL); return TRUE; }
static void mqtt_connection_update_context(MqttClientConnection *conn, const char *topic)
{ assert(locked && conn->box_connection && topic[0]); }
static const char *mqtt_connection_common_name(MqttClientConnection *conn)
{ (void)conn; return "AABBCCDDEEFF"; }
static bool_t mqtt_publish_settings_response(MqttClientConnection *conn, bool_t track, bool_t request)
{ assert(conn->active && track && request); responses++; return response_ok; }
/* SERVER_CALLBACKS */

static size_t append_topic(uint8_t *packet, size_t offset, const char *topic, int qos)
{
    size_t length = strlen(topic);
    packet[offset++] = length >> 8;
    packet[offset++] = length;
    memcpy(packet + offset, topic, length);
    offset += length;
    if (qos >= 0) packet[offset++] = qos;
    return offset;
}
int main(void)
{
    MqttClientConnection conn = {.active = true, .box_connection = TRUE, .established = TRUE};
    uint8_t packet[2048] = {0, 1}, codes[32];
    size_t count = 0, length = append_topic(packet, 2, "toniebox/AABBCCDDEEFF/#", 0);
    length = append_topic(packet, length, "other/+", 2);
    assert(!mqtt_passthrough_subscription_apply(&conn, FALSE, packet, length, codes, 32, &count));
    assert(count == 2 && codes[0] == 0 && codes[1] == 2 && conn.subscription_count == 2);
    MqttSubscription saved[32];
    memcpy(saved, conn.subscriptions, sizeof(saved));
    assert(mqtt_passthrough_subscription_apply(&conn, FALSE, packet, length - 1, codes, 32, &count));
    assert(!memcmp(saved, conn.subscriptions, sizeof(saved)) && conn.subscription_count == 2);
    assert(mqtt_passthrough_subscription_apply(&conn, FALSE, packet, length, codes, 1, &count));
    assert(!memcmp(saved, conn.subscriptions, sizeof(saved)));
    tb2_mqtt_subscription_t snapshot[32];
    assert(mqtt_passthrough_subscription_snapshot(&conn, snapshot, 1) == 0);
    assert(mqtt_passthrough_subscription_snapshot(&conn, snapshot, 32) == 2);
    assert(!strcmp(snapshot[1].topic, "other/+") && snapshot[1].qos == 2);
    length = append_topic(packet, 2, "other/+", -1);
    length = append_topic(packet, length, "not/subscribed", -1);
    assert(!mqtt_passthrough_subscription_apply(&conn, TRUE, packet, length, codes, 32, &count));
    assert(count == 2 && conn.subscription_count == 1);
    length = 2;
    for (size_t i = 1; i < 32; i++) {
        char topic[32]; snprintf(topic, sizeof(topic), "topic/%zu", i);
        length = append_topic(packet, length, topic, 1);
    }
    assert(!mqtt_passthrough_subscription_apply(&conn, FALSE, packet, length, codes, 32, &count));
    assert(conn.subscription_count == 32);
    length = append_topic(packet, 2, "full/table", 1);
    length = append_topic(packet, length, "topic/1", 2);
    assert(!mqtt_passthrough_subscription_apply(&conn, FALSE, packet, length, codes, 32, &count));
    assert(count == 2 && codes[0] == 0x80 && codes[1] == 2 && conn.subscription_count == 32);
    assert(!locked);

    MqttClientConnection routed = {.active = true, .box_connection = TRUE};
    length = append_topic(packet, 2, "toniebox/AABBCCDDEEFF/#", 0);
    length = append_topic(packet, length, "toniebox/AABBCCDDEEFF/+", 2);
    assert(!mqtt_passthrough_subscription_apply(&routed, FALSE, packet, length, codes, 32, &count));
    assert(mqtt_passthrough_subscription_qos(&routed, "toniebox/AABBCCDDEEFF/playback") == 2);
    assert(mqtt_passthrough_subscription_qos(&routed, "toniebox/AABBCCDDEEFF/settings/desired") == 0);
    assert(mqtt_passthrough_subscription_qos(&routed, "other/topic") == -1);
    length = append_topic(packet, 2, "toniebox/AABBCCDDEEFF/+", -1);
    assert(!mqtt_passthrough_subscription_apply(&routed, TRUE, packet, length, codes, 32, &count));
    assert(mqtt_passthrough_subscription_qos(&routed, "toniebox/AABBCCDDEEFF/playback") == 0);
    length = append_topic(packet, 2, "toniebox/AABBCCDDEEFF/#", -1);
    assert(!mqtt_passthrough_subscription_apply(&routed, TRUE, packet, length, codes, 32, &count));
    assert(mqtt_passthrough_subscription_qos(&routed, "toniebox/AABBCCDDEEFF/playback") == -1);
    assert(!locked); /* Delayed cloud traffic cannot revive a removed local subscription. */

    conn.passthrough = (void *)1;
    conn.settings_request_pending = TRUE;
    conn.settings_request_sent = TRUE;
    now = MQTT_SETTINGS_REQUEST_TIMEOUT_MS - 1;
    mqtt_settings_request_pump(&conn);
    assert(!responses && conn.settings_request_pending);
    now++;
    mqtt_settings_request_pump(&conn);
    assert(responses == 1 && !conn.settings_request_pending && !conn.settings_request_sent);
    mqtt_settings_request_pump(&conn);
    assert(responses == 1);
    connected = false; conn.settings_request_pending = TRUE; conn.settings_request_since = now;
    mqtt_settings_request_pump(&conn);
    assert(responses == 2 && !conn.settings_request_pending);
    connected = true; now += MQTT_SETTINGS_REQUEST_TIMEOUT_MS;
    mqtt_settings_request_pump(&conn);
    assert(responses == 2); /* Reconnect cannot replay a locally answered request. */
    connected = false; response_ok = false; conn.settings_request_pending = TRUE;
    mqtt_settings_request_pump(&conn); mqtt_settings_request_pump(&conn);
    assert(responses == 3 && !conn.settings_request_pending);
    uint16_t packet_id = 0;
    write_fails = true;
    for (bool_t app_locked = FALSE; app_locked <= TRUE; app_locked++) {
        assert(!mqtt_connection_publish_packet_internal(&conn, "test/control", "{}", 0,
            FALSE, &packet_id, "local_app_control", app_locked));
        assert(box_write_failed && conn.active && conn.passthrough == (void *)1);
        assert(!closes && !locked_closes); /* Main loop still owns this session. */
    }
    write_fails = false;
    assert(mqtt_connection_publish_packet_internal(&conn, "test/control", "{}", 0,
        FALSE, &packet_id, "local_app_control", TRUE));
    conn.passthrough = NULL; write_fails = true;
    assert(!mqtt_connection_publish_packet_internal(&conn, "test/control", "{}", 0,
        FALSE, &packet_id, "local_app_control", FALSE));
    assert(closes == 1 && !conn.active);
    conn.active = true;
    assert(!mqtt_connection_publish_packet_internal(&conn, "test/control", "{}", 0,
        FALSE, &packet_id, "local_app_control", TRUE));
    assert(locked_closes == 1 && !conn.active);
    puts("MQTT server callbacks PASS: subscriptions, bounded settings fallback and main-loop write-failure ownership");
    return 0;
}
