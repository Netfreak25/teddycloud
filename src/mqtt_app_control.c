#include <math.h>
#include <stddef.h>
#include <string.h>

#include "mqtt_app_control.h"

static bool mqtt_app_control_elapsed(uint32_t sent_at, uint32_t now)
{
    return (uint32_t)(now - sent_at) > MQTT_APP_CONTROL_REPLY_WINDOW_MS;
}

/* Reject extensions and duplicate keys: neither can be safely attributed to
 * the simple local command. All callers use at most four allowed fields. */
static bool mqtt_app_control_shape(const cJSON *object,
                                    const char *const *allowed, size_t count)
{
    if (!cJSON_IsObject(object))
        return false;
    unsigned seen = 0;
    const cJSON *field;
    cJSON_ArrayForEach(field, object)
    {
        size_t index;
        for (index = 0; field->string != NULL && index < count; index++)
            if (strcmp(field->string, allowed[index]) == 0)
                break;
        if (field->string == NULL || index == count || (seen & (1U << index)))
            return false;
        seen |= 1U << index;
    }
    return true;
}

static bool mqtt_app_control_enabled(const cJSON *object, bool *enabled)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, "state");
    if (!cJSON_IsString(field) || field->valuestring == NULL)
        return false;
    if (strcmp(field->valuestring, "on") == 0 ||
        strcmp(field->valuestring, "active") == 0)
        *enabled = true;
    else if (strcmp(field->valuestring, "off") == 0)
        *enabled = false;
    else
        return false;
    return true;
}

static bool mqtt_app_control_duration(const cJSON *object, uint32_t *duration)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, "duration");
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < 0 || field->valuedouble > UINT32_MAX)
        return false;
    uint32_t value = (uint32_t)field->valuedouble;
    if ((double)value != field->valuedouble)
        return false;
    *duration = value;
    return true;
}

void mqtt_app_control_reset(mqtt_app_control_state_t *state)
{
    if (state != NULL)
        memset(state, 0, sizeof(*state));
}

void mqtt_app_control_expire(mqtt_app_control_state_t *state, uint32_t now)
{
    if (state == NULL)
        return;
    if (state->ping.pending && mqtt_app_control_elapsed(state->ping.sent_at, now))
        memset(&state->ping, 0, sizeof(state->ping));
    if (state->stl.pending && mqtt_app_control_elapsed(state->stl.sent_at, now))
        memset(&state->stl, 0, sizeof(state->stl));
    if (state->cloud_stl_pending && mqtt_app_control_elapsed(state->cloud_stl_at, now))
    {
        state->cloud_stl_pending = false;
        state->cloud_stl_at = 0;
    }
}

void mqtt_app_control_local_ping_sent(mqtt_app_control_state_t *state,
                                      const char *request_id, uint32_t now)
{
    if (state == NULL)
        return;
    mqtt_app_control_expire(state, now);
    bool valid = request_id != NULL && request_id[0] != '\0' &&
                 strlen(request_id) < sizeof(state->ping.request_id);
    bool ambiguous = valid && state->ping.pending &&
                     strcmp(state->ping.request_id, request_id) == 0;
    memset(&state->ping, 0, sizeof(state->ping));
    if (!valid)
        return;
    state->ping.pending = true;
    state->ping.ambiguous = ambiguous;
    state->ping.sent_at = now;
    memcpy(state->ping.request_id, request_id, strlen(request_id) + 1);
}

void mqtt_app_control_local_stl_sent(mqtt_app_control_state_t *state,
                                     const cJSON *command, uint32_t now)
{
    static const char *const allowed[] = {"state", "duration"};
    if (state == NULL)
        return;
    mqtt_app_control_expire(state, now);
    bool overlap = state->stl.pending || state->cloud_stl_pending;
    memset(&state->stl, 0, sizeof(state->stl));
    state->stl.pending = true;
    state->stl.ambiguous = overlap;
    state->stl.sent_at = now;
    state->stl.matchable = mqtt_app_control_shape(command, allowed, 2) &&
                           mqtt_app_control_enabled(command, &state->stl.enabled) &&
                           (!state->stl.enabled ||
                            mqtt_app_control_duration(command, &state->stl.duration));
}

void mqtt_app_control_cloud_delivered(mqtt_app_control_state_t *state,
                                      const char *command, const cJSON *payload,
                                      uint32_t now)
{
    if (state == NULL || command == NULL)
        return;
    mqtt_app_control_expire(state, now);
    if (strcmp(command, "stl") == 0 || strcmp(command, "sleep") == 0)
    {
        state->cloud_stl_pending = true;
        state->cloud_stl_at = now;
        if (state->stl.pending)
            state->stl.ambiguous = true;
    }
    else if (strcmp(command, "ping") == 0 && state->ping.pending && cJSON_IsObject(payload))
    {
        /* Any duplicate requestId that matches is already ambiguous. */
        const cJSON *field;
        cJSON_ArrayForEach(field, payload)
            if (field->string != NULL && strcmp(field->string, "requestId") == 0 &&
                cJSON_IsString(field) && field->valuestring != NULL &&
                strcmp(field->valuestring, state->ping.request_id) == 0)
                state->ping.ambiguous = true;
    }
}

bool mqtt_app_control_match_pong(mqtt_app_control_state_t *state,
                                 const char *request_id, uint32_t now,
                                 uint32_t *round_trip_ms)
{
    if (state == NULL)
        return false;
    mqtt_app_control_expire(state, now);
    if (!state->ping.pending || state->ping.ambiguous || request_id == NULL ||
        strcmp(state->ping.request_id, request_id) != 0)
        return false;
    if (round_trip_ms != NULL)
        *round_trip_ms = (uint32_t)(now - state->ping.sent_at);
    memset(&state->ping, 0, sizeof(state->ping));
    return true;
}

bool mqtt_app_control_match_bedtime(mqtt_app_control_state_t *state,
                                    const cJSON *reply, uint32_t now)
{
    static const char *const outer[] = {"stl"};
    static const char *const inner[] = {"state", "duration", "defaultDuration", "until"};
    if (state == NULL)
        return false;
    mqtt_app_control_expire(state, now);
    if (!state->stl.pending || !state->stl.matchable || state->stl.ambiguous ||
        !mqtt_app_control_shape(reply, outer, 1))
        return false;
    const cJSON *stl = cJSON_GetObjectItemCaseSensitive(reply, "stl");
    bool enabled;
    uint32_t duration;
    if (!mqtt_app_control_shape(stl, inner, 4) ||
        !mqtt_app_control_enabled(stl, &enabled) || enabled != state->stl.enabled ||
        (enabled && (!mqtt_app_control_duration(stl, &duration) ||
                     duration != state->stl.duration)))
        return false;
    memset(&state->stl, 0, sizeof(state->stl));
    return true;
}
