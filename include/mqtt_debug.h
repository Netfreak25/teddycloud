#ifndef MQTT_DEBUG_H
#define MQTT_DEBUG_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
#include "cJSON.h"

typedef struct mqtt_debug_file mqtt_debug_file_t;

/* Diagnostic selection is explicit per box; a global value never enables it. */
bool mqtt_debug_enabled(settings_t *settings);
void mqtt_debug_init(void);
void mqtt_debug_deinit(void);
void mqtt_debug_sync(uint64_t owner, settings_t *settings, bool established);
void mqtt_debug_close(uint64_t owner, const char *reason);
bool mqtt_debug_active(uint64_t owner);
uint64_t mqtt_debug_now_ms(void);
uint64_t mqtt_debug_wall_ms(void);
uint64_t mqtt_debug_next_message(uint64_t owner);

/* Takes ownership of details, including when disabled. Calls only queue bounded
 * sanitized data; MQTT callers never perform diagnostic filesystem operations. */
void mqtt_debug_event(uint64_t owner, uint64_t epoch, uint64_t message,
    const char *origin, const char *stage, cJSON *details);
void mqtt_debug_packet(uint64_t owner, uint64_t epoch, uint64_t message,
    const char *origin, const char *stage, const uint8_t *data, size_t length);
void mqtt_debug_unassigned(const char *stage, cJSON *details);

/* Caller owns JSON. Export never exposes legacy raw capture paths. */
cJSON *mqtt_debug_status(settings_t *settings);
/* Remove one recording, including pending diagnostic events; never closes MQTT. */
error_t mqtt_debug_delete(settings_t *settings, const char *session);
error_t mqtt_debug_file_open(settings_t *settings, const char *session,
    const char *name, mqtt_debug_file_t **file, uint64_t *length);
error_t mqtt_debug_file_read(mqtt_debug_file_t *file, void *buffer,
    size_t length, size_t *received);
error_t mqtt_debug_file_limit(mqtt_debug_file_t *file, uint64_t length);
void mqtt_debug_file_close(mqtt_debug_file_t *file);

#endif
