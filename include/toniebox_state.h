#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "toniefile.h"

#include "toniebox_state_type.h"
#include "handler_sse.h"
#include "mqtt.h"

void toniebox_state_init();
void toniebox_state_restore();
toniebox_state_t *get_toniebox_state();
toniebox_state_t *get_toniebox_state_id(uint8_t id);
const char *tbs_toniebox2_playback_status_name(toniebox_state_tb2_playback_status_t status);
const char *tbs_toniebox2_volume_source_name(const toniebox_state_volume_t *volume);
void tbs_toniebox2_volume_snapshot(uint8_t overlay_id, toniebox_state_volume_t *volume);

void tbs_tag_placed(client_ctx_t *client_ctx, uint64_t uid, bool valid);
void tbs_tag_removed(client_ctx_t *client_ctx, uint64_t uid, bool valid);
void tbs_knock(client_ctx_t *client_ctx, bool forward);
void tbs_tilt(client_ctx_t *client_ctx, bool forward);
void tbs_playback(client_ctx_t *client_ctx, toniebox_state_playback_t playback);
void tbs_toniebox2_playback_state(client_ctx_t *client_ctx, const char *tonie,
                                  bool content_version_valid, uint64_t content_version,
                                  bool chapter_valid, uint32_t chapter,
                                  bool chapter_until_ms_valid, uint64_t chapter_until_ms,
                                  const char *chapter_duration);
void tbs_toniebox2_playback_command_state(client_ctx_t *client_ctx,
                                          toniebox_state_tb2_playback_status_t status);
void tbs_toniebox2_claim(client_ctx_t *client_ctx, const char *ruid, const char *bd, bool bd_all_zero);
void tbs_toniebox2_bedtime_state_changed(client_ctx_t *client_ctx);
void tbs_toniebox2_battery_metrics(client_ctx_t *client_ctx,
                                   bool percent_valid, uint32_t percent,
                                   bool raw_valid, int32_t raw,
                                   bool current_valid, int32_t current,
                                   const char *status);
void tbs_toniebox2_headphones_metrics(client_ctx_t *client_ctx,
                                      bool speaker_output_valid, bool speaker_output,
                                      bool connected_valid, uint32_t connected_count,
                                      const char *connected_json);
void tbs_toniebox2_volume_state(client_ctx_t *client_ctx, uint32_t level);
void tbs_toniebox2_volume_command(uint8_t overlay_id, uint32_t level,
                                  uint32_t expected_revision);
void tbs_toniebox2_pong(client_ctx_t *client_ctx, const char *request_id,
                        bool round_trip_ms_valid, uint32_t round_trip_ms);
void tbs_toniebox2_setup_status(client_ctx_t *client_ctx, const char *payload, bool truncated);
void tbs_toniebox2_metrics_events(client_ctx_t *client_ctx, const char *payload, bool truncated);
void tbs_toniebox2_metrics_fleet(client_ctx_t *client_ctx, const char *payload, bool truncated);
void tbs_toniebox2_alarm_reply(client_ctx_t *client_ctx, const char *payload, bool truncated);
void tbs_playback_stop(client_ctx_t *client_ctx);
void tbs_playback_file(client_ctx_t *client_ctx, char *filepath);
void tbs_playback_system_sound(client_ctx_t *client_ctx, toniebox_state_system_sound_lang_t language, toniebox_state_system_sound_t system_sound);

bool tbs_cmd_stop(uint8_t overlay_id);
bool tbs_cmd_set_vol_limit_spk(uint8_t overlay_id, uint32_t level);
bool tbs_cmd_set_vol_limit_hdp(uint8_t overlay_id, uint32_t level);
bool tbs_cmd_set_led(uint8_t overlay_id, uint32_t mode);
bool tbs_cmd_set_slap_enabled(uint8_t overlay_id, bool enabled);
bool tbs_cmd_set_slap_dir(uint8_t overlay_id, bool back_left);
