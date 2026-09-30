#ifndef MQTT_DELIVERY_H
#define MQTT_DELIVERY_H

#include "compiler_port.h"
#include "error.h"

/** Acceptance and actual transport completion are deliberately distinct. */
typedef enum {
    MQTT_DELIVERY_SENT,
    MQTT_DELIVERY_QUEUED,
    MQTT_DELIVERY_BUSY,
    MQTT_DELIVERY_FAILED
} mqtt_delivery_status_t;

typedef struct {
    mqtt_delivery_status_t status;
    error_t error;
} mqtt_delivery_result_t;

static inline mqtt_delivery_result_t mqtt_delivery_result(mqtt_delivery_status_t status, error_t error)
{
    mqtt_delivery_result_t result = {status, error};
    return result;
}

static inline bool_t mqtt_delivery_accepted(mqtt_delivery_result_t result)
{
    return result.status == MQTT_DELIVERY_SENT || result.status == MQTT_DELIVERY_QUEUED;
}

#endif
