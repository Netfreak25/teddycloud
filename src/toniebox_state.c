#include "toniebox_state.h"
#include "settings.h"
#include "server_helpers.h"
#include <ctype.h>
#include <time.h>

static toniebox_state_t Box_State_Overlay[MAX_OVERLAYS];

static bool tbs_is_hex_ruid(const char *ruid)
{
    if (ruid == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < 16; i++)
    {
        char c = ruid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            return false;
        }
    }

    return ruid[16] == '\0';
}

const char *tbs_toniebox2_playback_status_name(toniebox_state_tb2_playback_status_t status)
{
    switch (status)
    {
    case TBS_TB2_PLAYBACK_STATUS_PLAYING:
        return "playing";
    case TBS_TB2_PLAYBACK_STATUS_PAUSED:
        return "paused";
    case TBS_TB2_PLAYBACK_STATUS_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

static void tbs_record_last_ruid(client_ctx_t *client_ctx, const char *ruid, bool refresh_same_ruid)
{
    if (client_ctx == NULL || client_ctx->settingsNoOverlay == NULL || !tbs_is_hex_ruid(ruid))
    {
        return;
    }

    char last_ruid[17];
    osStrncpy(last_ruid, ruid, sizeof(last_ruid) - 1);
    last_ruid[sizeof(last_ruid) - 1] = '\0';
    for (size_t i = 0; last_ruid[i] != '\0'; i++)
    {
        last_ruid[i] = tolower(last_ruid[i]);
    }

    if (refresh_same_ruid && osStrcmp(client_ctx->settingsNoOverlay->internal.last_ruid, last_ruid) == 0)
    {
        settings_set_unsigned_id("internal.last_ruid_time", time(NULL), client_ctx->settingsNoOverlay->internal.overlayNumber);
        return;
    }

    setLastRuid(last_ruid, client_ctx->settingsNoOverlay);
}

void toniebox_state_init()
{
    for (size_t i = 0; i < MAX_OVERLAYS; i++)
    {
        osMemset(&Box_State_Overlay[i], 0, sizeof(toniebox_state_t));
    }
}

toniebox_state_t *get_toniebox_state()
{
    return get_toniebox_state_id(0);
}
toniebox_state_t *get_toniebox_state_id(uint8_t id)
{
    return &Box_State_Overlay[id];
}

void tbs_tag_placed(client_ctx_t *client_ctx, uint64_t uid, bool valid)
{
    client_ctx->state->tag.uid = uid;
    client_ctx->state->tag.valid = valid;
    if (valid)
    {
        setLastUid(client_ctx->state->tag.uid, client_ctx->settingsNoOverlay);
    }

    char cuid[16 + 1];
    osSprintf((char *)cuid, "%016" PRIX64 "", (int64_t)uid);

    sse_sendEvent(valid ? "TagValid" : "TagInvalid", cuid, true);

    mqtt_sendBoxEvent(valid ? "TagValid" : "TagInvalid", cuid, client_ctx);
    mqtt_sendBoxEvent(!valid ? "TagValid" : "TagInvalid", "", client_ctx);
}

void tbs_tag_removed(client_ctx_t *client_ctx, uint64_t uid, bool valid)
{
    client_ctx->state->tag.uid = uid;
    client_ctx->state->tag.valid = valid;

    char cuid[16 + 2];
    osSprintf((char *)cuid, "-%016" PRIX64 "", (int64_t)uid);

    mqtt_sendBoxEvent(valid ? "TagValid" : "TagInvalid", cuid, client_ctx);
    mqtt_sendBoxEvent(!valid ? "TagValid" : "TagInvalid", "", client_ctx);
}

void tbs_knock(client_ctx_t *client_ctx, bool forward)
{
    sse_sendEvent("knock", forward ? "forward" : "backward", true);
    mqtt_sendBoxEvent(forward ? "KnockForward" : "KnockBackward", "{\"event_type\": \"triggered\"}", client_ctx);
}
void tbs_tilt(client_ctx_t *client_ctx, bool forward)
{
    sse_sendEvent("tilt", forward ? "forward" : "backward", true);
    mqtt_sendBoxEvent(forward ? "TiltForward" : "TiltBackward", "{\"event_type\": \"triggered\"}", client_ctx);
}

void tbs_playback(client_ctx_t *client_ctx, toniebox_state_playback_t playback)
{
    switch (playback)
    {
    case TBS_PLAYBACK_STARTING:
        sse_sendEvent("playback", "starting", true);
        mqtt_sendBoxEvent("Playback", "OFF", client_ctx);
    case TBS_PLAYBACK_STARTED:
        sse_sendEvent("playback", "started", true);
        mqtt_sendBoxEvent("Playback", "ON", client_ctx);
        break;
    case TBS_PLAYBACK_STOPPED:
        if (client_ctx->state->box.stream_ctx.stop_on_playback_stop && !client_ctx->state->box.stream_ctx.quit && client_ctx->state->tag.valid)
        {
            client_ctx->state->box.stream_ctx.active = false;
        }
        client_ctx->state->tag.audio_id = 0;
        client_ctx->state->tag.valid = false;
        client_ctx->state->tag.uid = 0;

        sse_sendEvent("playback", "stopped", true);
        mqtt_sendBoxEvent("Playback", "OFF", client_ctx);
        mqtt_sendBoxEvent("TagValid", "", client_ctx);
        mqtt_sendBoxEvent("ContentAudioId", "", client_ctx);
        mqtt_sendBoxEvent("ContentTitle", "", client_ctx);
        char *url = custom_asprintf("%s/img_empty.png", settings_get_string("core.host_url"));
        mqtt_sendBoxEvent("ContentPicture", url, client_ctx);
        osFreeMem(url);
        break;
    default:
        break;
    }
    mqtt_sendBoxEvent("TagInvalid", "", client_ctx);
}

static void tbs_send_toniebox2_playback_state_events(client_ctx_t *client_ctx, const toniebox_state_playback_state_t *playback_state)
{
    char value[32];

    mqtt_sendBoxEvent("PlaybackTonie", playback_state->valid ? playback_state->tonie : "", client_ctx);
    mqtt_sendBoxEvent("PlaybackStatus", tbs_toniebox2_playback_status_name(playback_state->status), client_ctx);
    mqtt_sendBoxEvent("PlaybackRuid", playback_state->ruid_valid ? playback_state->ruid : "", client_ctx);

    value[0] = '\0';
    if (playback_state->valid && playback_state->content_version_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu64, playback_state->contentVersion);
    }
    mqtt_sendBoxEvent("PlaybackContentVersion", value, client_ctx);

    value[0] = '\0';
    if (playback_state->valid && playback_state->chapter_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, playback_state->chapter);
    }
    mqtt_sendBoxEvent("PlaybackChapter", value, client_ctx);

    value[0] = '\0';
    if (playback_state->valid && playback_state->chapter_until_ms_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu64, playback_state->chapterUntilMs);
    }
    mqtt_sendBoxEvent("PlaybackChapterUntilMs", value, client_ctx);
    mqtt_sendBoxEvent("PlaybackChapterDuration", playback_state->valid && playback_state->chapter_duration_valid ? playback_state->chapterDuration : "", client_ctx);
}

void tbs_toniebox2_playback_state(client_ctx_t *client_ctx, const char *tonie,
                                  bool content_version_valid, uint64_t content_version,
                                  bool chapter_valid, uint32_t chapter,
                                  bool chapter_until_ms_valid, uint64_t chapter_until_ms,
                                  const char *chapter_duration)
{
    if (client_ctx == NULL || client_ctx->state == NULL)
    {
        return;
    }

    toniebox_state_playback_state_t *playback_state = &client_ctx->state->playback_state;
    bool was_playing = playback_state->valid || client_ctx->state->box.playback;
    bool previous_ruid_valid = playback_state->ruid_valid;
    char previous_ruid[TBS_TB2_CLAIM_RUID_MAX];
    osStrncpy(previous_ruid, playback_state->ruid, sizeof(previous_ruid) - 1);
    previous_ruid[sizeof(previous_ruid) - 1] = '\0';

    osMemset(playback_state, 0, sizeof(*playback_state));

    if (tonie == NULL || tonie[0] == '\0')
    {
        playback_state->status = TBS_TB2_PLAYBACK_STATUS_STOPPED;
        client_ctx->state->box.playback = false;
        if (was_playing)
        {
            tbs_playback(client_ctx, TBS_PLAYBACK_STOPPED);
        }
        tbs_send_toniebox2_playback_state_events(client_ctx, playback_state);
        return;
    }

    playback_state->valid = true;
    playback_state->status = TBS_TB2_PLAYBACK_STATUS_PLAYING;
    playback_state->updated_at = (uint32_t)time(NULL);
    osStrncpy(playback_state->tonie, tonie, sizeof(playback_state->tonie) - 1);
    playback_state->tonie[sizeof(playback_state->tonie) - 1] = '\0';
    tbs_record_last_ruid(client_ctx, playback_state->tonie, false);
    if (tbs_is_hex_ruid(playback_state->tonie))
    {
        playback_state->ruid_valid = true;
        osStrncpy(playback_state->ruid, playback_state->tonie, sizeof(playback_state->ruid) - 1);
        playback_state->ruid[sizeof(playback_state->ruid) - 1] = '\0';
    }
    else if (previous_ruid_valid)
    {
        playback_state->ruid_valid = true;
        osStrncpy(playback_state->ruid, previous_ruid, sizeof(playback_state->ruid) - 1);
        playback_state->ruid[sizeof(playback_state->ruid) - 1] = '\0';
    }
    playback_state->content_version_valid = content_version_valid;
    playback_state->contentVersion = content_version;
    playback_state->chapter_valid = chapter_valid;
    playback_state->chapter = chapter;
    playback_state->chapter_until_ms_valid = chapter_until_ms_valid;
    playback_state->chapterUntilMs = chapter_until_ms;
    if (chapter_duration != NULL && chapter_duration[0] != '\0')
    {
        playback_state->chapter_duration_valid = true;
        osStrncpy(playback_state->chapterDuration, chapter_duration, sizeof(playback_state->chapterDuration) - 1);
        playback_state->chapterDuration[sizeof(playback_state->chapterDuration) - 1] = '\0';
    }

    client_ctx->state->box.playback = true;
    if (!was_playing)
    {
        tbs_playback(client_ctx, TBS_PLAYBACK_STARTED);
    }
    tbs_send_toniebox2_playback_state_events(client_ctx, playback_state);
}

void tbs_toniebox2_playback_command_state(client_ctx_t *client_ctx,
                                          toniebox_state_tb2_playback_status_t status)
{
    if (client_ctx == NULL || client_ctx->state == NULL ||
        (status != TBS_TB2_PLAYBACK_STATUS_PLAYING && status != TBS_TB2_PLAYBACK_STATUS_PAUSED))
    {
        return;
    }

    toniebox_state_playback_state_t *playback_state = &client_ctx->state->playback_state;
    if (!playback_state->valid || playback_state->tonie[0] == '\0')
    {
        return;
    }

    playback_state->status = status;
    playback_state->updated_at = (uint32_t)time(NULL);
    mqtt_sendBoxEvent("PlaybackStatus", tbs_toniebox2_playback_status_name(status), client_ctx);
}

void tbs_toniebox2_claim(client_ctx_t *client_ctx, const char *ruid, const char *bd, bool bd_all_zero)
{
    if (client_ctx == NULL || client_ctx->state == NULL || ruid == NULL || bd == NULL)
    {
        return;
    }

    toniebox_state_claim_t *claim = &client_ctx->state->claim;
    osMemset(claim, 0, sizeof(*claim));
    claim->valid = true;
    claim->bd_all_zero = bd_all_zero;
    claim->updated_at = (uint32_t)time(NULL);
    osStrncpy(claim->ruid, ruid, sizeof(claim->ruid) - 1);
    claim->ruid[sizeof(claim->ruid) - 1] = '\0';
    osStrncpy(claim->bd, bd, sizeof(claim->bd) - 1);
    claim->bd[sizeof(claim->bd) - 1] = '\0';

    toniebox_state_playback_state_t *playback_state = &client_ctx->state->playback_state;
    playback_state->ruid_valid = true;
    osStrncpy(playback_state->ruid, claim->ruid, sizeof(playback_state->ruid) - 1);
    playback_state->ruid[sizeof(playback_state->ruid) - 1] = '\0';
    mqtt_sendBoxEvent("PlaybackRuid", playback_state->ruid, client_ctx);

    tbs_record_last_ruid(client_ctx, claim->ruid, true);
}

void tbs_toniebox2_bedtime_state_changed(client_ctx_t *client_ctx)
{
    if (client_ctx == NULL || client_ctx->state == NULL)
    {
        return;
    }

    toniebox_state_bedtime_t *bedtime = &client_ctx->state->bedtime;
    char value[32];

    mqtt_sendBoxEvent("BedtimeState", bedtime->valid ? bedtime->state : "", client_ctx);

    value[0] = '\0';
    if (bedtime->valid && bedtime->duration_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, bedtime->duration);
    }
    mqtt_sendBoxEvent("BedtimeDuration", value, client_ctx);

    value[0] = '\0';
    if (bedtime->valid && bedtime->default_duration_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, bedtime->defaultDuration);
    }
    mqtt_sendBoxEvent("BedtimeDefaultDuration", value, client_ctx);
    mqtt_sendBoxEvent("BedtimeUntil", bedtime->valid && bedtime->until_valid ? bedtime->until : "", client_ctx);
}

void tbs_toniebox2_battery_metrics(client_ctx_t *client_ctx,
                                   bool percent_valid, uint32_t percent,
                                   bool raw_valid, int32_t raw,
                                   bool current_valid, int32_t current,
                                   const char *status)
{
    if (client_ctx == NULL || client_ctx->state == NULL)
    {
        return;
    }

    toniebox_state_battery_t *battery = &client_ctx->state->battery;
    osMemset(battery, 0, sizeof(*battery));
    battery->valid = true;
    battery->updated_at = (uint32_t)time(NULL);
    battery->percent_valid = percent_valid;
    battery->percent = percent;
    battery->raw_valid = raw_valid;
    battery->raw = raw;
    battery->current_valid = current_valid;
    battery->current = current;
    if (status != NULL && status[0] != '\0')
    {
        battery->status_valid = true;
        osStrncpy(battery->status, status, sizeof(battery->status) - 1);
        battery->status[sizeof(battery->status) - 1] = '\0';
    }

    char value[32];
    value[0] = '\0';
    if (battery->percent_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, battery->percent);
    }
    mqtt_sendBoxEvent("BatteryPercent", value, client_ctx);

    value[0] = '\0';
    if (battery->raw_valid)
    {
        osSnprintf(value, sizeof(value), "%d", (int)battery->raw);
    }
    mqtt_sendBoxEvent("BatteryRaw", value, client_ctx);

    value[0] = '\0';
    if (battery->current_valid)
    {
        osSnprintf(value, sizeof(value), "%d", (int)battery->current);
    }
    mqtt_sendBoxEvent("BatteryCurrent", value, client_ctx);
    mqtt_sendBoxEvent("BatteryStatus", battery->status_valid ? battery->status : "", client_ctx);
}

void tbs_toniebox2_headphones_metrics(client_ctx_t *client_ctx,
                                      bool speaker_output_valid, bool speaker_output,
                                      bool connected_valid, uint32_t connected_count,
                                      const char *connected_json)
{
    if (client_ctx == NULL || client_ctx->state == NULL)
    {
        return;
    }

    toniebox_state_headphones_t *headphones = &client_ctx->state->headphones;
    osMemset(headphones, 0, sizeof(*headphones));
    headphones->valid = true;
    headphones->updated_at = (uint32_t)time(NULL);
    headphones->speaker_output_valid = speaker_output_valid;
    headphones->speaker_output = speaker_output;
    headphones->connected_valid = connected_valid;
    headphones->connected_count = connected_count;
    if (connected_valid)
    {
        osStrncpy(headphones->connected,
                  connected_json != NULL && connected_json[0] != '\0' ? connected_json : "[]",
                  sizeof(headphones->connected) - 1);
        headphones->connected[sizeof(headphones->connected) - 1] = '\0';
    }

    char value[32];
    mqtt_sendBoxEvent("SpeakerOutput",
                      headphones->speaker_output_valid ? (headphones->speaker_output ? "ON" : "OFF") : "",
                      client_ctx);
    mqtt_sendBoxEvent("HeadphonesConnected",
                      headphones->connected_valid ? (headphones->connected_count > 0 ? "ON" : "OFF") : "",
                      client_ctx);

    value[0] = '\0';
    if (headphones->connected_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, headphones->connected_count);
    }
    mqtt_sendBoxEvent("HeadphonesConnectedCount", value, client_ctx);
    mqtt_sendBoxEvent("HeadphonesConnectedDevices",
                      headphones->connected_valid ? headphones->connected : "",
                      client_ctx);
}

void tbs_toniebox2_volume_state(client_ctx_t *client_ctx, uint32_t level)
{
    if (client_ctx == NULL || client_ctx->state == NULL || level > TBS_TB2_VOLUME_LEVEL_MAX)
    {
        return;
    }

    toniebox_state_volume_t *volume = &client_ctx->state->volume;
    volume->valid = true;
    volume->level = level;
    volume->updated_at = (uint32_t)time(NULL);

    char value[16];
    osSnprintf(value, sizeof(value), "%" PRIu32, level);
    mqtt_sendBoxEvent("VolumeLevel", value, client_ctx);
}

void tbs_toniebox2_pong(client_ctx_t *client_ctx, const char *request_id,
                        bool round_trip_ms_valid, uint32_t round_trip_ms)
{
    if (client_ctx == NULL || client_ctx->state == NULL || request_id == NULL || request_id[0] == '\0')
    {
        return;
    }

    toniebox_state_pong_t *pong = &client_ctx->state->pong;
    osMemset(pong, 0, sizeof(*pong));
    pong->valid = true;
    pong->updated_at = (uint32_t)time(NULL);
    pong->round_trip_ms_valid = round_trip_ms_valid;
    pong->round_trip_ms = round_trip_ms;
    osStrncpy(pong->request_id, request_id, sizeof(pong->request_id) - 1);
    pong->request_id[sizeof(pong->request_id) - 1] = '\0';

    mqtt_sendBoxEvent("PongRequestId", pong->request_id, client_ctx);
    char value[16] = "";
    if (pong->round_trip_ms_valid)
    {
        osSnprintf(value, sizeof(value), "%" PRIu32, pong->round_trip_ms);
    }
    mqtt_sendBoxEvent("PongRoundTripMs", value, client_ctx);
}

static void tbs_toniebox2_json_snapshot(client_ctx_t *client_ctx, toniebox_state_json_snapshot_t *snapshot,
                                        const char *payload, bool truncated, const char *event_name)
{
    if (client_ctx == NULL || client_ctx->state == NULL || snapshot == NULL || payload == NULL)
    {
        return;
    }

    osMemset(snapshot, 0, sizeof(*snapshot));
    snapshot->valid = true;
    snapshot->truncated = truncated;
    snapshot->updated_at = (uint32_t)time(NULL);
    osStrncpy(snapshot->payload, payload, sizeof(snapshot->payload) - 1);
    snapshot->payload[sizeof(snapshot->payload) - 1] = '\0';
    mqtt_sendBoxEvent(event_name, snapshot->payload, client_ctx);
}

void tbs_toniebox2_setup_status(client_ctx_t *client_ctx, const char *payload, bool truncated)
{
    if (client_ctx != NULL && client_ctx->state != NULL)
    {
        tbs_toniebox2_json_snapshot(client_ctx, &client_ctx->state->setup_status, payload, truncated, "SetupStatus");
    }
}

void tbs_toniebox2_metrics_events(client_ctx_t *client_ctx, const char *payload, bool truncated)
{
    if (client_ctx != NULL && client_ctx->state != NULL)
    {
        tbs_toniebox2_json_snapshot(client_ctx, &client_ctx->state->metrics_events, payload, truncated, "MetricsEvents");
    }
}

void tbs_toniebox2_metrics_fleet(client_ctx_t *client_ctx, const char *payload, bool truncated)
{
    if (client_ctx != NULL && client_ctx->state != NULL)
    {
        tbs_toniebox2_json_snapshot(client_ctx, &client_ctx->state->metrics_fleet, payload, truncated, "MetricsFleet");
    }
}

void tbs_toniebox2_alarm_reply(client_ctx_t *client_ctx, const char *payload, bool truncated)
{
    if (client_ctx != NULL && client_ctx->state != NULL)
    {
        tbs_toniebox2_json_snapshot(client_ctx, &client_ctx->state->alarm_reply, payload, truncated, "AlarmReply");
    }
}

void tbs_playback_file(client_ctx_t *client_ctx, char *filepath)
{
    // filepath: content/00000000/00000012
    // Get the first part ("content")
    char *content = strtok(filepath, "/");
    if (content == NULL)
    {
        return;
    }
    // Get the second part ("00000000")
    char *dir = strtok(NULL, "/");
    if (dir == NULL || strlen(dir) != 8)
    {
        return;
    }
    // Get the third part ("00000012")
    char *file = strtok(NULL, "/");
    if (file == NULL || strlen(file) != 8)
    {
        return;
    }

    if (strncmp(dir, "0000000", 7) == 0)
    {
        if (strncmp(file, "000000", 6) == 0)
        {
            toniebox_state_system_sound_t language = (toniebox_state_system_sound_lang_t)strtol(dir, NULL, 16);
            toniebox_state_system_sound_t sound = (toniebox_state_system_sound_t)strtol(file, NULL, 16);
            tbs_playback_system_sound(client_ctx, language, sound);
        }
    }
}
void tbs_playback_system_sound(client_ctx_t *client_ctx, toniebox_state_system_sound_lang_t language, toniebox_state_system_sound_t system_sound)
{
    tbs_playback(client_ctx, TBS_PLAYBACK_STOPPED);
}

bool tbs_cmd_stop(uint8_t overlay_id)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS)
    {
        return false;
    }
    toniebox_state_t *state = get_toniebox_state_id(overlay_id);
    if (state->box.stream_ctx.active && !state->box.stream_ctx.quit)
    {
        TRACE_INFO("Stopping active stream on overlay %" PRIu8 "\r\n", overlay_id);
        state->box.stream_ctx.active = false;
        sse_sendEvent("cmd", "stop", true);
        return true;
    }
    TRACE_INFO("No active stream to stop on overlay %" PRIu8 "\r\n", overlay_id);
    return false;
}

bool tbs_cmd_set_vol_limit_spk(uint8_t overlay_id, uint32_t level)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS || level > 3)
    {
        return false;
    }
    settings_t *settings = get_settings_id(overlay_id);
    if (!settings->internal.config_used)
    {
        return false;
    }
    TRACE_INFO("Setting speaker volume limit to %" PRIu32 " on overlay %" PRIu8 "\r\n", level, overlay_id);
    settings_set_unsigned_id("toniebox.max_vol_spk", level, overlay_id);
    sse_sendEvent("cmd", "vol_limit_spk", true);
    return true;
}

bool tbs_cmd_set_vol_limit_hdp(uint8_t overlay_id, uint32_t level)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS || level > 3)
    {
        return false;
    }
    settings_t *settings = get_settings_id(overlay_id);
    if (!settings->internal.config_used)
    {
        return false;
    }
    TRACE_INFO("Setting headphone volume limit to %" PRIu32 " on overlay %" PRIu8 "\r\n", level, overlay_id);
    settings_set_unsigned_id("toniebox.max_vol_hdp", level, overlay_id);
    sse_sendEvent("cmd", "vol_limit_hdp", true);
    return true;
}

bool tbs_cmd_set_led(uint8_t overlay_id, uint32_t mode)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS || mode > 2)
    {
        return false;
    }
    settings_t *settings = get_settings_id(overlay_id);
    if (!settings->internal.config_used)
    {
        return false;
    }
    TRACE_INFO("Setting LED mode to %" PRIu32 " on overlay %" PRIu8 "\r\n", mode, overlay_id);
    settings_set_unsigned_id("toniebox.led", mode, overlay_id);
    sse_sendEvent("cmd", "led", true);
    return true;
}

bool tbs_cmd_set_slap_enabled(uint8_t overlay_id, bool enabled)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS)
    {
        return false;
    }
    settings_t *settings = get_settings_id(overlay_id);
    if (!settings->internal.config_used)
    {
        return false;
    }
    TRACE_INFO("Setting slap enabled to %s on overlay %" PRIu8 "\r\n", enabled ? "true" : "false", overlay_id);
    settings_set_bool_id("toniebox.slap_enabled", enabled, overlay_id);
    sse_sendEvent("cmd", "slap_enabled", true);
    return true;
}

bool tbs_cmd_set_slap_dir(uint8_t overlay_id, bool back_left)
{
    if (overlay_id == 0 || overlay_id >= MAX_OVERLAYS)
    {
        return false;
    }
    settings_t *settings = get_settings_id(overlay_id);
    if (!settings->internal.config_used)
    {
        return false;
    }
    TRACE_INFO("Setting slap direction to %s on overlay %" PRIu8 "\r\n", back_left ? "back-left" : "forw-left", overlay_id);
    settings_set_bool_id("toniebox.slap_back_left", back_left, overlay_id);
    sse_sendEvent("cmd", "slap_dir", true);
    return true;
}
