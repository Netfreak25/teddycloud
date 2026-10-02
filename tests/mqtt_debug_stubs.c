/* Diagnostic boundary for existing native protocol tests. The collector has
 * separate tests; disabled diagnostics must leave protocol behavior unchanged. */
#include "mqtt_debug.h"
#include <string.h>
#include <stdatomic.h>

/* Optional in-memory observer for instrumentation assertions. No production
 * collector, filesystem or TLS behavior is substituted by this boundary. */
static bool test_active;
static atomic_uint_fast64_t test_now;
static uint64_t test_message;
static cJSON *test_events;
void mqtt_debug_test_reset(bool active, uint64_t now)
{
    cJSON_Delete(test_events);
    test_events = active ? cJSON_CreateArray() : NULL;
    test_active = active;
    atomic_store(&test_now, now);
    test_message = 0;
}
void mqtt_debug_test_time(uint64_t now) { atomic_store(&test_now, now); }
unsigned mqtt_debug_test_count(const char *stage)
{
    unsigned count = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, test_events)
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(entry, "stage")), stage)) count++;
    return count;
}
const cJSON *mqtt_debug_test_last(const char *stage)
{
    const cJSON *last = NULL;
    cJSON *entry;
    cJSON_ArrayForEach(entry, test_events)
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(entry, "stage")), stage)) last = entry;
    return last;
}

bool mqtt_debug_enabled(settings_t *settings) { (void)settings; return false; }
bool mqtt_debug_active(uint64_t owner) { (void)owner; return test_active; }
uint64_t mqtt_debug_now_ms(void) { return atomic_load(&test_now); }
uint64_t mqtt_debug_wall_ms(void) { return 0; }
uint64_t mqtt_debug_next_message(uint64_t owner) { (void)owner; return test_active ? ++test_message : 0; }
void mqtt_debug_init(void) {}
void mqtt_debug_deinit(void) {}
void mqtt_debug_sync(uint64_t owner, settings_t *settings, bool established)
{ (void)owner; (void)settings; (void)established; }
void mqtt_debug_close(uint64_t owner, const char *reason) { (void)owner; (void)reason; }
void mqtt_debug_event(uint64_t owner, uint64_t epoch, uint64_t message,
    const char *origin, const char *stage, cJSON *details)
{
    (void)owner; (void)epoch;
    if (!test_active) { cJSON_Delete(details); return; }
    cJSON *entry = details != NULL ? details : cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "stage", stage);
    cJSON_AddStringToObject(entry, "origin", origin);
    cJSON_AddNumberToObject(entry, "message_id", (double)message);
    cJSON_AddItemToArray(test_events, entry);
}
void mqtt_debug_packet(uint64_t owner, uint64_t epoch, uint64_t message,
    const char *origin, const char *stage, const uint8_t *data, size_t length)
{
    (void)data;
    if (!test_active) return;
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "bytes", (double)length);
    mqtt_debug_event(owner, epoch, message, origin, stage, entry);
}
