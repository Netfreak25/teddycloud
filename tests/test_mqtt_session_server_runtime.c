/* Real server decisions are inserted by the native-test runner. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "core/net.h"
#include "core/socket.h"
#include "tls.h"
#include "mqtt_server.h"
#include "tb2_mqtt_passthrough.h"
#include "mqtt_forward_filter.h"
#include "mutex_manager.h"
#include "debug.h"
#undef TRACE_INFO
#undef TRACE_DEBUG
#define TRACE_INFO(...) do { if (false) printf(__VA_ARGS__); } while (0)
#define TRACE_DEBUG(...) do { if (false) printf(__VA_ARGS__); } while (0)
/* SERVER_TYPES */
static settings_t box;
static MqttClientConnection connection;
static bool enabled = true, connected, blocked, accept_write = true, clock_valid = true;
static uint32_t now;
static unsigned writes, updates, lock_depth;
static uint64_t current_revision;
static const char request[] = "toniebox/AABBCCDDEEFF/settings/request";
static const char desired[] = "toniebox/AABBCCDDEEFF/settings/desired";

bool_t tb2_mqtt_passthrough_is_enabled(void) { return enabled; }
bool_t tb2_mqtt_passthrough_is_upstream_connected(const tb2_mqtt_passthrough_session_t *session)
{ assert(session); return connected; }
static bool_t mqtt_monotonic_ms(uint32_t *value) { *value = now; return clock_valid; }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_MQTT_SESSION && lock_depth++ == 0); }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_MQTT_SESSION && lock_depth-- == 1); }
static const char *mqtt_connection_common_name(MqttClientConnection *conn) { return conn->box_common_name; }
static void mqtt_connection_update_context(MqttClientConnection *conn, const char *topic)
{ assert(conn == &connection && topic); updates++; }
static bool_t mqtt_toniebox2_settings_desired_topic(MqttClientConnection *conn, char *topic, size_t capacity)
{ assert(conn == &connection && capacity > sizeof(desired)); strcpy(topic, desired); return true; }
static bool_t mqtt_connection_local_control_allowed(MqttClientConnection *conn);
static bool_t mqtt_publish_settings_desired_to_connection(MqttClientConnection *conn, bool_t track)
{
    assert(conn == &connection); assert(track == box.internal.toniebox2SettingsDesiredPending);
    if (!mqtt_connection_local_control_allowed(conn)) return false;
    writes++; return accept_write;
}
static void mqtt_copy_u64_settings_array(settings_t *settings, const char *key,
    uint64_t target[MQTT_TB2_SETTING_COUNT])
{
    assert(settings == &box && key && lock_depth == 1);
    memset(target, 0, sizeof(uint64_t) * MQTT_TB2_SETTING_COUNT);
    target[0] = current_revision;
}
bool settings_set_unsigned_id(const char *key, uint32_t value, uint8_t overlay)
{
    assert(overlay == 1 && lock_depth == 1);
    if (!strcmp(key, "internal.toniebox2SettingsDesiredAttempts"))
        box.internal.toniebox2SettingsDesiredAttempts = value;
    else if (!strcmp(key, "internal.toniebox2SettingsDesiredLastAttempt"))
        box.internal.toniebox2SettingsDesiredLastAttempt = value;
    else assert(false);
    return true;
}
void osFreeMem(void *p) { free(p); }
bool_t mqtt_forward_filter_should_block(settings_t *settings, const char *topic,
    const uint8_t *payload, size_t length, const char **filter_id)
{ assert(settings == &box && topic && payload && length && filter_id); return blocked; }
/* SERVER_FUNCTIONS */

static void reset(void)
{
    memset(&box, 0, sizeof(box));
    memset(&connection, 0, sizeof(connection));
    box.internal.overlayNumber = 1;
    connection.active = true;
    connection.established = connection.box_connection = true;
    connection.box_overlay_id = 1;
    connection.client_ctx.settings = &box;
    connection.passthrough = (void *)1;
    strcpy(connection.box_common_name, "AABBCCDDEEFF");
    strcpy(connection.subscriptions[0].topic, desired);
    connection.subscriptions[0].qos = 1;
    connection.subscription_count = 1;
    enabled = clock_valid = accept_write = true;
    connected = blocked = false;
    now = 1000;
    writes = updates = 0;
    current_revision = 0;
    assert(!lock_depth);
}

static tb2_mqtt_observer_result_t request_settings(void)
{
    tb2_mqtt_observer_result_t result = {0};
    bool_t handled = mqtt_passthrough_settings_request(&connection, request,
        (const uint8_t *)"{}", 2, &result);
    assert(handled == mqtt_connection_local_control_allowed(&connection));
    return result;
}

static size_t subscription_packet(uint8_t *packet, const char *topic, uint8_t qos, bool unsubscribe)
{
    size_t length = strlen(topic);
    packet[0] = 0; packet[1] = 1;
    packet[2] = length >> 8; packet[3] = length;
    memcpy(packet + 4, topic, length);
    if (!unsubscribe) packet[4 + length] = qos;
    return 4 + length + !unsubscribe;
}

static void test_permissions_and_requests(void)
{
    reset();
    assert(!mqtt_connection_local_control_allowed(&connection));
    request_settings(); mqtt_settings_request_pump(&connection);
    assert(!writes && !connection.settings_request_pending);
    connected = true;
    assert(!mqtt_connection_local_control_allowed(&connection));
    box.mqtt_client_upstream.local_control_enabled = true;
    assert(mqtt_connection_local_control_allowed(&connection));
    box.mqtt_client_upstream.local_control_enabled = false;
    enabled = false;
    assert(mqtt_connection_local_control_allowed(&connection));
    connected = false;
    tb2_mqtt_observer_result_t result = request_settings();
    assert(result.action == TB2_MQTT_OBSERVER_CONSUME && result.locally_processed);
    mqtt_settings_request_pump(&connection); mqtt_settings_request_pump(&connection);
    assert(writes == 1 && !connection.settings_request_pending);
    enabled = true;
    connection.passthrough = NULL;
    assert(mqtt_connection_local_control_allowed(&connection));

    reset(); box.mqtt_client_upstream.local_control_enabled = true;
    connection.subscription_count = 0;
    request_settings(); mqtt_settings_request_pump(&connection);
    assert(!writes && !connection.settings_request_pending);
    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    result = request_settings();
    assert(result.action == TB2_MQTT_OBSERVER_FORWARD);
    mqtt_settings_request_pump(&connection);
    assert(!writes); /* Queued is not sent and must not trigger an immediate fallback. */
    now += 9999;
    request_settings(); /* A second request must not extend the existing deadline. */
    mqtt_settings_request_pump(&connection); assert(!writes);
    mqtt_passthrough_publish_completed(&connection, true, request, NULL, 0);
    assert(connection.settings_request_sent);
    now++;
    mqtt_settings_request_pump(&connection); mqtt_settings_request_pump(&connection);
    assert(writes == 1 && !connection.settings_request_pending);

    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    request_settings();
    mqtt_passthrough_publish_completed(&connection, true, request, NULL, 0);
    connected = false;
    mqtt_settings_request_pump(&connection);
    assert(writes == 1 && !connection.settings_request_pending);
    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    request_settings();
    mqtt_passthrough_publish_completed(&connection, false, desired, NULL, 0);
    connected = false; mqtt_settings_request_pump(&connection);
    assert(!writes && !connection.settings_request_pending);

    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true; blocked = true;
    result = request_settings(); assert(result.action == TB2_MQTT_OBSERVER_CONSUME);
    accept_write = false; mqtt_settings_request_pump(&connection);
    assert(connection.settings_request_pending);
    accept_write = true; mqtt_settings_request_pump(&connection);
    assert(!connection.settings_request_pending && writes == 2);
    reset(); box.mqtt_client_upstream.local_control_enabled = true;
    request_settings(); box.mqtt_client_upstream.local_control_enabled = false;
    mqtt_settings_request_pump(&connection); assert(!writes && !connection.settings_request_pending);

    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    now = UINT32_MAX - 5000; request_settings(); now += 10000;
    mqtt_settings_request_pump(&connection); assert(writes == 1);
    reset(); box.mqtt_client_upstream.local_control_enabled = true; clock_valid = false;
    request_settings(); assert(!connection.settings_request_pending);
    tb2_mqtt_observer_result_t foreign = {0};
    assert(!mqtt_passthrough_settings_request(&connection,
        "toniebox/001122334455/settings/request", (const uint8_t *)"{}", 2, &foreign));
    assert(!writes);
}

static MqttSettingsDelivery *local_settings_delivery(void)
{
    MqttSettingsDelivery *delivery = calloc(1, sizeof(*delivery));
    assert(delivery);
    delivery->conn = &connection;
    delivery->session = connection.passthrough;
    delivery->revisions[0] = current_revision;
    delivery->track_attempt = true;
    return delivery;
}

static void test_local_completion_satisfies_request(void)
{
    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    box.internal.toniebox2SettingsDesiredPending = true;
    request_settings();
    mqtt_settings_request_pump(&connection);
    assert(!writes && connection.settings_request_pending);
    /* Existing explicitly pending local changes remain allowed while Cloud waits. */
    assert(mqtt_publish_pending_settings_desired_to_connection(&connection, true, true));
    assert(writes == 1 && connection.settings_request_pending);
    connection.settings_delivery_queued = true;
    mqtt_local_settings_completed(local_settings_delivery(), NO_ERROR);
    assert(!connection.settings_request_pending && !connection.settings_delivery_queued);
    assert(box.internal.toniebox2SettingsDesiredAttempts == 1);
    assert(box.internal.toniebox2SettingsDesiredLastAttempt != 0);
    now += MQTT_SETTINGS_REQUEST_TIMEOUT_MS;
    mqtt_settings_request_pump(&connection);
    assert(writes == 1); /* No second fallback after the already-written answer. */

    reset(); box.mqtt_client_upstream.local_control_enabled = true; connected = true;
    request_settings();
    MqttSettingsDelivery *delivery = local_settings_delivery();
    current_revision++; /* An obsolete local payload cannot satisfy the new values. */
    mqtt_local_settings_completed(delivery, NO_ERROR);
    assert(connection.settings_request_pending);
    delivery = local_settings_delivery();
    delivery->session = (void *)2;
    mqtt_local_settings_completed(delivery, NO_ERROR);
    assert(connection.settings_request_pending);
    delivery = local_settings_delivery();
    mutex_lock(MUTEX_MQTT_SESSION); /* Cancellation already holds the lifetime lock. */
    mqtt_local_settings_completed(delivery, ERROR_NOT_CONNECTED);
    mutex_unlock(MUTEX_MQTT_SESSION);
    assert(connection.settings_request_pending);
    mqtt_local_settings_completed(local_settings_delivery(), NO_ERROR);
    assert(!connection.settings_request_pending && !lock_depth);
}

static void test_subscriptions(void)
{
    reset();
    uint8_t packet[512], codes[4]; size_t count = 0;
    size_t length = subscription_packet(packet, "toniebox/AABBCCDDEEFF/#", 2, false);
    assert(!mqtt_passthrough_subscription_apply(&connection, false, packet, length, codes, 4, &count));
    assert(count == 1 && codes[0] == 2 && connection.subscription_count == 2);
    assert(mqtt_passthrough_subscription_qos(&connection, desired) == 2);
    tb2_mqtt_subscription_t snapshot[MQTT_MAX_SUBSCRIPTIONS];
    assert(mqtt_passthrough_subscription_snapshot(&connection, snapshot, MQTT_MAX_SUBSCRIPTIONS) == 2);
    assert(snapshot[1].qos == 2 && !strcmp(snapshot[0].topic, desired));
    assert(!mqtt_passthrough_subscription_snapshot(&connection, snapshot, 1));
    /* Invalid suffix must not partially publish a changed subscription table. */
    MqttSubscription before[MQTT_MAX_SUBSCRIPTIONS]; memcpy(before, connection.subscriptions, sizeof(before));
    packet[length++] = 0;
    assert(mqtt_passthrough_subscription_apply(&connection, false, packet, length, codes, 4, &count));
    assert(!memcmp(before, connection.subscriptions, sizeof(before)));
    length = subscription_packet(packet, "toniebox/#/bad", 0, false);
    assert(mqtt_passthrough_subscription_apply(&connection, false, packet, length, codes, 4, &count));
    length = subscription_packet(packet, "toniebox/AABBCCDDEEFF/#", 0, true);
    assert(!mqtt_passthrough_subscription_apply(&connection, true, packet, length, codes, 4, &count));
    assert(connection.subscription_count == 1 && mqtt_passthrough_subscription_qos(&connection, desired) == 1);
    for (size_t i = 1; i < MQTT_MAX_SUBSCRIPTIONS; i++)
        snprintf(connection.subscriptions[i].topic, sizeof(connection.subscriptions[i].topic), "topic/%zu", i);
    connection.subscription_count = MQTT_MAX_SUBSCRIPTIONS;
    length = subscription_packet(packet, "new/topic", 1, false);
    assert(!mqtt_passthrough_subscription_apply(&connection, false, packet, length, codes, 4, &count));
    assert(count == 1 && codes[0] == 0x80 && connection.subscription_count == MQTT_MAX_SUBSCRIPTIONS);
    connection.active = false;
    assert(mqtt_passthrough_subscription_qos(&connection, desired) == -1);
    assert(!lock_depth);
}

int main(void)
{
    test_permissions_and_requests();
    test_local_completion_satisfies_request();
    test_subscriptions();
    puts("MQTT server session callbacks: permissions, fallback deadlines and subscriptions passed");
    return 0;
}
