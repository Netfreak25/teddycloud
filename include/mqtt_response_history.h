#ifndef MQTT_RESPONSE_HISTORY_H
#define MQTT_RESPONSE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "error.h"

#define MQTT_RESPONSE_HISTORY_ENTRIES 32U
#define MQTT_RESPONSE_HISTORY_BYTES (1024U * 1024U)
#define MQTT_RESPONSE_HISTORY_WINDOW_MS 30000U

typedef struct
{
    uint16_t packet_id;
    uint32_t recorded_at;
    uint8_t original_sha256[32];
    bool consume;
    uint8_t *payload;
    size_t payload_len;
} mqtt_response_history_record_t;

/** Bounded observer-result history, not an offline forwarding queue. The caller
 * serializes access and forgets an ID before processing any non-DUP PUBLISH. */
typedef struct
{
    mqtt_response_history_record_t records[MQTT_RESPONSE_HISTORY_ENTRIES];
    size_t payload_bytes;
} mqtt_response_history_t;

void mqtt_response_history_init(mqtt_response_history_t *history);
void mqtt_response_history_reset(mqtt_response_history_t *history);
void mqtt_response_history_forget(mqtt_response_history_t *history, uint16_t packet_id);
/** Call only for DUP PUBLISH; returned storage lives until the next mutation.
 * Original packet fingerprints ignore only the fixed-header DUP bit. A reused
 * ID with different bytes forgets its old record and never matches. */
const mqtt_response_history_record_t *mqtt_response_history_find(
    mqtt_response_history_t *history, uint16_t packet_id,
    const uint8_t *original, size_t original_len, uint32_t now);
/** Store only a locally consumed/rewritten observer result. Allocation/limit
 * errors must not be ignored: privacy-sensitive replies cannot safely be
 * reprocessed after their local Pending state has already been consumed. */
error_t mqtt_response_history_remember(mqtt_response_history_t *history,
    uint16_t packet_id, const uint8_t *original, size_t original_len,
    bool consume, const uint8_t *payload, size_t payload_len, uint32_t now);

#endif
