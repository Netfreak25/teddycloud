#ifndef MQTT_FORWARD_FILTER_H
#define MQTT_FORWARD_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "compiler_port.h"
#include "settings.h"

typedef enum
{
    MQTT_FORWARD_ROUTE_BOX_TO_TONIES,
    MQTT_FORWARD_ROUTE_TONIES_TO_BOX,
    MQTT_FORWARD_ROUTE_LOCAL_TO_BOX
} mqtt_forward_route_t;

typedef enum
{
    MQTT_FORWARD_ACTION_FORWARD,
    MQTT_FORWARD_ACTION_BLOCK,
    MQTT_FORWARD_ACTION_LOCAL
} mqtt_forward_action_t;

typedef enum
{
    MQTT_FORWARD_REASON_RULE_ALLOWED,
    MQTT_FORWARD_REASON_RULE_BLOCKED,
    MQTT_FORWARD_REASON_MASTER_BYPASS,
    MQTT_FORWARD_REASON_LOCAL,
    MQTT_FORWARD_REASON_NOT_EVALUATED_LOCAL_CONSUME,
    MQTT_FORWARD_REASON_NOT_EVALUATED_AUTOMATIC_BLOCK
} mqtt_forward_reason_t;

typedef struct
{
    mqtt_forward_route_t route;
    mqtt_forward_action_t action;
    // Static registration key; NULL when manual filtering was not evaluated.
    const char *setting_id;
    mqtt_forward_reason_t reason;
} mqtt_forward_filter_result_t;

/** Evaluate manual Internet forwarding only; automatic content protection
 *  must already have approved any message offered for relay. */
mqtt_forward_filter_result_t mqtt_forward_filter_evaluate(
    settings_t *box_settings, mqtt_forward_route_t route, const char *topic,
    const uint8_t *payload, size_t payload_len);

const char *mqtt_forward_route_name(mqtt_forward_route_t route);
const char *mqtt_forward_reason_name(mqtt_forward_reason_t reason);

#endif
