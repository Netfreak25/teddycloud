#ifndef MQTT_APP_CONTROL_H
#define MQTT_APP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define MQTT_APP_CONTROL_REPLY_WINDOW_MS 30000U
#define MQTT_APP_CONTROL_REQUEST_ID_MAX 64U

typedef struct
{
    bool pending;
    bool ambiguous;
    uint32_t sent_at;
    char request_id[MQTT_APP_CONTROL_REQUEST_ID_MAX];
} mqtt_app_control_ping_t;

typedef struct
{
    bool pending;
    bool ambiguous;
    bool matchable;
    bool enabled;
    uint32_t sent_at;
    uint32_t duration;
} mqtt_app_control_stl_t;

/** One bounded ledger per MQTT connection. Caller serializes all access and
 * records only completed sends. Times are monotonic milliseconds, not UTC.
 * Clear on connection close; the owner generates non-repeating ping IDs. */
typedef struct
{
    mqtt_app_control_ping_t ping;
    mqtt_app_control_stl_t stl;
    bool cloud_stl_pending;
    uint32_t cloud_stl_at;
} mqtt_app_control_state_t;

void mqtt_app_control_reset(mqtt_app_control_state_t *state);
void mqtt_app_control_expire(mqtt_app_control_state_t *state, uint32_t now);
void mqtt_app_control_local_ping_sent(mqtt_app_control_state_t *state,
                                      const char *request_id, uint32_t now);
void mqtt_app_control_local_stl_sent(mqtt_app_control_state_t *state,
                                     const cJSON *command, uint32_t now);
/** command is the delivered app-control suffix: ping, stl or sleep. */
void mqtt_app_control_cloud_delivered(mqtt_app_control_state_t *state,
                                      const char *command, const cJSON *payload,
                                      uint32_t now);
bool mqtt_app_control_match_pong(mqtt_app_control_state_t *state,
                                 const char *request_id, uint32_t now,
                                 uint32_t *round_trip_ms);
/** STL has no request ID. Match only simple expected state/duration, without
 * known overlapping commands or extra response fields. This cannot prove the
 * origin of an indistinguishable, unsolicited state announcement. */
bool mqtt_app_control_match_bedtime(mqtt_app_control_state_t *state,
                                    const cJSON *reply, uint32_t now);

#endif
