#pragma once

#include "toniefile.h"

#define TBS_TB2_PLAYBACK_TONIE_MAX 64
#define TBS_TB2_PLAYBACK_DURATION_MAX 32
#define TBS_TB2_CLAIM_RUID_MAX 17
#define TBS_TB2_CLAIM_BD_MAX 65
#define TBS_TB2_BATTERY_STATUS_MAX 32
#define TBS_TB2_HEADPHONES_CONNECTED_MAX 128
#define TBS_TB2_REQUEST_ID_MAX 64
#define TBS_TB2_RUNTIME_JSON_MAX 512
#define TBS_TB2_VOLUME_LEVEL_MIN 1
#define TBS_TB2_VOLUME_LEVEL_MAX 12

typedef enum
{
    TBS_SYS_SOUND_STARTUP_JINGLE = 0x00000000,
    TBS_SYS_SOUND_TADA = 0x00000001,
    TBS_SYS_SOUND_TADUM_MHHH = 0x00000002,
    TBS_SYS_SOUND_LOW_BATTERY_WARNING = 0x00000003,
    TBS_SYS_SOUND_TONIE_PLAYBACK_FINISHED = 0x00000006,
    TBS_SYS_SOUND_OFFLINE_MODE_ON = 0x00000007,
    TBS_SYS_SOUND_OFFLINE_MODE_OFF = 0x00000008,
    TBS_SYS_SOUND_LOW_BATTERY_SHUTDOWN = 0x00000009,
    TBS_SYS_SOUND_OFFLINE_EC_KOALA = 0x0000000A,
    TBS_SYS_SOUND_INSTALL_HINT = 0x0000000B,
    TBS_SYS_SOUND_KEEP_ON_CHARGER = 0x0000000D,
    TBS_SYS_SOUND_PLAYBACK_LIMIT = 0x0000000E,
    TBS_SYS_SOUND_DOWNLOAD_INTERRUPTED = 0x0000000F,
    TBS_SYS_SOUND_INSTALL_SUCCESS = 0x00000010,
    TBS_SYS_SOUND_NO_INTERNET_EC_TURTLE = 0x00000011,
    TBS_SYS_SOUND_NO_CONTENT_EC_GROUNDHOG = 0x00000012,
    TBS_SYS_SOUND_WIFI_PW_WRONG = 0x00000013,
    TBS_SYS_SOUND_CONNECTION_EC_HEDGEHOG = 0x00000014,
    TBS_SYS_SOUND_CONNECTION_EC_ANT = 0x00000015,
    TBS_SYS_SOUND_CONNECTION_EC_MEERKAT = 0x00000016,
    TBS_SYS_SOUND_CONNECTION_EC_OWL = 0x00000017,
    TBS_SYS_SOUND_WIFI_EC_ELEPHANT = 0x00000018,
} toniebox_state_system_sound_t;

typedef enum
{
    TBS_SYS_SOUND_LANG_EN_GB = 0x00000000,
    TBS_SYS_SOUND_LANG_DE_DE = 0x00000001,
    TBS_SYS_SOUND_LANG_EN_US = 0x00000002,
    TBS_SYS_SOUND_LANG_FR_FR = 0x00000003,
} toniebox_state_system_sound_lang_t;

typedef struct
{
    const char *id;
    const char *name;
    bool playback;
    bool charger;
    uint32_t volumeLevel;
    uint32_t volumedB;
    stream_ctx_t stream_ctx;
} toniebox_state_box_t;

typedef struct
{
    uint64_t uid;
    bool valid;
    uint32_t audio_id;
    bool custom;
} toniebox_state_tag_t;

typedef struct
{
    bool valid;
    char state[32];
    bool duration_valid;
    uint32_t duration;
    bool default_duration_valid;
    uint32_t defaultDuration;
    bool until_valid;
    char until[64];
    uint32_t updated_at;
} toniebox_state_bedtime_t;

typedef enum
{
    TBS_TB2_PLAYBACK_STATUS_UNKNOWN,
    TBS_TB2_PLAYBACK_STATUS_PLAYING,
    TBS_TB2_PLAYBACK_STATUS_PAUSED,
    TBS_TB2_PLAYBACK_STATUS_STOPPED,
} toniebox_state_tb2_playback_status_t;

typedef struct
{
    bool valid;
    toniebox_state_tb2_playback_status_t status;
    char tonie[TBS_TB2_PLAYBACK_TONIE_MAX];
    bool ruid_valid;
    char ruid[TBS_TB2_CLAIM_RUID_MAX];
    bool content_version_valid;
    uint64_t contentVersion;
    bool chapter_valid;
    uint32_t chapter;
    bool chapter_until_ms_valid;
    uint64_t chapterUntilMs;
    bool chapter_duration_valid;
    char chapterDuration[TBS_TB2_PLAYBACK_DURATION_MAX];
    uint32_t updated_at;
} toniebox_state_playback_state_t;

typedef struct
{
    bool valid;
    char ruid[TBS_TB2_CLAIM_RUID_MAX];
    char bd[TBS_TB2_CLAIM_BD_MAX];
    bool bd_all_zero;
    uint32_t updated_at;
} toniebox_state_claim_t;

typedef struct
{
    bool valid;
    bool percent_valid;
    uint32_t percent;
    bool raw_valid;
    int32_t raw;
    bool current_valid;
    int32_t current;
    bool status_valid;
    char status[TBS_TB2_BATTERY_STATUS_MAX];
    uint32_t updated_at;
} toniebox_state_battery_t;

typedef struct
{
    bool valid;
    bool speaker_output_valid;
    bool speaker_output;
    bool connected_valid;
    uint32_t connected_count;
    char connected[TBS_TB2_HEADPHONES_CONNECTED_MAX];
    uint32_t updated_at;
} toniebox_state_headphones_t;

typedef struct
{
    bool valid;
    uint32_t level;
    uint32_t updated_at;
} toniebox_state_volume_t;

typedef struct
{
    bool valid;
    char request_id[TBS_TB2_REQUEST_ID_MAX];
    bool round_trip_ms_valid;
    uint32_t round_trip_ms;
    uint32_t updated_at;
} toniebox_state_pong_t;

typedef struct
{
    bool valid;
    bool truncated;
    char payload[TBS_TB2_RUNTIME_JSON_MAX];
    uint32_t updated_at;
} toniebox_state_json_snapshot_t;

typedef struct
{
    toniebox_state_box_t box;
    toniebox_state_tag_t tag;
    toniebox_state_bedtime_t bedtime;
    toniebox_state_playback_state_t playback_state;
    toniebox_state_claim_t claim;
    toniebox_state_battery_t battery;
    toniebox_state_headphones_t headphones;
    toniebox_state_volume_t volume;
    toniebox_state_pong_t pong;
    toniebox_state_json_snapshot_t setup_status;
    toniebox_state_json_snapshot_t metrics_events;
    toniebox_state_json_snapshot_t metrics_fleet;
    toniebox_state_json_snapshot_t alarm_reply;
} toniebox_state_t;

typedef enum
{
    TBS_PLAYBACK_NONE,
    TBS_PLAYBACK_STARTING,
    TBS_PLAYBACK_STARTED,
    TBS_PLAYBACK_STOPPED,
} toniebox_state_playback_t;
