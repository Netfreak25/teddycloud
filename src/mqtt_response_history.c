#include <string.h>

#include "mqtt_response_history.h"
#include "hash/sha256.h"
#include "os_port.h"

static void mqtt_response_history_remove(mqtt_response_history_t *history,
    mqtt_response_history_record_t *record)
{
    history->payload_bytes -= record->payload_len;
    osFreeMem(record->payload);
    memset(record, 0, sizeof(*record));
}

static void mqtt_response_history_expire(mqtt_response_history_t *history, uint32_t now)
{
    for (size_t i = 0; i < MQTT_RESPONSE_HISTORY_ENTRIES; i++)
    {
        mqtt_response_history_record_t *record = &history->records[i];
        if (record->packet_id != 0 &&
            (uint32_t)(now - record->recorded_at) >= MQTT_RESPONSE_HISTORY_WINDOW_MS)
            mqtt_response_history_remove(history, record);
    }
}

static bool mqtt_response_history_hash(const uint8_t *original, size_t length,
    uint8_t digest[32])
{
    if (original == NULL || length < 2 || original[0] >> 4 != 3)
        return false;
    Sha256Context context;
    uint8_t first = original[0] & ~0x08U;
    sha256Init(&context);
    sha256Update(&context, &first, 1);
    sha256Update(&context, original + 1, length - 1);
    sha256Final(&context, digest);
    return true;
}

void mqtt_response_history_init(mqtt_response_history_t *history)
{
    if (history != NULL)
        memset(history, 0, sizeof(*history));
}

void mqtt_response_history_reset(mqtt_response_history_t *history)
{
    if (history == NULL)
        return;
    for (size_t i = 0; i < MQTT_RESPONSE_HISTORY_ENTRIES; i++)
        mqtt_response_history_remove(history, &history->records[i]);
}

void mqtt_response_history_forget(mqtt_response_history_t *history, uint16_t packet_id)
{
    if (history == NULL || packet_id == 0)
        return;
    for (size_t i = 0; i < MQTT_RESPONSE_HISTORY_ENTRIES; i++)
        if (history->records[i].packet_id == packet_id)
            mqtt_response_history_remove(history, &history->records[i]);
}

const mqtt_response_history_record_t *mqtt_response_history_find(
    mqtt_response_history_t *history, uint16_t packet_id,
    const uint8_t *original, size_t original_len, uint32_t now)
{
    if (history == NULL || packet_id == 0)
        return NULL;
    mqtt_response_history_expire(history, now);
    uint8_t digest[32];
    if (!mqtt_response_history_hash(original, original_len, digest))
        return NULL;
    for (size_t i = 0; i < MQTT_RESPONSE_HISTORY_ENTRIES; i++)
    {
        mqtt_response_history_record_t *record = &history->records[i];
        if (record->packet_id != packet_id)
            continue;
        if (memcmp(record->original_sha256, digest, sizeof(digest)) == 0)
            return record;
        mqtt_response_history_remove(history, record);
        return NULL;
    }
    return NULL;
}

error_t mqtt_response_history_remember(mqtt_response_history_t *history,
    uint16_t packet_id, const uint8_t *original, size_t original_len,
    bool consume, const uint8_t *payload, size_t payload_len, uint32_t now)
{
    uint8_t digest[32];
    if (history == NULL || packet_id == 0 ||
        !mqtt_response_history_hash(original, original_len, digest) ||
        (consume && payload_len != 0) || (!consume && payload_len != 0 && payload == NULL))
        return ERROR_INVALID_PARAMETER;
    mqtt_response_history_expire(history, now);
    mqtt_response_history_record_t *target = NULL;
    for (size_t i = 0; i < MQTT_RESPONSE_HISTORY_ENTRIES; i++)
    {
        mqtt_response_history_record_t *record = &history->records[i];
        if (record->packet_id == packet_id)
        {
            if (memcmp(record->original_sha256, digest, sizeof(digest)) == 0)
                return NO_ERROR; // Retries do not extend the privacy window.
            target = record;
            break;
        }
        if (record->packet_id == 0 && target == NULL)
            target = record;
    }
    if (target == NULL || payload_len > MQTT_RESPONSE_HISTORY_BYTES ||
        history->payload_bytes - target->payload_len > MQTT_RESPONSE_HISTORY_BYTES - payload_len)
        return ERROR_OUT_OF_RESOURCES;
    uint8_t *copy = NULL;
    if (!consume)
    {
        copy = osAllocMem(payload_len != 0 ? payload_len : 1);
        if (copy == NULL)
            return ERROR_OUT_OF_MEMORY;
        if (payload_len != 0)
            memcpy(copy, payload, payload_len);
    }
    mqtt_response_history_remove(history, target);
    target->packet_id = packet_id;
    target->recorded_at = now;
    memcpy(target->original_sha256, digest, sizeof(digest));
    target->consume = consume;
    target->payload = copy;
    target->payload_len = payload_len;
    history->payload_bytes += payload_len;
    return NO_ERROR;
}
