#ifndef MQTT_NOCLOUD_FILTER_H
#define MQTT_NOCLOUD_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "compiler_port.h"
#include "settings.h"

typedef enum
{
    MQTT_NOCLOUD_ALLOW,
    MQTT_NOCLOUD_BLOCK,
    MQTT_NOCLOUD_REWRITE,
} mqtt_nocloud_action_t;

typedef struct
{
    mqtt_nocloud_action_t action;
    char *payload;
    size_t payload_len;
    const char *filter_id;
    size_t removed_count;
} mqtt_nocloud_filter_result_t;

void mqtt_nocloud_filter_publish(settings_t *box_settings,
                                 bool_t box_to_upstream,
                                 const char *topic,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 mqtt_nocloud_filter_result_t *result);
void mqtt_nocloud_filter_result_free(mqtt_nocloud_filter_result_t *result);

#endif
