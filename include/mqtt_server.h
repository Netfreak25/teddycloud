#pragma once

#include "error.h"
#include "handler.h"

typedef enum
{
    MQTT_SERVER_PLAYBACK_START,
    MQTT_SERVER_PLAYBACK_PAUSE,
    MQTT_SERVER_PLAYBACK_NEXT,
    MQTT_SERVER_PLAYBACK_PREV,
    MQTT_SERVER_PLAYBACK_RESTART,
} mqtt_server_playback_action_t;

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
void mqtt_server_mark_toniebox2_settings_changed(uint8_t overlay_id);
void mqtt_server_mark_toniebox2_setting_changed(uint8_t overlay_id, const char *setting_name);
bool_t mqtt_server_publish_toniebox2_settings_desired_for_overlay(uint8_t overlay_id);
bool_t mqtt_server_publish_playback_for_overlay(uint8_t overlay_id, mqtt_server_playback_action_t action);
bool_t mqtt_server_publish_playback_position_for_overlay(uint8_t overlay_id, uint32_t chapter, uint32_t position_ms);
bool_t mqtt_server_publish_volume_for_overlay(uint8_t overlay_id, uint32_t level);
bool_t mqtt_server_publish_ping_for_overlay(uint8_t overlay_id, char *request_id, size_t request_id_size);
bool_t mqtt_server_publish_app_control_stl_for_overlay(uint8_t overlay_id, const char *payload_json);
bool_t mqtt_server_publish_app_control_sleep_for_overlay(uint8_t overlay_id);
