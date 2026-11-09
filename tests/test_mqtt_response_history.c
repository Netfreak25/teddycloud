/* Real SHA-256 and history module; only allocation is fault-injectable. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_response_history.h"

static bool fail_allocate;
static size_t allocations;
void *osAllocMem(size_t size)
{
    if (fail_allocate) return NULL;
    void *memory = malloc(size);
    if (memory != NULL) allocations++;
    return memory;
}
void osFreeMem(void *memory)
{
    if (memory != NULL) { assert(allocations); allocations--; free(memory); }
}

int main(void)
{
    mqtt_response_history_t history;
    mqtt_response_history_init(&history);
    uint8_t original[] = {0x32, 7, 0, 1, 'a', 0, 1, '{', '}'};
    uint8_t duplicate[sizeof(original)];
    memcpy(duplicate, original, sizeof(original)); duplicate[0] |= 0x08;
    assert(!mqtt_response_history_remember(&history, 1, original, sizeof(original),
        true, NULL, 0, 100));
    const mqtt_response_history_record_t *record = mqtt_response_history_find(
        &history, 1, duplicate, sizeof(duplicate), 101);
    assert(record && record->consume && !record->payload);
    assert(!mqtt_response_history_remember(&history, 1, original, sizeof(original),
        true, NULL, 0, 500));
    assert(!mqtt_response_history_find(&history, 1, duplicate, sizeof(duplicate), 30100));

    uint8_t clean[] = {'{', '}', '\n'};
    assert(!mqtt_response_history_remember(&history, 2, original, sizeof(original),
        false, clean, sizeof(clean), 1000));
    clean[0] = 'X'; // Stored rewrite owns its bytes.
    record = mqtt_response_history_find(&history, 2, duplicate, sizeof(duplicate), 1001);
    assert(record && !record->consume && record->payload_len == 3 && record->payload[0] == '{');
    duplicate[4] = 'b';
    assert(!mqtt_response_history_find(&history, 2, duplicate, sizeof(duplicate), 1002));
    assert(!history.payload_bytes && !allocations);
    duplicate[4] = 'a';
    assert(!mqtt_response_history_remember(&history, 2, original, sizeof(original),
        false, NULL, 0, 1000));
    record = mqtt_response_history_find(&history, 2, duplicate, sizeof(duplicate), 1001);
    assert(record && record->payload && !record->payload_len);
    mqtt_response_history_forget(&history, 2); // Required before a non-DUP ID reuse.
    assert(!mqtt_response_history_find(&history, 2, duplicate, sizeof(duplicate), 1002));

    for (uint16_t id = 1; id <= MQTT_RESPONSE_HISTORY_ENTRIES; id++)
        assert(!mqtt_response_history_remember(&history, id, original, sizeof(original),
            true, NULL, 0, 2000));
    assert(mqtt_response_history_remember(&history, 33, original, sizeof(original),
        true, NULL, 0, 2001) == ERROR_OUT_OF_RESOURCES);
    assert(mqtt_response_history_find(&history, 1, duplicate, sizeof(duplicate), 2001));
    mqtt_response_history_reset(&history);
    uint8_t *large = malloc(MQTT_RESPONSE_HISTORY_BYTES);
    assert(large); memset(large, 'a', MQTT_RESPONSE_HISTORY_BYTES);
    assert(!mqtt_response_history_remember(&history, 1, original, sizeof(original),
        false, large, MQTT_RESPONSE_HISTORY_BYTES, 3000));
    assert(mqtt_response_history_remember(&history, 2, original, sizeof(original),
        false, clean, 1, 3001) == ERROR_OUT_OF_RESOURCES);
    assert(history.payload_bytes == MQTT_RESPONSE_HISTORY_BYTES);
    mqtt_response_history_reset(&history);
    free(large);

    assert(!mqtt_response_history_remember(&history, 1, original, sizeof(original),
        false, clean, sizeof(clean), 4000));
    fail_allocate = true;
    assert(mqtt_response_history_remember(&history, 2, original, sizeof(original),
        false, clean, sizeof(clean), 4001) == ERROR_OUT_OF_MEMORY);
    assert(mqtt_response_history_find(&history, 1, duplicate, sizeof(duplicate), 4002));
    fail_allocate = false;
    mqtt_response_history_reset(&history);
    assert(!mqtt_response_history_remember(&history, 1, original, sizeof(original),
        true, NULL, 0, UINT32_MAX - 99));
    assert(mqtt_response_history_find(&history, 1, duplicate, sizeof(duplicate), 0));
    assert(!mqtt_response_history_find(&history, 1, duplicate, sizeof(duplicate), 29900));
    mqtt_response_history_reset(&history);
    assert(!allocations && !history.payload_bytes);
    puts("MQTT response history PASS: DUP identity, sanitized replay, expiry, reuse and hard memory limits");
    return 0;
}
