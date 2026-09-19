/* Native, deterministic correlation checks. Real cJSON; no server or mocks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_app_control.h"

static void local_stl(mqtt_app_control_state_t *state, const char *text, uint32_t now)
{
    cJSON *json = cJSON_Parse(text);
    assert(json != NULL);
    mqtt_app_control_local_stl_sent(state, json, now);
    cJSON_Delete(json);
}

static void cloud(mqtt_app_control_state_t *state, const char *command,
                   const char *text, uint32_t now)
{
    cJSON *json = cJSON_Parse(text);
    assert(json != NULL);
    mqtt_app_control_cloud_delivered(state, command, json, now);
    cJSON_Delete(json);
}

static bool bedtime(mqtt_app_control_state_t *state, const char *text, uint32_t now)
{
    cJSON *json = cJSON_Parse(text);
    assert(json != NULL);
    bool matched = mqtt_app_control_match_bedtime(state, json, now);
    cJSON_Delete(json);
    return matched;
}

static void test_ping_matching_and_replacement(void)
{
    mqtt_app_control_state_t state = {0};
    uint32_t elapsed = 999;
    mqtt_app_control_local_ping_sent(&state, "tc-1", 100);
    assert(!mqtt_app_control_match_pong(&state, "foreign", 101, &elapsed));
    assert(elapsed == 999 && state.ping.pending);
    assert(mqtt_app_control_match_pong(&state, "tc-1", 130, &elapsed));
    assert(elapsed == 30);
    assert(!mqtt_app_control_match_pong(&state, "tc-1", 131, &elapsed));

    mqtt_app_control_local_ping_sent(&state, "tc-2", 140);
    mqtt_app_control_local_ping_sent(&state, "tc-3", 141);
    assert(!mqtt_app_control_match_pong(&state, "tc-2", 142, NULL));
    assert(mqtt_app_control_match_pong(&state, "tc-3", 143, NULL));

    mqtt_app_control_local_ping_sent(&state, "same", 150);
    mqtt_app_control_local_ping_sent(&state, "same", 151);
    assert(state.ping.ambiguous);
    assert(!mqtt_app_control_match_pong(&state, "same", 152, NULL));
    mqtt_app_control_local_ping_sent(&state, "new", 153);
    assert(mqtt_app_control_match_pong(&state, "new", 154, NULL));
}

static void test_cloud_ping_collision_and_connection_reset(void)
{
    mqtt_app_control_state_t state = {0}, other_connection = {0};
    mqtt_app_control_local_ping_sent(&state, "tc-4", 10);
    cloud(&state, "ping", "{\"requestId\":\"cloud-1\"}", 11);
    assert(!state.ping.ambiguous);
    cloud(&state, "ping", "{\"requestId\":\"tc-4\"}", 12);
    assert(state.ping.ambiguous);
    assert(!mqtt_app_control_match_pong(&state, "tc-4", 13, NULL));
    mqtt_app_control_local_ping_sent(&other_connection, "tc-5", 12);
    assert(mqtt_app_control_match_pong(&other_connection, "tc-5", 13, NULL));

    mqtt_app_control_local_ping_sent(&state, "tc-6", 20);
    local_stl(&state, "{\"state\":\"on\",\"duration\":300}", 20);
    mqtt_app_control_reset(&state);
    assert(!mqtt_app_control_match_pong(&state, "tc-6", 21, NULL));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"active\",\"duration\":300}}", 21));
}

static void test_expiration_and_clock_wrap(void)
{
    mqtt_app_control_state_t state = {0};
    uint32_t elapsed = 0;
    mqtt_app_control_local_ping_sent(&state, "boundary", 1);
    assert(mqtt_app_control_match_pong(&state, "boundary", 30001, &elapsed));
    assert(elapsed == MQTT_APP_CONTROL_REPLY_WINDOW_MS);
    mqtt_app_control_local_ping_sent(&state, "expired", 2);
    assert(!mqtt_app_control_match_pong(&state, "expired", 30003, NULL));
    assert(!state.ping.pending);

    const uint32_t start = UINT32_MAX - 10;
    mqtt_app_control_local_ping_sent(&state, "wrap", start);
    assert(mqtt_app_control_match_pong(&state, "wrap", 9, &elapsed));
    assert(elapsed == 20);
    cloud(&state, "stl", "{}", start);
    local_stl(&state, "{\"state\":\"off\"}", 9);
    assert(state.stl.ambiguous);
    mqtt_app_control_expire(&state, 30010);
    assert(!state.stl.pending && !state.cloud_stl_pending);
    local_stl(&state, "{\"state\":\"off\"}", 30011);
    assert(bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 30012));
}

static void test_stl_expected_fields_and_values(void)
{
    mqtt_app_control_state_t state = {0};
    local_stl(&state, "{\"state\":\"on\",\"duration\":600}", 1);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\",\"duration\":600}}", 2));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300}}", 3));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\"}}", 4));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":\"600\"}}", 5));
    assert(bedtime(&state, "{\"stl\":{\"state\":\"active\",\"duration\":600,\"defaultDuration\":300,\"until\":\"later\"}}", 6));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"active\",\"duration\":600}}", 7));

    local_stl(&state, "{\"state\":\"active\",\"duration\":300}", 8);
    assert(bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300}}", 9));
    local_stl(&state, "{\"state\":\"off\"}", 10);
    assert(bedtime(&state, "{\"stl\":{\"state\":\"off\",\"duration\":0,\"until\":null}}", 11));
}

static void test_stl_extensions_and_invalid_commands(void)
{
    mqtt_app_control_state_t state = {0};
    const char *commands[] = {
        "{\"state\":\"on\",\"duration\":300,\"alarm\":{}}",
        "{\"state\":\"on\",\"duration\":300,\"extension\":true}",
        "{\"state\":\"on\"}",
        "{\"state\":\"on\",\"duration\":300.5}",
        "{\"state\":\"on\",\"duration\":-1}",
        "{\"state\":\"on\",\"duration\":4294967296}",
        "{\"state\":\"on\",\"duration\":1e999}",
        "{\"state\":\"on\",\"state\":\"off\",\"duration\":300}",
        "{\"state\":\"unknown\",\"duration\":300}",
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    {
        mqtt_app_control_reset(&state);
        local_stl(&state, commands[i], 1);
        assert(!state.stl.matchable);
        assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300}}", 2));
    }
    mqtt_app_control_reset(&state);
    local_stl(&state, "{\"state\":\"on\",\"duration\":300}", 1);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300},\"alarm\":{}}", 2));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300,\"extension\":true}}", 3));
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\",\"state\":\"on\",\"duration\":300}}", 4));
    assert(bedtime(&state, "{\"stl\":{\"state\":\"on\",\"duration\":300}}", 5));
}

static void test_stl_overlapping_local_and_cloud_commands(void)
{
    mqtt_app_control_state_t state = {0};
    local_stl(&state, "{\"state\":\"on\",\"duration\":300}", 1);
    local_stl(&state, "{\"state\":\"off\"}", 2);
    assert(state.stl.ambiguous);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 3));

    mqtt_app_control_reset(&state);
    local_stl(&state, "{\"state\":\"off\"}", 1);
    cloud(&state, "stl", "{\"state\":\"off\"}", 2);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 3));

    mqtt_app_control_reset(&state);
    cloud(&state, "stl", "{\"state\":\"off\"}", 1);
    local_stl(&state, "{\"state\":\"off\"}", 2);
    assert(state.stl.ambiguous);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 3));

    mqtt_app_control_reset(&state);
    local_stl(&state, "{\"state\":\"off\"}", 1);
    cloud(&state, "sleep", "{}", 2);
    assert(state.stl.ambiguous);
    assert(!bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 3));
    mqtt_app_control_expire(&state, 30003);
    local_stl(&state, "{\"state\":\"off\"}", 30004);
    cloud(&state, "volume", "{\"level\":3}", 30005);
    assert(bedtime(&state, "{\"stl\":{\"state\":\"off\"}}", 30006));
}

int main(void)
{
    test_ping_matching_and_replacement();
    test_cloud_ping_collision_and_connection_reset();
    test_expiration_and_clock_wrap();
    test_stl_expected_fields_and_values();
    test_stl_extensions_and_invalid_commands();
    test_stl_overlapping_local_and_cloud_commands();
    puts("MQTT app-control correlation: 6 focused cases passed");
    return 0;
}
