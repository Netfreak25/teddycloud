#ifndef MQTT_FORWARD_FILTER_H
#define MQTT_FORWARD_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "compiler_port.h"
#include "settings.h"

bool_t mqtt_forward_filter_should_block(settings_t *box_settings,
                                        const char *topic,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        const char **filter_id);

#endif
