#pragma once

#include "error.h"
#include "handler.h"
#include "mqtt_delivery.h"

typedef enum
{
    MQTT_SERVER_PLAYBACK_START,
    MQTT_SERVER_PLAYBACK_PAUSE,
    MQTT_SERVER_PLAYBACK_NEXT,
    MQTT_SERVER_PLAYBACK_PREV,
    MQTT_SERVER_PLAYBACK_RESTART,
} mqtt_server_playback_action_t;

/** Optional diagnostic timing only; never publishes or changes a box state. */
void mqtt_server_debug_hass_duration(client_ctx_t *client_ctx, uint64_t started_ms);

void mqtt_server_init();
error_t mqtt_server_reload_certificate();
void mqtt_server_task();
void mqtt_server_deinit();
bool_t mqtt_server_has_active_box_connection(uint8_t overlay_id);
bool_t mqtt_server_has_playback_control(uint8_t overlay_id);
bool_t mqtt_server_has_volume_control(uint8_t overlay_id);
bool_t mqtt_server_has_ping_control(uint8_t overlay_id);
bool_t mqtt_server_has_bedtime_control(uint8_t overlay_id);
bool_t mqtt_server_has_sleep_control(uint8_t overlay_id);
bool_t mqtt_server_publish_fresh_tonies(client_ctx_t *client_ctx);
bool_t mqtt_server_publish_fresh_tonies_for_overlay(uint8_t overlay_id);
bool_t mqtt_server_publish_fresh_tonie_for_overlay(uint8_t overlay_id,
                                                   uint64_t uid);
/** Owned per-request cache/generation view; completion never confirms newer changes. */
typedef struct mqtt_freshness_snapshot mqtt_freshness_snapshot_t;
mqtt_freshness_snapshot_t *mqtt_server_freshness_begin(uint8_t overlay_id);
const uint64_t *mqtt_server_freshness_cache(const mqtt_freshness_snapshot_t *snapshot,
                                          size_t *count);
error_t mqtt_server_freshness_prepare(mqtt_freshness_snapshot_t *snapshot,
                                      const uint64_t *stale_uids, size_t count);
void mqtt_server_freshness_finish(mqtt_freshness_snapshot_t *snapshot, bool_t sent);
/** Enable lifecycle hooks after mutex initialization, before server threads. */
void mqtt_server_freshness_init(void);
/** Called when an overlay is removed/reassigned, never on MQTT disconnect. */
void mqtt_server_freshness_forget_overlay(uint8_t overlay_id);
void mqtt_server_freshness_reconcile_overlays(void);
void mqtt_server_freshness_begin_reload(void);
/** Caller already holds MUTEX_MQTT_SESSION around the content mutation. */
bool_t mqtt_server_publish_fresh_tonie_for_overlay_locked(uint8_t overlay_id, uint64_t uid);
void mqtt_server_mark_toniebox2_settings_changed(uint8_t overlay_id);
void mqtt_server_mark_toniebox2_setting_changed(uint8_t overlay_id, const char *setting_name);
bool_t mqtt_server_publish_toniebox2_settings_desired_for_overlay(uint8_t overlay_id);
mqtt_delivery_result_t mqtt_server_publish_playback_for_overlay(uint8_t overlay_id, mqtt_server_playback_action_t action);
mqtt_delivery_result_t mqtt_server_publish_playback_position_for_overlay(uint8_t overlay_id, uint32_t chapter, uint32_t position_ms);
mqtt_delivery_result_t mqtt_server_publish_volume_for_overlay(uint8_t overlay_id, uint32_t level);
mqtt_delivery_result_t mqtt_server_publish_ping_for_overlay(uint8_t overlay_id, char *request_id, size_t request_id_size);
mqtt_delivery_result_t mqtt_server_publish_app_control_stl_for_overlay(uint8_t overlay_id, const char *payload_json);
mqtt_delivery_result_t mqtt_server_publish_app_control_sleep_for_overlay(uint8_t overlay_id);
/** Atomically admit optional STL and Sleep on packet-aware connections. */
mqtt_delivery_result_t mqtt_server_publish_shutdown_for_overlay(uint8_t overlay_id,
    const char *bedtime_payload_json, bool_t *bedtime_sent);
