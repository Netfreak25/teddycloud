#ifndef MQTT_SETTINGS_H
#define MQTT_SETTINGS_H

#include "cJSON.h"
#include "settings.h"

#define MQTT_SETTINGS_SNAPSHOT_LIMIT (64U * 1024U)
#define MQTT_SETTINGS_REVISION_MAX UINT64_C(9007199254740991)
#define MQTT_SETTINGS_SENT_GRACE_MS 30000U

typedef enum
{
    MQTT_TB2_SETTING_MAX_VOLUME = 0,
    MQTT_TB2_SETTING_BEDTIME_MAX_VOLUME,
    MQTT_TB2_SETTING_MAX_HEADPHONE_VOLUME,
    MQTT_TB2_SETTING_BEDTIME_MAX_HEADPHONE_VOLUME,
    MQTT_TB2_SETTING_LIGHTRING_BRIGHTNESS,
    MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS,
    MQTT_TB2_SETTING_SCRUBBING_ENABLED,
    MQTT_TB2_SETTING_SKIPPING_ENABLED,
    MQTT_TB2_SETTING_SKIPPING_DIRECTION,
    MQTT_TB2_SETTING_AGE_MODE,
    MQTT_TB2_SETTING_COUNT
} mqtt_settings_field_t;

typedef struct
{
    const char *json_name;
    const char *setting_name;
} mqtt_settings_descriptor_t;

extern const mqtt_settings_descriptor_t mqtt_settings_descriptors[MQTT_TB2_SETTING_COUNT];

bool mqtt_settings_cloud_managed(settings_t *settings);
int mqtt_settings_field_index(const char *setting_name);
bool mqtt_settings_json_revision(const cJSON *item, uint64_t *revision);

/** Accept only a fully forwarded Cloud Desired; never publishes or changes pending state. */
error_t mqtt_settings_accept_cloud(settings_t *settings, const uint8_t *payload,
                                  size_t length, uint16_t *accepted_mask);
/** Caller owns the returned copy; NULL means no valid snapshot. */
cJSON *mqtt_settings_snapshot_copy(settings_t *settings);
uint64_t mqtt_settings_revision_floor(settings_t *settings);
const char *mqtt_settings_field_state(settings_t *settings, size_t index);
void mqtt_settings_confirm_cloud(settings_t *settings, cJSON *toniebox_history);
/** A local field write supersedes any historical Cloud application proof. */
void mqtt_settings_local_sent(settings_t *settings, size_t index);
/** Caller holds MUTEX_SETTINGS; does not save, publish or recursively lock it. */
void mqtt_settings_reapply_overlays_locked(void);

/** Fixed per-connection ledger; never contains a merely received Cloud revision. */
typedef struct
{
    uint64_t revision[MQTT_TB2_SETTING_COUNT];
    bool valid[MQTT_TB2_SETTING_COUNT];
    bool ambiguous[MQTT_TB2_SETTING_COUNT];
    bool authority_known;
    bool cloud_managed;
    uint32_t transition_at;
    bool transition_pending;
    uint16_t transition_mask;
} mqtt_settings_sent_t;

void mqtt_settings_sent_authority(mqtt_settings_sent_t *state, bool cloud, uint32_t now);
void mqtt_settings_sent_record(mqtt_settings_sent_t *state, size_t index, uint64_t revision);
void mqtt_settings_sent_cloud(mqtt_settings_sent_t *state, size_t index, uint64_t revision);
bool mqtt_settings_sent_match(mqtt_settings_sent_t *state, size_t index,
                             uint64_t revision, uint32_t now);

#endif
