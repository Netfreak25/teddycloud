#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <byteswap.h>

#include "core/net.h"
#include "core/socket.h"
#include "error.h"
#include "debug.h"
#include "settings.h"
#include "mqtt_server.h"
#include "tls.h"
#include "rand.h"
#include "tls_adapter.h"
#include "encoding/base64.h"
#include "cJSON.h"
#include "handler.h"
#include "tb2_mqtt_passthrough.h"
#include "toniebox_state.h"

// Forward declaration of platform-specific function
uint_t tcpWaitForEvents(Socket *socket, uint_t eventMask, systime_t timeout);

#define MQTT_MAX_PACKET_SIZE 4096
#define MQTT_MAX_CONNECTIONS 32
#define MQTT_MAX_SUBSCRIPTIONS 32
#define MQTT_FRESH_TONIES_MAX 50
#define MQTT_SETTINGS_DESIRED_PAYLOAD_SIZE 2048
#define MQTT_SETTINGS_DESIRED_MAX_ATTEMPTS 3
#define MQTT_SETTINGS_DESIRED_RETRY_INTERVAL_SEC 5
#define MQTT_APP_CONTROL_REPLY_WINDOW_SEC 30
#define MQTT_MILLISECONDS_PER_SECOND 1000ULL
#define MQTT_JSON_SAFE_INTEGER_MAX 9007199254740991.0
#define MQTT_LOG_INLINE_PAYLOAD_SIZE 256
#define MQTT_FRESH_TONIES_DEBOUNCE_SEC 2
#define MQTT_FRESH_TONIES_REASON_MAX 32
#define MQTT_FRESH_TONIES_DUPLICATE_LOG_INTERVAL_SEC 60

typedef struct {
    char topic[256];
    uint8_t qos;
} MqttSubscription;

typedef struct {
    Socket *socket;
    TlsContext *tlsContext;
    bool active;
    uint8_t buffer[MQTT_MAX_PACKET_SIZE];
    size_t buffer_len;
    bool_t mode_decided;
    client_ctx_t client_ctx;
    bool_t box_connection;
    uint8_t box_overlay_id;
    char box_common_name[32];
    tb2_mqtt_passthrough_session_t *passthrough;
    MqttSubscription subscriptions[MQTT_MAX_SUBSCRIPTIONS];
    size_t subscription_count;
} MqttClientConnection;

typedef struct {
    bool_t valid;
    uint32_t sent_at;
    uint32_t sequence;
    uint32_t payload_hash;
} MqttAppControlStlState;

typedef struct {
    bool_t valid;
    uint32_t sequence;
    systime_t sent_at;
    char request_id[TBS_TB2_REQUEST_ID_MAX];
} MqttAppControlPingState;

typedef struct {
    bool_t pending;
    uint32_t pending_since;
    uint32_t last_publish_at;
    uint32_t last_delay_log_at;
    uint32_t last_duplicate_log_at;
    uint32_t coalesced_count;
    bool_t last_payload_valid;
    uint32_t last_payload_hash;
    size_t last_payload_len;
    char reason[MQTT_FRESH_TONIES_REASON_MAX];
} MqttFreshToniesPublishState;

typedef enum {
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
} MqttToniebox2SettingId;

typedef struct {
    const char *json_name;
    const char *setting_name;
} MqttToniebox2SettingDescriptor;

typedef enum {
    MQTT_MSG_ANY = 0,
    MQTT_MSG_CONNECT = 0x10,
    MQTT_MSG_PUBLISH = 0x30,
    MQTT_MSG_SUBSCRIBE = 0x80,
    MQTT_MSG_PINGREQ = 0xC0,
    MQTT_MSG_DISCONNECT = 0xE0
} MqttMessageType;

typedef struct {
    MqttMessageType type;
    const char *topic;
    error_t (*handler)(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len);
} MqttHandlerEntry;

static Socket *serverSocket = NULL;
static MqttClientConnection connections[MQTT_MAX_CONNECTIONS];
static MqttAppControlStlState app_control_stl_state[MAX_OVERLAYS];
static MqttAppControlPingState app_control_ping_state[MAX_OVERLAYS];
static MqttFreshToniesPublishState fresh_tonies_publish_state[MAX_OVERLAYS];

static bool_t mqtt_publish_fresh_tonies_to_connection(MqttClientConnection *conn, settings_t *settings, bool_t finish_on_success, bool_t allow_duplicate);
static MqttFreshToniesPublishState *mqtt_fresh_tonies_publish_state(settings_t *settings);
static void mqtt_mark_fresh_tonies_pending(settings_t *settings, const char *reason);
static bool_t mqtt_fresh_tonies_background_payload_already_sent(settings_t *settings);
static const MqttToniebox2SettingDescriptor toniebox2_settings_descriptors[MQTT_TB2_SETTING_COUNT] = {
    [MQTT_TB2_SETTING_MAX_VOLUME] = {"max_volume", "toniebox2.max_volume"},
    [MQTT_TB2_SETTING_BEDTIME_MAX_VOLUME] = {"bedtime_max_volume", "toniebox2.bedtime_max_volume"},
    [MQTT_TB2_SETTING_MAX_HEADPHONE_VOLUME] = {"max_headphone_volume", "toniebox2.max_headphone_volume"},
    [MQTT_TB2_SETTING_BEDTIME_MAX_HEADPHONE_VOLUME] = {"bedtime_max_headphone_volume", "toniebox2.bedtime_max_headphone_volume"},
    [MQTT_TB2_SETTING_LIGHTRING_BRIGHTNESS] = {"lightring_brightness", "toniebox2.lightring_brightness"},
    [MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS] = {"bedtime_lightring_brightness", "toniebox2.bedtime_lightring_brightness"},
    [MQTT_TB2_SETTING_SCRUBBING_ENABLED] = {"scrubbing_enabled", "toniebox2.scrubbing_enabled"},
    [MQTT_TB2_SETTING_SKIPPING_ENABLED] = {"skipping_enabled", "toniebox2.slap_enabled"},
    [MQTT_TB2_SETTING_SKIPPING_DIRECTION] = {"skipping_direction", "toniebox2.slap_back_left"},
    [MQTT_TB2_SETTING_AGE_MODE] = {"age_mode", "toniebox2.baby_mode"},
};

static int mqtt_connection_slot(MqttClientConnection *conn)
{
    if (conn == NULL)
    {
        return -1;
    }
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        if (conn == &connections[i])
        {
            return (int)i;
        }
    }

    return -1;
}

static const char *mqtt_connection_common_name(MqttClientConnection *conn)
{
    if (conn == NULL)
    {
        return "-";
    }
    if (conn->box_common_name[0] != '\0')
    {
        return conn->box_common_name;
    }
    if (conn->client_ctx.settings != NULL && conn->client_ctx.settings->commonName != NULL &&
        conn->client_ctx.settings->commonName[0] != '\0')
    {
        return conn->client_ctx.settings->commonName;
    }

    return "-";
}

static uint8_t mqtt_connection_overlay_id(MqttClientConnection *conn)
{
    if (conn == NULL)
    {
        return 0;
    }
    if (conn->box_overlay_id != 0)
    {
        return conn->box_overlay_id;
    }
    if (conn->client_ctx.settings != NULL)
    {
        return conn->client_ctx.settings->internal.overlayNumber;
    }

    return 0;
}

static bool_t mqtt_connection_has_trusted_client_cert(MqttClientConnection *conn)
{
    if (conn == NULL || conn->tlsContext == NULL)
    {
        return FALSE;
    }

    char_t *subject = conn->tlsContext->client_cert_subject;
    char_t *issuer = conn->tlsContext->client_cert_issuer;
    return osStrlen(issuer) > 0 &&
           (osStrstr(issuer, "Boxine Factory SubCA") != NULL ||
            osStrstr(issuer, "Toniebox SubCA") != NULL ||
            osStrstr(issuer, "TeddyCloud") != NULL ||
            osStrstr(subject, "TeddyCloud") != NULL ||
            osStrstr(issuer, "Toniebox Root CA") != NULL);
}

static bool_t mqtt_promote_connection_to_box(MqttClientConnection *conn, settings_t *box_settings,
                                             const char *common_name, const char *source)
{
    if (conn == NULL || box_settings == NULL || !box_settings->internal.config_used ||
        common_name == NULL || common_name[0] == '\0')
    {
        return FALSE;
    }

    if (conn->box_common_name[0] != '\0' && osStrcmp(conn->box_common_name, common_name) != 0)
    {
        TRACE_WARNING("MQTT connection topic/cert mismatch: existing box=%s new box=%s source=%s\r\n",
                      conn->box_common_name,
                      common_name,
                      source != NULL ? source : "-");
        return FALSE;
    }

    bool_t was_box_connection = conn->box_connection;
    conn->client_ctx.settings = box_settings;
    conn->client_ctx.settingsNoOverlay = box_settings;
    conn->client_ctx.state = get_toniebox_state_id(box_settings->internal.overlayNumber);
    conn->client_ctx.state->box.id = conn->client_ctx.settings->commonName;
    conn->client_ctx.state->box.name = conn->client_ctx.settings->boxName;
    conn->box_connection = TRUE;
    conn->box_overlay_id = box_settings->internal.overlayNumber;
    osStrncpy(conn->box_common_name, common_name, sizeof(conn->box_common_name) - 1);
    conn->box_common_name[sizeof(conn->box_common_name) - 1] = '\0';

    if (!was_box_connection)
    {
        TRACE_INFO("MQTT connection mapped to box %s overlay=%u via %s\r\n",
                   conn->box_common_name,
                   (unsigned)conn->box_overlay_id,
                   source != NULL ? source : "-");
    }
    return TRUE;
}

static void mqtt_connection_close(MqttClientConnection *conn, const char *reason)
{
    if (conn == NULL)
    {
        return;
    }

    if (reason != NULL && reason[0] != '\0')
    {
        TRACE_INFO("MQTT connection closed: slot=%d box=%s overlay=%u reason=%s\r\n",
                   mqtt_connection_slot(conn),
                   mqtt_connection_common_name(conn),
                   (unsigned)mqtt_connection_overlay_id(conn),
                   reason);
    }

    if (conn->passthrough != NULL)
    {
        tb2_mqtt_passthrough_close(conn->passthrough, "connection_closed", FALSE);
        conn->passthrough = NULL;
    }

    if (conn->tlsContext != NULL)
    {
        tlsFree(conn->tlsContext);
    }
    if (conn->socket != NULL)
    {
        socketClose(conn->socket);
    }

    conn->active = false;
    conn->tlsContext = NULL;
    conn->socket = NULL;
    conn->client_ctx.mqtt_connection = NULL;
    conn->buffer_len = 0;
    conn->mode_decided = FALSE;
    conn->subscription_count = 0;
    osMemset(conn->buffer, 0, sizeof(conn->buffer));
    osMemset(conn->subscriptions, 0, sizeof(conn->subscriptions));
    conn->box_connection = FALSE;
    conn->box_overlay_id = 0;
    conn->box_common_name[0] = '\0';
}

static bool mqtt_topic_match(const char *filter, const char *topic)
{
    while (*filter && *topic)
    {
        if (*filter == '+')
        {
            filter++;
            while (*topic && *topic != '/')
            {
                topic++;
            }
        }
        else if (*filter == '#')
        {
            return true;
        }
        else if (*filter == *topic)
        {
            filter++;
            topic++;
        }
        else
        {
            return false;
        }
    }
    
    if (*filter == '\0' && *topic == '\0')
    {
        return true;
    }
    if (*filter == '#' && *(filter + 1) == '\0')
    {
        return true;
    }
    if (*filter == '/' && *(filter + 1) == '#' && *(filter + 2) == '\0' && *topic == '\0')
    {
        return true;
    }
    return false;
}

static void mqtt_connection_update_context(MqttClientConnection *conn, const char *topic)
{
    if (conn == NULL || topic == NULL || strncmp(topic, "toniebox/", 9) != 0)
    {
        return;
    }

    const char *mac_start = topic + 9;
    const char *mac_end = strchr(mac_start, '/');
    if (mac_end != NULL)
    {
        size_t mac_len = mac_end - mac_start;
        if (mac_len > 0 && mac_len < 32)
        {
            char mac[32];
            memcpy(mac, mac_start, mac_len);
            mac[mac_len] = '\0';

            if (get_overlay_id(mac) > 0)
            {
                settings_t *box_settings = get_settings_cn(mac);
                if (box_settings != NULL && box_settings->internal.config_used)
                {
                    if (conn->box_connection || mqtt_connection_has_trusted_client_cert(conn))
                    {
                        mqtt_promote_connection_to_box(conn, box_settings, mac, conn->box_connection ? "topic" : "trusted-topic");
                    }
                    else
                    {
                        conn->client_ctx.settings = box_settings;
                        conn->client_ctx.settingsNoOverlay = box_settings;
                        conn->client_ctx.state = get_toniebox_state_id(box_settings->internal.overlayNumber);
                        conn->client_ctx.state->box.id = conn->client_ctx.settings->commonName;
                        conn->client_ctx.state->box.name = conn->client_ctx.settings->boxName;
                    }
                }
            }
        }
    }
}

static char_t *mqtt_server_cert = NULL;
static size_t mqtt_server_cert_len = 0;
static char_t *mqtt_server_key = NULL;
static size_t mqtt_server_key_len = 0;

static char_t *mqtt_server_read_file(const char_t *filename, size_t *length)
{
    *length = 0;
    uint32_t fileSize = 0;
    error_t error = fsGetFileSize(filename, &fileSize);
    if (error != NO_ERROR)
        return NULL;

    char_t *buffer = osAllocMem(fileSize + 1);
    if (buffer == NULL)
        return NULL;

    FsFile *fp = fsOpenFile(filename, FS_FILE_MODE_READ);
    if (fp == NULL)
    {
        osFreeMem(buffer);
        return NULL;
    }

    size_t read = 0;
    error = fsReadFile(fp, buffer, fileSize, &read);
    fsCloseFile(fp);

    if (error != NO_ERROR || read != fileSize)
    {
        osFreeMem(buffer);
        return NULL;
    }

    buffer[fileSize] = '\0';
    *length = fileSize;
    return buffer;
}

error_t mqtt_server_tls_init(TlsContext *tlsContext)
{
    error_t error;

    error = tlsSetConnectionEnd(tlsContext, TLS_CONNECTION_END_SERVER);
    if (error)
        return error;

    error = tlsSetBufferSize(tlsContext, TLS_TX_BUFFER_SIZE, TLS_RX_BUFFER_SIZE);
    if (error)
        return error;

    error = tlsSetPrng(tlsContext, rand_get_algo(), rand_get_context());
    if (error)
        return error;

    error = tlsSetCache(tlsContext, tlsCache);
    if (error)
        return error;

    error = tlsEnableSecureRenegotiation(tlsContext, TRUE);
    if (error)
        return error;

    error = tlsSetClientAuthMode(tlsContext, TLS_CLIENT_AUTH_OPTIONAL);
    if (error)
        return error;

    if (mqtt_server_cert && mqtt_server_key)
    {
        error = tlsLoadCertificate(tlsContext, 0, mqtt_server_cert, mqtt_server_cert_len, mqtt_server_key, mqtt_server_key_len, NULL);
    }
    else
    {
        TRACE_ERROR("Certificates not loaded\r\n");
        error = ERROR_FAILURE;
    }

    return error;
}

void mqtt_server_init() {
    settings_t *settings = get_settings();
    if (!settings->mqtt_server.enabled) return;

    TRACE_INFO("Initializing on port %u\r\n", settings->mqtt_server.port);

    // Load certificates once
    char *cert_path = osAllocMem(256);
    char *key_path = osAllocMem(256);

    settings_resolve_dir(&cert_path, settings->mqtt_server.cert_crt, settings->internal.basedirfull);
    settings_resolve_dir(&key_path, settings->mqtt_server.cert_key, settings->internal.basedirfull);

    mqtt_server_cert = mqtt_server_read_file(cert_path, &mqtt_server_cert_len);
    mqtt_server_key = mqtt_server_read_file(key_path, &mqtt_server_key_len);

    if (!mqtt_server_cert || !mqtt_server_key)
    {
        TRACE_ERROR("Failed to load certificates (cert: %s, key: %s)\r\n", cert_path, key_path);
    }

    osFreeMem(cert_path);
    osFreeMem(key_path);

    serverSocket = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
    if (serverSocket == NULL) {
        TRACE_ERROR("Failed to open socket\r\n");
        return;
    }

    socketSetTimeout(serverSocket, 0); // Non-blocking

    error_t error = socketBind(serverSocket, &IP_ADDR_ANY, settings->mqtt_server.port);
    if (error != NO_ERROR) {
        TRACE_ERROR("Failed to bind socket (port %u)\r\n", settings->mqtt_server.port);
        socketClose(serverSocket);
        serverSocket = NULL;
        return;
    }

    error = socketListen(serverSocket, 5);
    if (error != NO_ERROR) {
        TRACE_ERROR("Failed to listen on socket\r\n");
        socketClose(serverSocket);
        serverSocket = NULL;
        return;
    }

    osMemset(connections, 0, sizeof(connections));
    osMemset(app_control_stl_state, 0, sizeof(app_control_stl_state));
    osMemset(app_control_ping_state, 0, sizeof(app_control_ping_state));
}

static const char *mqtt_json_bool(bool value)
{
    return value ? "true" : "false";
}

static const char *mqtt_toniebox2_age_mode(settings_t *settings)
{
    if (settings != NULL && settings->toniebox2.baby_mode)
    {
        return "1+";
    }

    return "3+";
}

static const char *mqtt_toniebox2_skipping_direction(settings_t *settings)
{
    if (settings != NULL && settings->toniebox2.slap_back_left)
    {
        return "left";
    }

    return "right";
}

static int mqtt_toniebox2_setting_index_for_name(const char *setting_name)
{
    if (setting_name == NULL)
    {
        return -1;
    }

    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (osStrcmp(setting_name, toniebox2_settings_descriptors[i].setting_name) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static uint64_t mqtt_toniebox2_setting_current_value(settings_t *settings, size_t index)
{
    if (settings == NULL)
    {
        return 0;
    }

    switch (index)
    {
    case MQTT_TB2_SETTING_MAX_VOLUME:
        return settings->toniebox2.max_volume;
    case MQTT_TB2_SETTING_BEDTIME_MAX_VOLUME:
        return settings->toniebox2.bedtime_max_volume;
    case MQTT_TB2_SETTING_MAX_HEADPHONE_VOLUME:
        return settings->toniebox2.max_headphone_volume;
    case MQTT_TB2_SETTING_BEDTIME_MAX_HEADPHONE_VOLUME:
        return settings->toniebox2.bedtime_max_headphone_volume;
    case MQTT_TB2_SETTING_LIGHTRING_BRIGHTNESS:
        return settings->toniebox2.lightring_brightness;
    case MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS:
        return settings->toniebox2.bedtime_lightring_brightness;
    case MQTT_TB2_SETTING_SCRUBBING_ENABLED:
        return settings->toniebox2.scrubbing_enabled ? 1 : 0;
    case MQTT_TB2_SETTING_SKIPPING_ENABLED:
        return settings->toniebox2.slap_enabled ? 1 : 0;
    case MQTT_TB2_SETTING_SKIPPING_DIRECTION:
        return settings->toniebox2.slap_back_left ? 1 : 0;
    case MQTT_TB2_SETTING_AGE_MODE:
        return settings->toniebox2.baby_mode ? 1 : 0;
    default:
        return 0;
    }
}

static void mqtt_copy_u64_settings_array(settings_t *settings, const char *key, uint64_t target[MQTT_TB2_SETTING_COUNT])
{
    size_t len = 0;
    uint64_t *stored = settings_get_u64_array_id(key, settings->internal.overlayNumber, &len);
    osMemset(target, 0, sizeof(uint64_t) * MQTT_TB2_SETTING_COUNT);
    if (stored == NULL)
    {
        return;
    }

    size_t copy_len = len < MQTT_TB2_SETTING_COUNT ? len : MQTT_TB2_SETTING_COUNT;
    if (copy_len > 0)
    {
        osMemcpy(target, stored, sizeof(uint64_t) * copy_len);
    }
}

static void mqtt_toniebox2_settings_load_state(settings_t *settings,
                                               uint64_t revisions[MQTT_TB2_SETTING_COUNT],
                                               uint64_t pending[MQTT_TB2_SETTING_COUNT],
                                               uint64_t values[MQTT_TB2_SETTING_COUNT])
{
    mqtt_copy_u64_settings_array(settings, "internal.toniebox2SettingsDesiredRevisions", revisions);
    mqtt_copy_u64_settings_array(settings, "internal.toniebox2SettingsDesiredPendingFields", pending);
    mqtt_copy_u64_settings_array(settings, "internal.toniebox2SettingsDesiredValues", values);
}

static bool_t mqtt_toniebox2_settings_store_state(settings_t *settings,
                                                  const uint64_t revisions[MQTT_TB2_SETTING_COUNT],
                                                  const uint64_t pending[MQTT_TB2_SETTING_COUNT],
                                                  const uint64_t values[MQTT_TB2_SETTING_COUNT])
{
    uint8_t overlay_id = settings->internal.overlayNumber;
    bool_t ok = settings_set_u64_array_id("internal.toniebox2SettingsDesiredRevisions", revisions, MQTT_TB2_SETTING_COUNT, overlay_id) &&
                settings_set_u64_array_id("internal.toniebox2SettingsDesiredPendingFields", pending, MQTT_TB2_SETTING_COUNT, overlay_id) &&
                settings_set_u64_array_id("internal.toniebox2SettingsDesiredValues", values, MQTT_TB2_SETTING_COUNT, overlay_id);
    if (!ok)
    {
        TRACE_WARNING("Failed to store TB2 desired settings state for %s overlay=%u\r\n",
                      settings->commonName,
                      (unsigned)overlay_id);
    }
    return ok;
}

static uint64_t mqtt_toniebox2_settings_next_revision(const uint64_t revisions[MQTT_TB2_SETTING_COUNT])
{
    uint64_t max_revision = 0;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (revisions[i] > max_revision)
        {
            max_revision = revisions[i];
        }
    }

    uint64_t candidate = ((uint64_t)time(NULL)) * MQTT_MILLISECONDS_PER_SECOND;
    if (candidate <= max_revision)
    {
        candidate = max_revision + 1;
    }
    return candidate;
}

static size_t mqtt_toniebox2_settings_pending_count(const uint64_t pending[MQTT_TB2_SETTING_COUNT])
{
    size_t count = 0;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (pending[i] != 0)
        {
            count++;
        }
    }
    return count;
}

static bool_t mqtt_toniebox2_settings_mark_pending(settings_t *settings, const char *setting_name, size_t *marked_count)
{
    uint64_t revisions[MQTT_TB2_SETTING_COUNT];
    uint64_t pending[MQTT_TB2_SETTING_COUNT];
    uint64_t values[MQTT_TB2_SETTING_COUNT];
    mqtt_toniebox2_settings_load_state(settings, revisions, pending, values);

    int target = mqtt_toniebox2_setting_index_for_name(setting_name);
    bool_t mark_all = setting_name == NULL;
    if (!mark_all && target < 0)
    {
        TRACE_DEBUG("Ignoring unsupported TB2 setting change '%s'\r\n", setting_name);
        return FALSE;
    }

    uint64_t next_revision = mqtt_toniebox2_settings_next_revision(revisions);
    size_t marked = 0;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (!mark_all && (int)i != target)
        {
            continue;
        }

        uint64_t current_value = mqtt_toniebox2_setting_current_value(settings, i);
        if (revisions[i] == 0 || values[i] != current_value)
        {
            revisions[i] = next_revision++;
            pending[i] = 1;
            values[i] = current_value;
            marked++;
        }
    }

    size_t pending_count = mqtt_toniebox2_settings_pending_count(pending);
    if (marked_count != NULL)
    {
        *marked_count = marked;
    }
    if (marked == 0 && pending_count == 0)
    {
        return FALSE;
    }

    mqtt_toniebox2_settings_store_state(settings, revisions, pending, values);
    settings_set_bool_id("internal.toniebox2SettingsDesiredPending", pending_count > 0, settings->internal.overlayNumber);
    if (marked > 0)
    {
        settings_set_unsigned_id("internal.toniebox2SettingsDesiredAttempts", 0, settings->internal.overlayNumber);
        settings_set_unsigned_id("internal.toniebox2SettingsDesiredLastAttempt", 0, settings->internal.overlayNumber);
    }

    return TRUE;
}

static void mqtt_toniebox2_settings_ensure_pending_state(settings_t *settings)
{
    uint64_t revisions[MQTT_TB2_SETTING_COUNT];
    uint64_t pending[MQTT_TB2_SETTING_COUNT];
    uint64_t values[MQTT_TB2_SETTING_COUNT];
    mqtt_toniebox2_settings_load_state(settings, revisions, pending, values);
    if (mqtt_toniebox2_settings_pending_count(pending) > 0)
    {
        return;
    }

    size_t marked = 0;
    mqtt_toniebox2_settings_mark_pending(settings, NULL, &marked);
    if (marked > 0)
    {
        TRACE_INFO("Recovered TB2 desired settings pending state for %s with %" PRIuSIZE " fields\r\n",
                   settings->commonName,
                   marked);
        return;
    }

    settings_set_bool_id("internal.toniebox2SettingsDesiredPending", false, settings->internal.overlayNumber);
    settings_set_unsigned_id("internal.toniebox2SettingsDesiredAttempts", 0, settings->internal.overlayNumber);
    settings_set_unsigned_id("internal.toniebox2SettingsDesiredLastAttempt", 0, settings->internal.overlayNumber);
}

static char *mqtt_build_settings_desired_payload(settings_t *settings)
{
    if (settings == NULL)
    {
        settings = get_settings();
    }
    if (settings == NULL)
    {
        return NULL;
    }

    char *payload = osAllocMem(MQTT_SETTINGS_DESIRED_PAYLOAD_SIZE);
    if (payload == NULL)
    {
        return NULL;
    }

    if (settings->internal.toniebox2SettingsDesiredPending)
    {
        mqtt_toniebox2_settings_ensure_pending_state(settings);
    }

    uint64_t revisions[MQTT_TB2_SETTING_COUNT];
    mqtt_copy_u64_settings_array(settings, "internal.toniebox2SettingsDesiredRevisions", revisions);

    osSnprintf(payload, MQTT_SETTINGS_DESIRED_PAYLOAD_SIZE, "{\n"
"  \"settings_history\": {\n"
"    \"bi_tracking\": 0,\n"
"    \"max_headphone_volume\": %" PRIu64 ",\n"
"    \"battery_threshold\": 0,\n"
"    \"scrubbing_enabled\": %" PRIu64 ",\n"
"    \"fleet_obs_enabled\": 0,\n"
"    \"bedtime_max_headphone_volume\": %" PRIu64 ",\n"
"    \"timezone_transitions\": 0,\n"
"    \"log_level\": 0,\n"
"    \"timezone\": 0,\n"
"    \"bedtime_lightring_color\": 0,\n"
"    \"bedtime_max_volume\": %" PRIu64 ",\n"
"    \"skipping_direction\": %" PRIu64 ",\n"
"    \"max_volume\": %" PRIu64 ",\n"
"    \"bedtime_schedules\": 0,\n"
"    \"alarms\": 0,\n"
"    \"bedtime_lightring_brightness\": %" PRIu64 ",\n"
"    \"age_mode\": %" PRIu64 ",\n"
"    \"skipping_enabled\": %" PRIu64 ",\n"
"    \"lightring_brightness\": %" PRIu64 "\n"
"  },\n"
"  \"settings_applied\": false,\n"
"  \"bi_tracking\": true,\n"
"  \"max_headphone_volume\": %u,\n"
"  \"battery_threshold\": 1,\n"
"  \"scrubbing_enabled\": %s,\n"
"  \"fleet_obs_enabled\": true,\n"
"  \"bedtime_max_headphone_volume\": %u,\n"
"  \"timezone_transitions\": [\n"
"    {\n"
"      \"time\": 0,\n"
"      \"offset\": 0\n"
"    }\n"
"  ],\n"
"  \"log_level\": \"info\",\n"
"  \"timezone\": null,\n"
"  \"bedtime_lightring_color\": \"#ffffff\",\n"
"  \"bedtime_max_volume\": %u,\n"
"  \"skipping_direction\": \"%s\",\n"
"  \"max_volume\": %u,\n"
"  \"bedtime_schedules\": [],\n"
"  \"alarms\": [],\n"
"  \"bedtime_lightring_brightness\": %u,\n"
"  \"age_mode\": \"%s\",\n"
"  \"skipping_enabled\": %s,\n"
"  \"lightring_brightness\": %u\n"
"}",
        revisions[MQTT_TB2_SETTING_MAX_HEADPHONE_VOLUME],
        revisions[MQTT_TB2_SETTING_SCRUBBING_ENABLED],
        revisions[MQTT_TB2_SETTING_BEDTIME_MAX_HEADPHONE_VOLUME],
        revisions[MQTT_TB2_SETTING_BEDTIME_MAX_VOLUME],
        revisions[MQTT_TB2_SETTING_SKIPPING_DIRECTION],
        revisions[MQTT_TB2_SETTING_MAX_VOLUME],
        revisions[MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS],
        revisions[MQTT_TB2_SETTING_AGE_MODE],
        revisions[MQTT_TB2_SETTING_SKIPPING_ENABLED],
        revisions[MQTT_TB2_SETTING_LIGHTRING_BRIGHTNESS],
        (unsigned)settings->toniebox2.max_headphone_volume,
        mqtt_json_bool(settings->toniebox2.scrubbing_enabled),
        (unsigned)settings->toniebox2.bedtime_max_headphone_volume,
        (unsigned)settings->toniebox2.bedtime_max_volume,
        mqtt_toniebox2_skipping_direction(settings),
        (unsigned)settings->toniebox2.max_volume,
        (unsigned)settings->toniebox2.bedtime_lightring_brightness,
        mqtt_toniebox2_age_mode(settings),
        mqtt_json_bool(settings->toniebox2.slap_enabled),
        (unsigned)settings->toniebox2.lightring_brightness);

    return payload;
}

static bool mqtt_connection_has_sub(MqttClientConnection *conn, const char *topic)
{
    if (conn == NULL || !conn->active)
        return false;
    for (size_t i = 0; i < conn->subscription_count; i++)
    {
        if (mqtt_topic_match(conn->subscriptions[i].topic, topic))
        {
            return true;
        }
    }
    return false;
}

static bool mqtt_connection_has_exact_sub(MqttClientConnection *conn, const char *topic)
{
    if (conn == NULL || !conn->active || topic == NULL)
    {
        return false;
    }
    for (size_t i = 0; i < conn->subscription_count; i++)
    {
        if (osStrcmp(conn->subscriptions[i].topic, topic) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool_t mqtt_extract_toniebox_topic_common_name(const char *topic, const char *suffix, char *common_name, size_t common_name_size)
{
    const char *prefix = "toniebox/";
    size_t prefix_len = osStrlen(prefix);

    if (topic == NULL || suffix == NULL || common_name == NULL || common_name_size == 0)
    {
        return FALSE;
    }

    size_t topic_len = osStrlen(topic);
    size_t suffix_len = osStrlen(suffix);
    if (topic_len <= prefix_len + suffix_len)
    {
        return FALSE;
    }
    if (osStrncmp(topic, prefix, prefix_len) != 0)
    {
        return FALSE;
    }
    if (osStrcmp(topic + topic_len - suffix_len, suffix) != 0)
    {
        return FALSE;
    }

    size_t cn_len = topic_len - prefix_len - suffix_len;
    if (cn_len == 0 || cn_len >= common_name_size)
    {
        return FALSE;
    }

    osMemcpy(common_name, topic + prefix_len, cn_len);
    common_name[cn_len] = '\0';
    return TRUE;
}

static bool_t mqtt_is_hex_string(const char *value, size_t len)
{
    if (value == NULL || osStrlen(value) != len)
    {
        return FALSE;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static bool_t mqtt_is_all_zero_string(const char *value)
{
    if (value == NULL || value[0] == '\0')
    {
        return FALSE;
    }

    for (size_t i = 0; value[i] != '\0'; i++)
    {
        if (value[i] != '0')
        {
            return FALSE;
        }
    }

    return TRUE;
}

static bool_t mqtt_extract_toniebox_claim_topic(const char *topic, char *common_name, size_t common_name_size,
                                                char ruid[TBS_TB2_CLAIM_RUID_MAX])
{
    const char *prefix = "toniebox/";
    const char *claim_segment = "/claim/";
    size_t prefix_len = osStrlen(prefix);
    size_t claim_segment_len = osStrlen(claim_segment);

    if (topic == NULL || common_name == NULL || ruid == NULL)
    {
        return FALSE;
    }

    size_t topic_len = osStrlen(topic);

    if (topic_len <= prefix_len + claim_segment_len + 16 || osStrncmp(topic, prefix, prefix_len) != 0)
    {
        return FALSE;
    }

    const char *common_start = topic + prefix_len;
    const char *claim_start = osStrstr(common_start, claim_segment);
    if (claim_start == NULL)
    {
        return FALSE;
    }

    size_t common_len = claim_start - common_start;
    if (common_len == 0 || common_len >= common_name_size)
    {
        return FALSE;
    }

    const char *ruid_start = claim_start + claim_segment_len;
    if (!mqtt_is_hex_string(ruid_start, 16))
    {
        return FALSE;
    }

    osMemcpy(common_name, common_start, common_len);
    common_name[common_len] = '\0';
    osMemcpy(ruid, ruid_start, 16);
    ruid[16] = '\0';
    return TRUE;
}

static bool_t mqtt_connection_matches_box_overlay(MqttClientConnection *conn, settings_t *settings)
{
    if (conn == NULL || settings == NULL || !conn->active || !conn->box_connection)
    {
        return FALSE;
    }
    if (settings->commonName == NULL || settings->commonName[0] == '\0')
    {
        return FALSE;
    }
    if (conn->box_overlay_id != settings->internal.overlayNumber)
    {
        return FALSE;
    }
    if (conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, settings->commonName) != 0)
    {
        return FALSE;
    }

    return TRUE;
}

static bool_t mqtt_validate_json_payload(const char *payload, const char *label)
{
    if (payload == NULL || payload[0] == '\0')
    {
        TRACE_WARNING("MQTT app-control/%s payload is empty\r\n", label != NULL ? label : "-");
        return FALSE;
    }

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT app-control/%s payload is not valid JSON\r\n", label != NULL ? label : "-");
        return FALSE;
    }

    cJSON_Delete(json);
    return TRUE;
}

static uint32_t mqtt_payload_hash(const char *payload)
{
    uint32_t hash = 2166136261u;
    const uint8_t *p = (const uint8_t *)payload;

    while (p != NULL && *p != '\0')
    {
        hash ^= *p++;
        hash *= 16777619u;
    }

    return hash;
}

static bool_t mqtt_should_log_full_payload(size_t payload_len)
{
    settings_t *settings = get_settings();
    return settings != NULL &&
           settings->mqtt_server.log_full_payloads &&
           payload_len >= MQTT_LOG_INLINE_PAYLOAD_SIZE;
}

static void mqtt_trace_full_publish(const char *direction, const char *topic, const uint8_t *payload,
                                    size_t payload_len, uint8_t qos)
{
    if (!mqtt_should_log_full_payload(payload_len) || topic == NULL || payload == NULL)
    {
        return;
    }

    size_t encoded_len = 0;
    base64Encode(payload, payload_len, NULL, &encoded_len);

    char_t *payload_b64 = osAllocMem(encoded_len + 1);
    if (payload_b64 == NULL)
    {
        TRACE_WARNING("MQTT full payload capture skipped: allocation failed for topic=%s len=%" PRIuSIZE "\r\n",
                      topic,
                      payload_len);
        return;
    }

    base64Encode(payload, payload_len, payload_b64, &encoded_len);
    payload_b64[encoded_len] = '\0';

    TRACE_INFO("MQTT FULL PUBLISH dir=%s topic='%s' payload_b64='%s' (QoS %u, declared_len %" PRIuSIZE ", observed_len %" PRIuSIZE ")\r\n",
               direction != NULL ? direction : "?",
               topic,
               payload_b64,
               (unsigned)qos,
               payload_len,
               payload_len);

    osFreeMem(payload_b64);
}

static bool_t mqtt_connection_publish(MqttClientConnection *conn, const char *topic, const char *payload)
{
    if (conn == NULL || !conn->active || topic == NULL || payload == NULL)
    {
        return FALSE;
    }

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remaining_len = 2 + topic_len + payload_len;

    // Allocate memory for the packet
    size_t header_len = 1; // 0x30
    uint8_t length_bytes[4];
    size_t length_count = 0;
    size_t temp_len = remaining_len;
    do {
        uint8_t encoded_byte = temp_len % 128;
        temp_len /= 128;
        if (temp_len > 0) {
            encoded_byte |= 128;
        }
        length_bytes[length_count++] = encoded_byte;
    } while (temp_len > 0);

    size_t packet_size = header_len + length_count + remaining_len;
    uint8_t *packet = osAllocMem(packet_size);
    if (packet == NULL)
    {
        TRACE_ERROR("Failed to allocate memory for MQTT PUBLISH packet\r\n");
        return FALSE;
    }

    size_t p = 0;
    packet[p++] = 0x30; // PUBLISH (QoS 0)
    
    memcpy(&packet[p], length_bytes, length_count);
    p += length_count;

    packet[p++] = (topic_len >> 8) & 0xFF;
    packet[p++] = topic_len & 0xFF;

    memcpy(&packet[p], topic, topic_len);
    p += topic_len;

    memcpy(&packet[p], payload, payload_len);
    p += payload_len;

    size_t written = 0;
    error_t error;
    if (conn->tlsContext)
    {
        error = tlsWrite(conn->tlsContext, packet, packet_size, &written, 0);
    }
    else
    {
        error = socketSend(conn->socket, packet, packet_size, &written, 0);
    }

    if (error != NO_ERROR || written != packet_size)
    {
        TRACE_WARNING("MQTT PUBLISH failed: slot=%d box=%s overlay=%u topic=%s -> len %" PRIuSIZE ", written %" PRIuSIZE ", error=%s\r\n",
                      mqtt_connection_slot(conn),
                      mqtt_connection_common_name(conn),
                      (unsigned)mqtt_connection_overlay_id(conn),
                      topic,
                      payload_len,
                      written,
                      error2text(error));
        osFreeMem(packet);
        mqtt_connection_close(conn, "publish write failed");
        return FALSE;
    }

    mqtt_trace_full_publish("tx", topic, (const uint8_t *)payload, payload_len, 0);
    TRACE_INFO("MQTT PUBLISH: %s -> %s (len %" PRIuSIZE ")\r\n", topic, payload, payload_len);

    osFreeMem(packet);
    return TRUE;
}

static void mqtt_connection_update_context_from_cert(MqttClientConnection *conn)
{
    conn->box_connection = FALSE;
    conn->box_overlay_id = 0;
    conn->box_common_name[0] = '\0';
    if (conn->tlsContext != NULL && osStrlen(conn->tlsContext->client_cert_subject) > 0)
    {
        char_t *subject = conn->tlsContext->client_cert_subject;

        if (mqtt_connection_has_trusted_client_cert(conn))
        {
            char_t *commonName = NULL;
            if (osStrlen(subject) == 15 && !osStrncmp(subject, "b'", 2) && subject[14] == '\'') // tonies standard cn with b'[MAC]'
            {
                commonName = strdup(&subject[2]);
                commonName[osStrlen(commonName) - 1] = '\0';
            } else if (osStrlen(subject) == 12) {
                commonName = strdup(subject);
            }

            if (commonName != NULL) {
                settings_t *box_settings = get_settings_cn(commonName);
                if (box_settings != NULL && box_settings->internal.config_used)
                {
                    mqtt_promote_connection_to_box(conn, box_settings, commonName, "cert");
                }
                osFreeMem(commonName);
            }
        }
    }
}

bool_t mqtt_server_has_active_box_connection(uint8_t overlay_id)
{
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (conn->active && conn->box_connection && conn->client_ctx.settings != NULL &&
            conn->client_ctx.settings->internal.overlayNumber == overlay_id)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static void mqtt_touch_box_connection(MqttClientConnection *conn)
{
    if (conn == NULL || !conn->active || !conn->box_connection || conn->client_ctx.settings == NULL)
    {
        return;
    }

    settings_internal_t *internal = &conn->client_ctx.settings->internal;
    if (!internal->config_used)
    {
        return;
    }

    internal->online = true;
    internal->last_connection = time(NULL);
}

static bool_t mqtt_toniebox2_settings_desired_topic(settings_t *settings, char *topic, size_t topic_size)
{
    if (settings == NULL || settings->commonName == NULL || settings->commonName[0] == '\0')
    {
        return FALSE;
    }

    osSnprintf(topic, topic_size, "toniebox/%s/settings/desired", settings->commonName);
    return TRUE;
}

static bool_t mqtt_app_control_topic(settings_t *settings, const char *command, char *topic, size_t topic_size)
{
    if (settings == NULL || settings->commonName == NULL || settings->commonName[0] == '\0' ||
        command == NULL || command[0] == '\0')
    {
        return FALSE;
    }

    osSnprintf(topic, topic_size, "toniebox/%s/app-control/%s", settings->commonName, command);
    return TRUE;
}

static bool_t mqtt_is_toniebox2_overlay(settings_t *settings)
{
    return settings != NULL && settings->internal.config_used && settings->toniebox.boxGeneration == GENERATION_TB2;
}

static bool_t mqtt_publish_settings_desired_to_connection(MqttClientConnection *conn, bool_t track_pending_attempt)
{
    if (conn == NULL || !conn->active || !conn->box_connection || conn->client_ctx.settings == NULL)
    {
        return FALSE;
    }

    settings_t *settings = conn->client_ctx.settings;
    char topic[128];
    if (!mqtt_toniebox2_settings_desired_topic(settings, topic, sizeof(topic)))
    {
        return FALSE;
    }

    char *settings_payload = mqtt_build_settings_desired_payload(settings);
    if (settings_payload == NULL)
    {
        TRACE_ERROR("Failed to build settings response\r\n");
        return FALSE;
    }

    bool_t published = mqtt_connection_publish(conn, topic, settings_payload);
    osFreeMem(settings_payload);

    if (published && track_pending_attempt && settings->internal.toniebox2SettingsDesiredPending)
    {
        uint32_t attempts = settings->internal.toniebox2SettingsDesiredAttempts + 1;
        uint32_t now = (uint32_t)time(NULL);
        settings_set_unsigned_id("internal.toniebox2SettingsDesiredAttempts", attempts, settings->internal.overlayNumber);
        settings_set_unsigned_id("internal.toniebox2SettingsDesiredLastAttempt", now, settings->internal.overlayNumber);

        if (attempts >= MQTT_SETTINGS_DESIRED_MAX_ATTEMPTS)
        {
            TRACE_WARNING("MQTT settings desired sent %u times for %s, waiting for confirm\r\n",
                          (unsigned)attempts,
                          settings->commonName);
        }
    }

    return published;
}

static bool_t mqtt_publish_pending_settings_desired_to_connection(MqttClientConnection *conn, bool_t require_subscription, bool_t force)
{
    if (conn == NULL || !conn->active || !conn->box_connection || conn->client_ctx.settings == NULL)
    {
        return FALSE;
    }

    settings_t *settings = conn->client_ctx.settings;
    if (!settings->internal.toniebox2SettingsDesiredPending)
    {
        return FALSE;
    }

    if (settings->internal.toniebox2SettingsDesiredAttempts >= MQTT_SETTINGS_DESIRED_MAX_ATTEMPTS)
    {
        return FALSE;
    }

    char topic[128];
    if (!mqtt_toniebox2_settings_desired_topic(settings, topic, sizeof(topic)))
    {
        return FALSE;
    }

    if (require_subscription && !mqtt_connection_has_sub(conn, topic))
    {
        return FALSE;
    }

    uint32_t now = (uint32_t)time(NULL);
    uint32_t last_attempt = settings->internal.toniebox2SettingsDesiredLastAttempt;
    if (!force && last_attempt != 0 && now >= last_attempt && (now - last_attempt) < MQTT_SETTINGS_DESIRED_RETRY_INTERVAL_SEC)
    {
        return FALSE;
    }

    return mqtt_publish_settings_desired_to_connection(conn, TRUE);
}

static void mqtt_clear_settings_desired_pending(settings_t *settings)
{
    if (settings == NULL)
    {
        return;
    }

    settings_set_bool_id("internal.toniebox2SettingsDesiredPending", false, settings->internal.overlayNumber);
    settings_set_unsigned_id("internal.toniebox2SettingsDesiredAttempts", 0, settings->internal.overlayNumber);
    settings_set_unsigned_id("internal.toniebox2SettingsDesiredLastAttempt", 0, settings->internal.overlayNumber);
}

static bool_t mqtt_get_json_bool(cJSON *json, const char *name, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsBool(item))
    {
        return FALSE;
    }

    *value = cJSON_IsTrue(item);
    return TRUE;
}

static bool_t mqtt_get_json_percent(cJSON *json, const char *name, uint32_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 100)
    {
        return FALSE;
    }

    *value = (uint32_t)item->valueint;
    return TRUE;
}

static bool_t mqtt_get_json_u64(cJSON *json, const char *name, uint64_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > MQTT_JSON_SAFE_INTEGER_MAX)
    {
        return FALSE;
    }

    *value = (uint64_t)item->valuedouble;
    return TRUE;
}

static size_t mqtt_count_legacy_toniebox2_confirm_values(cJSON *json)
{
    size_t seen = 0;
    uint32_t percent_value = 0;
    bool bool_value = false;

    if (mqtt_get_json_percent(json, "max_volume", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_percent(json, "bedtime_max_volume", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_percent(json, "max_headphone_volume", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_percent(json, "bedtime_max_headphone_volume", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_percent(json, "lightring_brightness", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_percent(json, "bedtime_lightring_brightness", &percent_value))
    {
        seen++;
    }
    if (mqtt_get_json_bool(json, "scrubbing_enabled", &bool_value))
    {
        seen++;
    }
    if (mqtt_get_json_bool(json, "skipping_enabled", &bool_value))
    {
        seen++;
    }

    cJSON *skipping_direction = cJSON_GetObjectItemCaseSensitive(json, "skipping_direction");
    if (cJSON_IsString(skipping_direction) && skipping_direction->valuestring != NULL)
    {
        if (strcasecmp(skipping_direction->valuestring, "left") == 0 || strcasecmp(skipping_direction->valuestring, "right") == 0)
        {
            seen++;
        }
    }

    cJSON *age_mode = cJSON_GetObjectItemCaseSensitive(json, "age_mode");
    if (cJSON_IsString(age_mode) && age_mode->valuestring != NULL)
    {
        if (strcasecmp(age_mode->valuestring, "1+") == 0 || strcasecmp(age_mode->valuestring, "3+") == 0)
        {
            seen++;
        }
    }

    return seen;
}

static void mqtt_ack_toniebox2_settings_history(settings_t *settings, cJSON *history, size_t *acked, size_t *missing, size_t *stale, size_t *remaining)
{
    uint64_t revisions[MQTT_TB2_SETTING_COUNT];
    uint64_t pending[MQTT_TB2_SETTING_COUNT];
    uint64_t values[MQTT_TB2_SETTING_COUNT];
    mqtt_toniebox2_settings_load_state(settings, revisions, pending, values);

    *acked = 0;
    *missing = 0;
    *stale = 0;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (pending[i] == 0)
        {
            continue;
        }

        uint64_t confirmed_revision = 0;
        if (!mqtt_get_json_u64(history, toniebox2_settings_descriptors[i].json_name, &confirmed_revision))
        {
            (*missing)++;
            TRACE_INFO("MQTT settings confirm missing field for %s overlay=%u field=%s expected=%" PRIu64 "\r\n",
                       settings->commonName,
                       (unsigned)settings->internal.overlayNumber,
                       toniebox2_settings_descriptors[i].json_name,
                       revisions[i]);
            continue;
        }

        if (confirmed_revision == revisions[i])
        {
            pending[i] = 0;
            (*acked)++;
            continue;
        }

        (*stale)++;
        TRACE_INFO("MQTT settings confirm stale field for %s overlay=%u field=%s expected=%" PRIu64 " got=%" PRIu64 "\r\n",
                   settings->commonName,
                   (unsigned)settings->internal.overlayNumber,
                   toniebox2_settings_descriptors[i].json_name,
                   revisions[i],
                   confirmed_revision);
    }

    *remaining = mqtt_toniebox2_settings_pending_count(pending);
    mqtt_toniebox2_settings_store_state(settings, revisions, pending, values);
    settings_set_bool_id("internal.toniebox2SettingsDesiredPending", *remaining > 0, settings->internal.overlayNumber);
    if (*remaining == 0)
    {
        mqtt_clear_settings_desired_pending(settings);
    }
}

static bool_t mqtt_get_json_uint32(cJSON *json, const char *name, uint32_t *value)
{
    if (json == NULL || name == NULL || value == NULL)
    {
        return FALSE;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX)
    {
        return FALSE;
    }

    *value = (uint32_t)item->valuedouble;
    return TRUE;
}

static bool_t mqtt_get_json_int32(cJSON *json, const char *name, int32_t *value)
{
    if (json == NULL || name == NULL || value == NULL)
    {
        return FALSE;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < INT32_MIN || item->valuedouble > INT32_MAX)
    {
        return FALSE;
    }

    *value = (int32_t)item->valuedouble;
    return TRUE;
}

static bool_t mqtt_get_json_scalar_text(cJSON *json, const char *name, char *value, size_t value_size)
{
    if (json == NULL || name == NULL || value == NULL || value_size == 0)
    {
        return FALSE;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        osStrncpy(value, item->valuestring, value_size - 1);
        value[value_size - 1] = '\0';
        return TRUE;
    }
    if (cJSON_IsNumber(item) && item->valuedouble >= 0)
    {
        osSnprintf(value, value_size, "%.0f", item->valuedouble);
        return TRUE;
    }

    return FALSE;
}

static bool_t mqtt_get_json_decimal_text(cJSON *json, const char *name, char *value, size_t value_size)
{
    if (json == NULL || name == NULL || value == NULL || value_size == 0)
    {
        return FALSE;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        osStrncpy(value, item->valuestring, value_size - 1);
        value[value_size - 1] = '\0';
        return TRUE;
    }
    if (cJSON_IsNumber(item) && item->valuedouble >= 0)
    {
        osSnprintf(value, value_size, "%.3f", item->valuedouble);
        char *decimal = strchr(value, '.');
        if (decimal != NULL)
        {
            char *end = value + strlen(value) - 1;
            while (end > decimal && *end == '0')
            {
                *end-- = '\0';
            }
            if (end == decimal)
            {
                *end = '\0';
            }
        }
        return TRUE;
    }

    return FALSE;
}

static error_t handle_mqtt_connect(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    mqtt_connection_update_context_from_cert(conn);
    TRACE_INFO("MQTT: connection established for %s\r\n", conn->client_ctx.settings->commonName);
    
    uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    size_t written = 0;
    error_t error;
    if (conn->tlsContext)
        error = tlsWrite(conn->tlsContext, connack, sizeof(connack), &written, 0);
    else
        error = socketSend(conn->socket, connack, sizeof(connack), &written, 0);

    if (error != NO_ERROR || written != sizeof(connack))
    {
        mqtt_connection_close(conn, "connack write failed");
        return error != NO_ERROR ? error : ERROR_FAILURE;
    }

    return NO_ERROR;
}

static error_t handle_mqtt_pingreq(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    TRACE_INFO("PINGREQ received\r\n");
    uint8_t pingresp[] = {0xD0, 0x00};
    size_t written = 0;
    error_t error;
    if (conn->tlsContext)
        error = tlsWrite(conn->tlsContext, pingresp, sizeof(pingresp), &written, 0);
    else
        error = socketSend(conn->socket, pingresp, sizeof(pingresp), &written, 0);

    if (error != NO_ERROR || written != sizeof(pingresp))
    {
        mqtt_connection_close(conn, "pingresp write failed");
        return error != NO_ERROR ? error : ERROR_FAILURE;
    }

    return NO_ERROR;
}

static error_t handle_mqtt_disconnect(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    TRACE_INFO("DISCONNECT received\r\n");
    mqtt_connection_close(conn, "disconnect packet");
    return NO_ERROR;
}

static error_t handle_mqtt_subscribe(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    TRACE_INFO("SUBSCRIBE received\r\n");
    if (payload_len < 2)
        return ERROR_INVALID_LENGTH;

    size_t p = 0;
    uint16_t packet_id = (payload[p] << 8) | payload[p + 1];
    p += 2;
    bool_t publish_fresh_tonies_after_suback = FALSE;

    while (p < payload_len)
    {
        if (p + 2 > payload_len)
            break;
        size_t topic_len = (payload[p] << 8) | payload[p + 1];
        p += 2;

        if (p + topic_len > payload_len)
            break;

        char sub_topic[256];
        size_t copy_len = topic_len < sizeof(sub_topic) - 1 ? topic_len : sizeof(sub_topic) - 1;
        memcpy(sub_topic, &payload[p], copy_len);
        sub_topic[copy_len] = '\0';
        p += topic_len;

        if (p >= payload_len)
            break;
        uint8_t qos = payload[p++];
        TRACE_INFO("  Topic='%s', QoS=%u\r\n", sub_topic, qos);

        mqtt_connection_update_context(conn, sub_topic);

        // Store subscription on connection
        bool sub_found = false;
        for (size_t s = 0; s < conn->subscription_count; s++)
        {
            if (strcmp(conn->subscriptions[s].topic, sub_topic) == 0)
            {
                conn->subscriptions[s].qos = qos;
                sub_found = true;
                break;
            }
        }
        if (!sub_found && conn->subscription_count < MQTT_MAX_SUBSCRIPTIONS)
        {
            osStrncpy(conn->subscriptions[conn->subscription_count].topic, sub_topic, sizeof(conn->subscriptions[conn->subscription_count].topic) - 1);
            conn->subscriptions[conn->subscription_count].topic[sizeof(conn->subscriptions[conn->subscription_count].topic) - 1] = '\0';
            conn->subscriptions[conn->subscription_count].qos = qos;
            conn->subscription_count++;
        }

        // Check if this is the fresh-tonies topic subscription
        bool is_fresh_tonies_sub = false;
        char mac[32] = {0};
        if (strncmp(sub_topic, "toniebox/", 9) == 0 && topic_len > 22)
        {
            const char *suffix = "/fresh-tonies";
            size_t suffix_len = strlen(suffix);
            if (strcmp(sub_topic + topic_len - suffix_len, suffix) == 0)
            {
                size_t mac_len = topic_len - 9 - suffix_len;
                if (mac_len < sizeof(mac))
                {
                    memcpy(mac, sub_topic + 9, mac_len);
                    mac[mac_len] = '\0';
                    is_fresh_tonies_sub = true;
                }
            }
        }

        if (is_fresh_tonies_sub)
        {
            settings_t *box_settings = get_settings_cn(mac);
            if (box_settings != NULL && box_settings->internal.config_used)
            {
                publish_fresh_tonies_after_suback = TRUE;
            }
        }
    }

    // Responding with SUBACK (0x90)
    uint8_t suback[] = {0x90, 0x03, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFF), 0x00};
    size_t written = 0;
    error_t error;
    if (conn->tlsContext)
        error = tlsWrite(conn->tlsContext, suback, sizeof(suback), &written, 0);
    else
        error = socketSend(conn->socket, suback, sizeof(suback), &written, 0);

    if (error != NO_ERROR || written != sizeof(suback))
    {
        mqtt_connection_close(conn, "suback write failed");
        return error != NO_ERROR ? error : ERROR_FAILURE;
    }

    if (error == NO_ERROR && publish_fresh_tonies_after_suback)
    {
        mqtt_server_publish_fresh_tonies(&conn->client_ctx);
    }
    if (error == NO_ERROR)
    {
        mqtt_publish_pending_settings_desired_to_connection(conn, TRUE, TRUE);
    }
    return error;
}

static error_t handle_mqtt_publish_logs(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    if (mqtt_should_log_full_payload(payload_len))
    {
        TRACE_DEBUG("PUBLISH topic='%s', payload=<full-capture> (QoS 0, len %zu)\r\n", topic, payload_len);
        return NO_ERROR;
    }

    char payload_str[256];
    size_t copy_len = payload_len < sizeof(payload_str) - 1 ? payload_len : sizeof(payload_str) - 1;
    memcpy(payload_str, payload, copy_len);
    payload_str[copy_len] = '\0';

    TRACE_DEBUG("PUBLISH topic='%s', payload='%s' (QoS 0, len %zu)\r\n", topic, payload_str, payload_len);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_claim(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    char topic_common_name[32];
    char ruid[TBS_TB2_CLAIM_RUID_MAX];
    if (!mqtt_extract_toniebox_claim_topic(topic, topic_common_name, sizeof(topic_common_name), ruid))
    {
        TRACE_WARNING("MQTT claim topic is invalid: %s\r\n", topic != NULL ? topic : "-");
        return NO_ERROR;
    }

    if (!conn->box_connection || conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, topic_common_name) != 0 ||
        conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring claim from non-matching MQTT connection topic=%s\r\n", topic);
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT claim payload is not valid JSON for %s ruid=%s\r\n",
                      conn->client_ctx.settings->commonName,
                      ruid);
        return NO_ERROR;
    }

    cJSON *bd = cJSON_GetObjectItemCaseSensitive(json, "bd");
    if (!cJSON_IsString(bd) || bd->valuestring == NULL || bd->valuestring[0] == '\0' ||
        osStrlen(bd->valuestring) >= TBS_TB2_CLAIM_BD_MAX)
    {
        TRACE_WARNING("MQTT claim payload has no usable bd for %s ruid=%s\r\n",
                      conn->client_ctx.settings->commonName,
                      ruid);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    bool_t bd_all_zero = mqtt_is_all_zero_string(bd->valuestring);
    tbs_toniebox2_claim(&conn->client_ctx, ruid, bd->valuestring, bd_all_zero);

    TRACE_INFO("MQTT claim for %s overlay=%u: ruid=%s bd=%s%s\r\n",
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               ruid,
               bd->valuestring,
               bd_all_zero ? " all_zero" : "");

    cJSON_Delete(json);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_settings_request(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    if (!conn->box_connection)
    {
        TRACE_INFO("Ignoring settings request from non-box MQTT connection\r\n");
        return NO_ERROR;
    }

    char mac[32] = {0};
    size_t topic_len = strlen(topic);
    const char *suffix = "/settings/request";
    size_t suffix_len = strlen(suffix);
    size_t mac_len = topic_len - 9 - suffix_len;
    if (mac_len < sizeof(mac))
    {
        memcpy(mac, topic + 9, mac_len);
        mac[mac_len] = '\0';
    }

    TRACE_INFO("Settings request from mac=%s\r\n", mac);
    bool_t track_pending_attempt = conn->client_ctx.settings != NULL && conn->client_ctx.settings->internal.toniebox2SettingsDesiredPending;
    if (mqtt_publish_settings_desired_to_connection(conn, track_pending_attempt))
    {
        mqtt_server_publish_fresh_tonies(&conn->client_ctx);
    }
    return NO_ERROR;
}

static error_t handle_mqtt_publish_settings_confirm(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    if (!conn->box_connection || conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring settings confirm from non-box MQTT connection\r\n");
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT settings confirm payload is not valid JSON for %s\r\n", conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    settings_t *settings = conn->client_ctx.settings;
    size_t legacy_fields = mqtt_count_legacy_toniebox2_confirm_values(json);
    cJSON *settings_applied = cJSON_GetObjectItemCaseSensitive(json, "settings_applied");
    bool_t legacy_settings_applied = cJSON_IsTrue(settings_applied);
    cJSON *history = cJSON_GetObjectItemCaseSensitive(json, "toniebox_history");

    if (cJSON_IsObject(history))
    {
        size_t acked = 0;
        size_t missing = 0;
        size_t stale = 0;
        size_t remaining = 0;
        mqtt_ack_toniebox2_settings_history(settings, history, &acked, &missing, &stale, &remaining);
        TRACE_INFO("MQTT settings confirm for %s overlay=%u: acked=%" PRIuSIZE " missing=%" PRIuSIZE " stale=%" PRIuSIZE " remaining=%" PRIuSIZE "\r\n",
                   settings->commonName,
                   (unsigned)settings->internal.overlayNumber,
                   acked,
                   missing,
                   stale,
                   remaining);
    }
    else
    {
        TRACE_INFO("MQTT settings confirm for %s overlay=%u without toniebox_history; pending unchanged (settings_applied=%s legacy_fields=%" PRIuSIZE ")\r\n",
                   settings->commonName,
                   (unsigned)settings->internal.overlayNumber,
                   legacy_settings_applied ? "true" : "false",
                   legacy_fields);
    }

    if (legacy_settings_applied || legacy_fields > 0)
    {
        TRACE_INFO("MQTT settings confirm for %s overlay=%u contains legacy confirm fields; local settings are not overwritten\r\n",
                   settings->commonName,
                   (unsigned)settings->internal.overlayNumber);
    }

    cJSON_Delete(json);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_app_reply_bedtime_state(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    char topic_common_name[32];
    if (!mqtt_extract_toniebox_topic_common_name(topic, "/app-reply/bedtime-state", topic_common_name, sizeof(topic_common_name)))
    {
        return NO_ERROR;
    }

    if (!conn->box_connection || conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, topic_common_name) != 0 ||
        conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring bedtime-state reply from non-matching MQTT connection topic=%s\r\n", topic);
        return NO_ERROR;
    }

    settings_t *settings = conn->client_ctx.settings;
    toniebox_state_t *state = conn->client_ctx.state;
    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT bedtime-state payload is not valid JSON for %s\r\n", settings->commonName);
        return NO_ERROR;
    }

    cJSON *stl = cJSON_GetObjectItemCaseSensitive(json, "stl");
    if (!cJSON_IsObject(stl))
    {
        TRACE_WARNING("MQTT bedtime-state payload has no stl object for %s\r\n", settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    char duration_text[16] = "-";
    char default_duration_text[16] = "-";
    char until_text[64] = "-";
    const char *stl_state = "-";

    if (state != NULL)
    {
        osMemset(&state->bedtime, 0, sizeof(state->bedtime));
        state->bedtime.valid = true;
        state->bedtime.updated_at = (uint32_t)time(NULL);
    }

    cJSON *state_json = cJSON_GetObjectItemCaseSensitive(stl, "state");
    if (cJSON_IsString(state_json) && state_json->valuestring != NULL)
    {
        stl_state = state_json->valuestring;
        if (state != NULL)
        {
            osStrncpy(state->bedtime.state, state_json->valuestring, sizeof(state->bedtime.state) - 1);
            state->bedtime.state[sizeof(state->bedtime.state) - 1] = '\0';
            stl_state = state->bedtime.state;
        }
    }

    uint32_t duration = 0;
    if (mqtt_get_json_uint32(stl, "duration", &duration))
    {
        osSnprintf(duration_text, sizeof(duration_text), "%u", (unsigned)duration);
        if (state != NULL)
        {
            state->bedtime.duration_valid = true;
            state->bedtime.duration = duration;
        }
    }

    uint32_t default_duration = 0;
    if (mqtt_get_json_uint32(stl, "defaultDuration", &default_duration))
    {
        osSnprintf(default_duration_text, sizeof(default_duration_text), "%u", (unsigned)default_duration);
        if (state != NULL)
        {
            state->bedtime.default_duration_valid = true;
            state->bedtime.defaultDuration = default_duration;
        }
    }

    if (mqtt_get_json_scalar_text(stl, "until", until_text, sizeof(until_text)))
    {
        if (state != NULL)
        {
            osStrncpy(state->bedtime.until, until_text, sizeof(state->bedtime.until) - 1);
            state->bedtime.until[sizeof(state->bedtime.until) - 1] = '\0';
            state->bedtime.until_valid = true;
        }
    }

    uint32_t now = (uint32_t)time(NULL);
    uint32_t overlay_id = settings->internal.overlayNumber;
    uint32_t age = 0;
    uint32_t sequence = 0;
    uint32_t payload_hash = 0;
    const char *correlation = "none";
    if (overlay_id < MAX_OVERLAYS)
    {
        MqttAppControlStlState *last_stl = &app_control_stl_state[overlay_id];
        if (last_stl->valid)
        {
            age = now >= last_stl->sent_at ? now - last_stl->sent_at : 0;
            sequence = last_stl->sequence;
            payload_hash = last_stl->payload_hash;
            correlation = age <= MQTT_APP_CONTROL_REPLY_WINDOW_SEC ? "matched" : "stale";
        }
    }

    TRACE_INFO("MQTT bedtime-state for %s: state=%s duration=%s defaultDuration=%s until=%s stlReply=%s age=%u seq=%u hash=%08" PRIX32 "\r\n",
               settings->commonName,
               stl_state,
               duration_text,
               default_duration_text,
               until_text,
               correlation,
               (unsigned)age,
               (unsigned)sequence,
               payload_hash);

    tbs_toniebox2_bedtime_state_changed(&conn->client_ctx);

    cJSON_Delete(json);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_metrics_battery(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    char topic_common_name[32];
    if (!mqtt_extract_toniebox_topic_common_name(topic, "/metrics/battery", topic_common_name, sizeof(topic_common_name)))
    {
        return NO_ERROR;
    }

    if (!conn->box_connection || conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, topic_common_name) != 0 ||
        conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring battery metrics from non-matching MQTT connection topic=%s\r\n", topic);
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT battery metrics payload is not valid JSON for %s\r\n", conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    uint32_t percent = 0;
    int32_t raw = 0;
    int32_t current = 0;
    char status[TBS_TB2_BATTERY_STATUS_MAX] = "";
    bool_t percent_valid = mqtt_get_json_percent(json, "percent", &percent);
    bool_t raw_valid = mqtt_get_json_int32(json, "raw", &raw);
    bool_t current_valid = mqtt_get_json_int32(json, "current", &current);
    bool_t status_valid = mqtt_get_json_scalar_text(json, "status", status, sizeof(status));

    if (!percent_valid && !raw_valid && !current_valid && !status_valid)
    {
        TRACE_WARNING("MQTT battery metrics payload has no usable fields for %s\r\n", conn->client_ctx.settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    tbs_toniebox2_battery_metrics(&conn->client_ctx,
                                  percent_valid, percent,
                                  raw_valid, raw,
                                  current_valid, current,
                                  status_valid ? status : NULL);

    TRACE_INFO("MQTT battery metrics for %s overlay=%u: percent=%s raw=%s current=%s status=%s\r\n",
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               percent_valid ? "set" : "-",
               raw_valid ? "set" : "-",
               current_valid ? "set" : "-",
               status_valid ? status : "-");

    cJSON_Delete(json);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_metrics_headphones(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    char topic_common_name[32];
    if (!mqtt_extract_toniebox_topic_common_name(topic, "/metrics/headphones", topic_common_name, sizeof(topic_common_name)))
    {
        return NO_ERROR;
    }

    if (!conn->box_connection || conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, topic_common_name) != 0 ||
        conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring headphones metrics from non-matching MQTT connection topic=%s\r\n", topic);
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT headphones metrics payload is not valid JSON for %s\r\n", conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    bool speaker_output = false;
    bool_t speaker_output_valid = FALSE;
    cJSON *speaker = cJSON_GetObjectItemCaseSensitive(json, "speaker");
    if (cJSON_IsObject(speaker))
    {
        speaker_output_valid = mqtt_get_json_bool(speaker, "output", &speaker_output);
    }

    bool_t connected_valid = FALSE;
    uint32_t connected_count = 0;
    char *connected_json = NULL;
    cJSON *connected = cJSON_GetObjectItemCaseSensitive(json, "connected");
    if (cJSON_IsArray(connected))
    {
        connected_valid = TRUE;
        connected_count = (uint32_t)cJSON_GetArraySize(connected);
        connected_json = cJSON_PrintUnformatted(connected);
    }

    if (!speaker_output_valid && !connected_valid)
    {
        TRACE_WARNING("MQTT headphones metrics payload has no usable fields for %s\r\n", conn->client_ctx.settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    tbs_toniebox2_headphones_metrics(&conn->client_ctx,
                                     speaker_output_valid, speaker_output,
                                     connected_valid, connected_count,
                                     connected_json);

    TRACE_INFO("MQTT headphones metrics for %s overlay=%u: speakerOutput=%s connected=%s count=%u\r\n",
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               speaker_output_valid ? (speaker_output ? "true" : "false") : "-",
               connected_valid ? "set" : "-",
               (unsigned)connected_count);

    if (connected_json != NULL)
    {
        osFreeMem(connected_json);
    }
    cJSON_Delete(json);
    return NO_ERROR;
}

static bool_t mqtt_topic_matches_box_connection(MqttClientConnection *conn, const char *topic,
                                                const char *suffix, const char *label)
{
    char topic_common_name[32];
    if (!mqtt_extract_toniebox_topic_common_name(topic, suffix, topic_common_name, sizeof(topic_common_name)))
    {
        TRACE_WARNING("MQTT %s topic is invalid: %s\r\n", label, topic != NULL ? topic : "-");
        return FALSE;
    }

    if (conn == NULL || !conn->box_connection || conn->box_common_name[0] == '\0' ||
        osStrcmp(conn->box_common_name, topic_common_name) != 0 || conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring %s from non-matching MQTT connection topic=%s\r\n", label, topic);
        return FALSE;
    }

    return TRUE;
}

static error_t handle_mqtt_publish_volume_state(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                const uint8_t *payload, size_t payload_len)
{
    if (!mqtt_topic_matches_box_connection(conn, topic, "/volume/state", "volume-state"))
    {
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT volume-state payload is not valid JSON for %s\r\n", conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    uint32_t level = 0;
    if (!mqtt_get_json_uint32(json, "level", &level) || level > TBS_TB2_VOLUME_LEVEL_MAX)
    {
        TRACE_WARNING("MQTT volume-state payload has no usable level for %s\r\n", conn->client_ctx.settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    tbs_toniebox2_volume_state(&conn->client_ctx, level);
    TRACE_INFO("MQTT volume-state for %s overlay=%u: level=%u\r\n",
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               (unsigned)level);

    cJSON_Delete(json);
    return NO_ERROR;
}

static bool_t mqtt_match_app_control_ping(uint8_t overlay_id, const char *request_id, uint32_t *round_trip_ms)
{
    if (overlay_id >= MAX_OVERLAYS || request_id == NULL || round_trip_ms == NULL)
    {
        return FALSE;
    }

    MqttAppControlPingState *state = &app_control_ping_state[overlay_id];
    if (!state->valid || osStrcmp(state->request_id, request_id) != 0)
    {
        return FALSE;
    }

    *round_trip_ms = (uint32_t)(osGetSystemTime() - state->sent_at);
    state->valid = FALSE;
    return TRUE;
}

static error_t handle_mqtt_publish_app_reply_pong(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                  const uint8_t *payload, size_t payload_len)
{
    if (!mqtt_topic_matches_box_connection(conn, topic, "/app-reply/pong", "pong reply"))
    {
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT pong payload is not valid JSON for %s\r\n", conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(json, "requestId");
    if (!cJSON_IsString(request_id) || request_id->valuestring == NULL || request_id->valuestring[0] == '\0' ||
        osStrlen(request_id->valuestring) >= TBS_TB2_REQUEST_ID_MAX)
    {
        TRACE_WARNING("MQTT pong payload has no usable requestId for %s\r\n", conn->client_ctx.settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    uint32_t round_trip_ms = 0;
    bool_t round_trip_ms_valid = mqtt_match_app_control_ping(conn->client_ctx.settings->internal.overlayNumber,
                                                             request_id->valuestring, &round_trip_ms);
    tbs_toniebox2_pong(&conn->client_ctx, request_id->valuestring, round_trip_ms_valid, round_trip_ms);
    TRACE_INFO("MQTT pong for %s overlay=%u requestId=%s roundTripMs=%s%u\r\n",
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               request_id->valuestring,
               round_trip_ms_valid ? "" : "unmatched/",
               (unsigned)round_trip_ms);

    cJSON_Delete(json);
    return NO_ERROR;
}

typedef enum
{
    MQTT_TB2_SNAPSHOT_SETUP_STATUS,
    MQTT_TB2_SNAPSHOT_METRICS_EVENTS,
    MQTT_TB2_SNAPSHOT_METRICS_FLEET,
    MQTT_TB2_SNAPSHOT_ALARM_REPLY,
} MqttToniebox2SnapshotKind;

static error_t handle_mqtt_publish_json_snapshot(MqttClientConnection *conn, const char *topic,
                                                 const uint8_t *payload, size_t payload_len,
                                                 const char *suffix, const char *label,
                                                 MqttToniebox2SnapshotKind kind)
{
    if (!mqtt_topic_matches_box_connection(conn, topic, suffix, label))
    {
        return NO_ERROR;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT %s payload is not valid JSON for %s\r\n", label, conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    char *rendered = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (rendered == NULL)
    {
        TRACE_WARNING("MQTT %s payload could not be serialized for %s\r\n", label, conn->client_ctx.settings->commonName);
        return NO_ERROR;
    }

    char snapshot[TBS_TB2_RUNTIME_JSON_MAX];
    size_t rendered_len = osStrlen(rendered);
    bool_t truncated = rendered_len >= sizeof(snapshot);
    osStrncpy(snapshot, rendered, sizeof(snapshot) - 1);
    snapshot[sizeof(snapshot) - 1] = '\0';
    osFreeMem(rendered);

    switch (kind)
    {
    case MQTT_TB2_SNAPSHOT_SETUP_STATUS:
        tbs_toniebox2_setup_status(&conn->client_ctx, snapshot, truncated);
        break;
    case MQTT_TB2_SNAPSHOT_METRICS_EVENTS:
        tbs_toniebox2_metrics_events(&conn->client_ctx, snapshot, truncated);
        break;
    case MQTT_TB2_SNAPSHOT_METRICS_FLEET:
        tbs_toniebox2_metrics_fleet(&conn->client_ctx, snapshot, truncated);
        break;
    case MQTT_TB2_SNAPSHOT_ALARM_REPLY:
        tbs_toniebox2_alarm_reply(&conn->client_ctx, snapshot, truncated);
        break;
    }

    TRACE_INFO("MQTT %s for %s overlay=%u: stored=%" PRIuSIZE " bytes%s\r\n",
               label,
               conn->client_ctx.settings->commonName,
               (unsigned)conn->client_ctx.settings->internal.overlayNumber,
               osStrlen(snapshot),
               truncated ? " truncated" : "");
    return NO_ERROR;
}

static error_t handle_mqtt_publish_setup_status(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                const uint8_t *payload, size_t payload_len)
{
    return handle_mqtt_publish_json_snapshot(conn, topic, payload, payload_len, "/setup/status", "setup-status",
                                             MQTT_TB2_SNAPSHOT_SETUP_STATUS);
}

static error_t handle_mqtt_publish_metrics_events(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                  const uint8_t *payload, size_t payload_len)
{
    return handle_mqtt_publish_json_snapshot(conn, topic, payload, payload_len, "/metrics/events", "events metrics",
                                             MQTT_TB2_SNAPSHOT_METRICS_EVENTS);
}

static error_t handle_mqtt_publish_metrics_fleet(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                 const uint8_t *payload, size_t payload_len)
{
    return handle_mqtt_publish_json_snapshot(conn, topic, payload, payload_len, "/metrics/fleet", "fleet metrics",
                                             MQTT_TB2_SNAPSHOT_METRICS_FLEET);
}

static error_t handle_mqtt_publish_app_reply_alarm(MqttClientConnection *conn, MqttMessageType type, const char *topic,
                                                   const uint8_t *payload, size_t payload_len)
{
    return handle_mqtt_publish_json_snapshot(conn, topic, payload, payload_len, "/app-reply/alarm", "alarm reply",
                                             MQTT_TB2_SNAPSHOT_ALARM_REPLY);
}

static error_t handle_mqtt_publish_playback_state(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    char topic_common_name[32];
    if (!mqtt_extract_toniebox_topic_common_name(topic, "/playback/state", topic_common_name, sizeof(topic_common_name)))
    {
        return NO_ERROR;
    }

    if (!conn->box_connection || conn->box_common_name[0] == '\0' || osStrcmp(conn->box_common_name, topic_common_name) != 0 ||
        conn->client_ctx.settings == NULL)
    {
        TRACE_INFO("Ignoring playback-state from non-matching MQTT connection topic=%s\r\n", topic);
        return NO_ERROR;
    }

    settings_t *settings = conn->client_ctx.settings;
    cJSON *json = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (json == NULL)
    {
        TRACE_WARNING("MQTT playback-state payload is not valid JSON for %s\r\n", settings->commonName);
        return NO_ERROR;
    }

    cJSON *tonie = cJSON_GetObjectItemCaseSensitive(json, "tonie");
    if (cJSON_IsNull(tonie))
    {
        tbs_toniebox2_playback_state(&conn->client_ctx, NULL, false, 0, false, 0, false, 0, NULL);
        TRACE_INFO("MQTT playback-state for %s overlay=%u: stopped\r\n",
                   settings->commonName,
                   (unsigned)settings->internal.overlayNumber);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    if (!cJSON_IsString(tonie) || tonie->valuestring == NULL || tonie->valuestring[0] == '\0')
    {
        TRACE_WARNING("MQTT playback-state payload has no usable tonie field for %s\r\n", settings->commonName);
        cJSON_Delete(json);
        return NO_ERROR;
    }

    uint64_t content_version = 0;
    uint32_t chapter = 0;
    uint64_t chapter_until_ms = 0;
    char chapter_duration[TBS_TB2_PLAYBACK_DURATION_MAX];
    char content_version_text[24] = "-";
    char chapter_text[16] = "-";
    char chapter_until_ms_text[24] = "-";
    char chapter_duration_text[TBS_TB2_PLAYBACK_DURATION_MAX] = "-";
    chapter_duration[0] = '\0';

    bool_t content_version_valid = mqtt_get_json_u64(json, "contentVersion", &content_version);
    bool_t chapter_valid = mqtt_get_json_uint32(json, "chapter", &chapter);
    bool_t chapter_until_ms_valid = mqtt_get_json_u64(json, "chapterUntilMs", &chapter_until_ms);
    bool_t chapter_duration_valid = mqtt_get_json_decimal_text(json, "chapterDuration", chapter_duration, sizeof(chapter_duration));

    if (content_version_valid)
    {
        osSnprintf(content_version_text, sizeof(content_version_text), "%" PRIu64, content_version);
    }
    if (chapter_valid)
    {
        osSnprintf(chapter_text, sizeof(chapter_text), "%u", (unsigned)chapter);
    }
    if (chapter_until_ms_valid)
    {
        osSnprintf(chapter_until_ms_text, sizeof(chapter_until_ms_text), "%" PRIu64, chapter_until_ms);
    }
    if (chapter_duration_valid)
    {
        osStrncpy(chapter_duration_text, chapter_duration, sizeof(chapter_duration_text) - 1);
        chapter_duration_text[sizeof(chapter_duration_text) - 1] = '\0';
    }

    tbs_toniebox2_playback_state(&conn->client_ctx,
                                 tonie->valuestring,
                                 content_version_valid,
                                 content_version,
                                 chapter_valid,
                                 chapter,
                                 chapter_until_ms_valid,
                                 chapter_until_ms,
                                 chapter_duration_valid ? chapter_duration : NULL);

    TRACE_INFO("MQTT playback-state for %s overlay=%u: tonie=%s contentVersion=%s chapter=%s chapterUntilMs=%s chapterDuration=%s\r\n",
               settings->commonName,
               (unsigned)settings->internal.overlayNumber,
               tonie->valuestring,
               content_version_text,
               chapter_text,
               chapter_until_ms_text,
               chapter_duration_text);

    cJSON_Delete(json);
    return NO_ERROR;
}

static error_t handle_mqtt_publish_generic(MqttClientConnection *conn, MqttMessageType type, const char *topic, const uint8_t *payload, size_t payload_len)
{
    if (mqtt_should_log_full_payload(payload_len))
    {
        TRACE_INFO("PUBLISH topic='%s', payload=<full-capture> (QoS 0, len %zu)\r\n", topic, payload_len);
        return NO_ERROR;
    }

    char payload_str[256];
    size_t copy_len = payload_len < sizeof(payload_str) - 1 ? payload_len : sizeof(payload_str) - 1;
    memcpy(payload_str, payload, copy_len);
    payload_str[copy_len] = '\0';

    TRACE_INFO("PUBLISH topic='%s', payload='%s' (QoS 0, len %zu)\r\n", topic, payload_str, payload_len);
    return NO_ERROR;
}

static const MqttHandlerEntry mqtt_handlers[] = {
    {MQTT_MSG_CONNECT, NULL, &handle_mqtt_connect},
    {MQTT_MSG_PINGREQ, NULL, &handle_mqtt_pingreq},
    {MQTT_MSG_SUBSCRIBE, NULL, &handle_mqtt_subscribe},
    {MQTT_MSG_DISCONNECT, NULL, &handle_mqtt_disconnect},
    {MQTT_MSG_PUBLISH, "toniebox/+/logs", &handle_mqtt_publish_logs},
    {MQTT_MSG_PUBLISH, "toniebox/+/claim/+", &handle_mqtt_publish_claim},
    {MQTT_MSG_PUBLISH, "toniebox/+/settings/request", &handle_mqtt_publish_settings_request},
    {MQTT_MSG_PUBLISH, "toniebox/+/settings/confirm", &handle_mqtt_publish_settings_confirm},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/bedtime-state", &handle_mqtt_publish_app_reply_bedtime_state},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/pong", &handle_mqtt_publish_app_reply_pong},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/alarm", &handle_mqtt_publish_app_reply_alarm},
    {MQTT_MSG_PUBLISH, "toniebox/+/setup/status", &handle_mqtt_publish_setup_status},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/battery", &handle_mqtt_publish_metrics_battery},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/events", &handle_mqtt_publish_metrics_events},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/fleet", &handle_mqtt_publish_metrics_fleet},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/headphones", &handle_mqtt_publish_metrics_headphones},
    {MQTT_MSG_PUBLISH, "toniebox/+/playback/state", &handle_mqtt_publish_playback_state},
    {MQTT_MSG_PUBLISH, "toniebox/+/volume/state", &handle_mqtt_publish_volume_state},
    {MQTT_MSG_PUBLISH, NULL, &handle_mqtt_publish_generic}
};

static const MqttHandlerEntry mqtt_passthrough_observer_handlers[] = {
    {MQTT_MSG_PUBLISH, "toniebox/+/claim/+", &handle_mqtt_publish_claim},
    {MQTT_MSG_PUBLISH, "toniebox/+/settings/confirm", &handle_mqtt_publish_settings_confirm},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/bedtime-state", &handle_mqtt_publish_app_reply_bedtime_state},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/pong", &handle_mqtt_publish_app_reply_pong},
    {MQTT_MSG_PUBLISH, "toniebox/+/app-reply/alarm", &handle_mqtt_publish_app_reply_alarm},
    {MQTT_MSG_PUBLISH, "toniebox/+/setup/status", &handle_mqtt_publish_setup_status},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/battery", &handle_mqtt_publish_metrics_battery},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/events", &handle_mqtt_publish_metrics_events},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/fleet", &handle_mqtt_publish_metrics_fleet},
    {MQTT_MSG_PUBLISH, "toniebox/+/metrics/headphones", &handle_mqtt_publish_metrics_headphones},
    {MQTT_MSG_PUBLISH, "toniebox/+/playback/state", &handle_mqtt_publish_playback_state},
    {MQTT_MSG_PUBLISH, "toniebox/+/volume/state", &handle_mqtt_publish_volume_state},
};

static void mqtt_passthrough_observe_publish(void *context, bool_t box_to_upstream,
                                             const char *topic, const uint8_t *payload,
                                             size_t payload_len, uint8_t qos)
{
    MqttClientConnection *conn = context;
    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    mqtt_trace_full_publish(direction, topic, payload, payload_len, qos);

    size_t preview_len = payload_len < MQTT_LOG_INLINE_PAYLOAD_SIZE ? payload_len :
                                                                    MQTT_LOG_INLINE_PAYLOAD_SIZE;
    TRACE_DEBUG("MQTT PASSTHROUGH PUBLISH dir=%s topic='%s' qos=%u payload='%.*s'%s len=%" PRIuSIZE "\r\n",
                direction, topic != NULL ? topic : "-", (unsigned)qos,
                (int)preview_len, payload != NULL ? (const char *)payload : "",
                payload_len > preview_len ? "..." : "", payload_len);

    if (!box_to_upstream || conn == NULL)
    {
        return;
    }
    mqtt_connection_update_context(conn, topic);
    for (size_t i = 0; i < sizeof(mqtt_passthrough_observer_handlers) /
                            sizeof(mqtt_passthrough_observer_handlers[0]); i++)
    {
        const MqttHandlerEntry *entry = &mqtt_passthrough_observer_handlers[i];
        if (mqtt_topic_match(entry->topic, topic))
        {
            error_t error = entry->handler(conn, MQTT_MSG_PUBLISH, topic, payload, payload_len);
            if (error)
            {
                TRACE_WARNING("MQTT passthrough observer handler failed topic='%s' error=%s code=%d\r\n",
                              topic, error2text(error), (int)error);
            }
            break;
        }
    }
}

void mqtt_server_task()
{
    if (serverSocket == NULL)
        return;

    // 1. Process active connections
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (conn->active)
        {
            size_t received = 0;
            error_t error = NO_ERROR;

            if (conn->passthrough != NULL)
            {
                error = tb2_mqtt_passthrough_task(conn->passthrough);
                if (error)
                {
                    const bool_t clean = error == ERROR_END_OF_STREAM || error == ERROR_ABORTED;
                    const char *result = error == ERROR_ABORTED ? "disabled" :
                                         error == ERROR_END_OF_STREAM ? "completed" :
                                         error == ERROR_WRITE_FAILED ? "capture_or_write_failed" :
                                         "stream_failed";
                    tb2_mqtt_passthrough_close(conn->passthrough, result, clean);
                    conn->passthrough = NULL;
                    mqtt_connection_close(conn, result);
                }
                continue;
            }

            // Non-blocking check for data
            if (tcpWaitForEvents(conn->socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY)
            {
                if (conn->tlsContext)
                {
                    error = tlsRead(conn->tlsContext, conn->buffer + conn->buffer_len, MQTT_MAX_PACKET_SIZE - conn->buffer_len, &received, 0);
                }
                else
                {
                    error = socketReceive(conn->socket, conn->buffer + conn->buffer_len, MQTT_MAX_PACKET_SIZE - conn->buffer_len, &received, 0);
                }

                if (error == NO_ERROR && received > 0)
                {
                    conn->buffer_len += received;

                    if (!conn->mode_decided)
                    {
                        conn->mode_decided = TRUE;
                        if (conn->tlsContext != NULL && tb2_mqtt_passthrough_is_armed())
                        {
                            bool_t handled = FALSE;
                            settings_t *passthrough_box_settings = NULL;
                            error = tb2_mqtt_passthrough_start(conn->tlsContext, conn->socket,
                                                               &conn->passthrough, &handled,
                                                               mqtt_passthrough_observe_publish, conn,
                                                               &passthrough_box_settings);
                            if (!error && handled)
                            {
                                if (passthrough_box_settings != NULL)
                                {
                                    mqtt_promote_connection_to_box(conn, passthrough_box_settings,
                                                                   passthrough_box_settings->commonName,
                                                                   "passthrough certificate");
                                }
                                error = tb2_mqtt_passthrough_forward_initial(conn->passthrough,
                                                                             conn->buffer,
                                                                             conn->buffer_len);
                            }
                            if (handled)
                            {
                                conn->buffer_len = 0;
                                if (error)
                                {
                                    if (conn->passthrough != NULL)
                                    {
                                        tb2_mqtt_passthrough_close(conn->passthrough,
                                                                   "initial_forward_failed", FALSE);
                                        conn->passthrough = NULL;
                                    }
                                    mqtt_connection_close(conn, "MQTT passthrough start failed");
                                }
                                continue;
                            }
                        }
                    }

                    size_t processed_total = 0;

                    while (processed_total + 2 <= conn->buffer_len)
                    {
                        uint8_t *pkt = &conn->buffer[processed_total];
                        size_t remaining_len = 0;
                        size_t multiplier = 1;
                        size_t pos = 1;
                        uint8_t digit;
                        bool rem_len_complete = false;

                        do
                        {
                            if (processed_total + pos >= conn->buffer_len) break;
                            digit = pkt[pos++];
                            remaining_len += (digit & 127) * multiplier;
                            multiplier *= 128;
                            if ((digit & 128) == 0) rem_len_complete = true;
                        } while (!rem_len_complete && pos < 5);

                        if (!rem_len_complete) break; // Need more bytes for remaining_len

                        size_t packet_size = pos + remaining_len;
                        if (processed_total + packet_size > conn->buffer_len) break; // Incomplete packet

                        // Process full packet using the pointer-based routing system
                        uint8_t cmd_raw = pkt[0];
                        uint8_t cmd = cmd_raw & 0xF0;

                        if (cmd == 0x30) // PUBLISH
                        {
                            uint8_t flags = cmd_raw & 0x0F;
                            uint8_t qos = (flags >> 1) & 0x03;
                            size_t p = pos;

                            size_t topic_len = (pkt[p] << 8) | pkt[p + 1];
                            p += 2;
                            char topic[256];
                            size_t copy_len = topic_len < sizeof(topic) - 1 ? topic_len : sizeof(topic) - 1;
                            memcpy(topic, &pkt[p], copy_len);
                            topic[copy_len] = '\0';
                            p += topic_len;

                            uint16_t packet_id = 0;
                            if (qos > 0)
                            {
                                packet_id = (pkt[p] << 8) | pkt[p + 1];
                                p += 2;
                            }

                            size_t payload_len = remaining_len - (p - pos);
                            const uint8_t *payload = pkt + p;

                            mqtt_trace_full_publish("rx", topic, payload, payload_len, qos);
                            mqtt_connection_update_context(conn, topic);

                            // Find matching handler for this PUBLISH topic
                            bool handled = false;
                            for (size_t h = 0; h < sizeof(mqtt_handlers) / sizeof(mqtt_handlers[0]); h++)
                            {
                                if (mqtt_handlers[h].type == MQTT_MSG_PUBLISH)
                                {
                                    if (mqtt_handlers[h].topic == NULL || mqtt_topic_match(mqtt_handlers[h].topic, topic))
                                    {
                                        mqtt_handlers[h].handler(conn, MQTT_MSG_PUBLISH, topic, payload, payload_len);
                                        handled = true;
                                        break;
                                    }
                                }
                            }

                            if (!conn->active)
                            {
                                break;
                            }

                            if (!handled)
                            {
                                TRACE_WARNING("No handler matched for PUBLISH topic='%s'\r\n", topic);
                            }

                            if (qos == 1)
                            {
                                uint8_t puback[] = {0x40, 0x02, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFF)};
                                size_t written = 0;
                                if (conn->tlsContext)
                                    tlsWrite(conn->tlsContext, puback, sizeof(puback), &written, 0);
                                else
                                    socketSend(conn->socket, puback, sizeof(puback), &written, 0);
                            }
                        }
                        else
                        {
                            // Route non-PUBLISH commands (CONNECT, SUBSCRIBE, PINGREQ, DISCONNECT)
                            bool handled = false;
                            for (size_t h = 0; h < sizeof(mqtt_handlers) / sizeof(mqtt_handlers[0]); h++)
                            {
                                if (mqtt_handlers[h].type == (MqttMessageType)cmd)
                                {
                                    mqtt_handlers[h].handler(conn, (MqttMessageType)cmd, NULL, pkt + pos, remaining_len);
                                    handled = true;
                                    break;
                                }
                            }

                            if (!conn->active)
                            {
                                break;
                            }

                            if (!handled)
                            {
                                TRACE_INFO("Unknown or unhandled command 0x%02X received (%zu bytes total)\r\n", cmd_raw, packet_size);
                            }

                            if (cmd == 0xE0) // DISCONNECT
                            {
                                break; // Exit this connection's packet processing loop
                            }
                        }

                        processed_total += packet_size;
                    }

                    if (processed_total > 0 && conn->active)
                    {
                        conn->buffer_len -= processed_total;
                        if (conn->buffer_len > 0)
                        {
                            memmove(conn->buffer, &conn->buffer[processed_total], conn->buffer_len);
                        }
                    }
                }
                else if (error != ERROR_WOULD_BLOCK && error != NO_ERROR && error != ERROR_TIMEOUT)
                {
                    TRACE_INFO("Connection closed (error: %s)\r\n", error2text(error));
                    mqtt_connection_close(conn, error2text(error));
                }
            }
            if (conn->active)
            {
                mqtt_touch_box_connection(conn);
                mqtt_publish_pending_settings_desired_to_connection(conn, TRUE, FALSE);
                if (conn->box_connection && conn->client_ctx.settings != NULL && conn->client_ctx.settings->internal.freshnessCacheChanged)
                {
                    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(conn->client_ctx.settings);
                    if (mqtt_fresh_tonies_background_payload_already_sent(conn->client_ctx.settings))
                    {
                        continue;
                    }
                    if (state == NULL || !state->pending)
                    {
                        mqtt_mark_fresh_tonies_pending(conn->client_ctx.settings, "background");
                    }
                    mqtt_publish_fresh_tonies_to_connection(conn, conn->client_ctx.settings, TRUE, FALSE);
                }
            }
        }
    }

    // 2. Non-blocking check for new connections
    if (tcpWaitForEvents(serverSocket, SOCKET_EVENT_ACCEPT, 0) & SOCKET_EVENT_ACCEPT)
    {
        IpAddr clientIpAddr;
        uint16_t clientPort;
        Socket *clientSocket = socketAccept(serverSocket, &clientIpAddr, &clientPort);
        if (clientSocket != NULL)
        {
            TRACE_INFO("Accepted connection from %s:%u\r\n", ipAddrToString(&clientIpAddr, NULL), clientPort);

            MqttClientConnection *conn = NULL;
            for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
            {
                if (!connections[i].active)
                {
                    conn = &connections[i];
                    break;
                }
            }

            if (conn != NULL)
            {
                osMemset(conn, 0, sizeof(MqttClientConnection));
                conn->socket = clientSocket;
                conn->active = true;
                conn->buffer_len = 0;
                conn->subscription_count = 0;
                socketSetTimeout(conn->socket, 0);

                conn->client_ctx.settings = get_settings();
                conn->client_ctx.settingsNoOverlay = conn->client_ctx.settings;
                conn->client_ctx.state = get_toniebox_state();
                conn->client_ctx.mqtt_connection = conn;

                conn->tlsContext = tlsInit();
                if (conn->tlsContext != NULL)
                {
                    if (mqtt_server_tls_init(conn->tlsContext) == NO_ERROR)
                    {
                        tlsSetSocket(conn->tlsContext, conn->socket);
                    }
                    else
                    {
                        TRACE_ERROR("TLS init failed\r\n");
                        tlsFree(conn->tlsContext);
                        conn->tlsContext = NULL;
                    }
                }
            }
            else
            {
                TRACE_WARNING("No free MQTT connection slots, closing connection\r\n");
                socketClose(clientSocket);
            }
        }
    }
}

void mqtt_server_deinit() {
    if (serverSocket != NULL) {
        socketClose(serverSocket);
        serverSocket = NULL;
    }
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        if (connections[i].active)
        {
            mqtt_connection_close(&connections[i], "server shutdown");
        }
    }

    if (mqtt_server_cert)
    {
        osFreeMem(mqtt_server_cert);
        mqtt_server_cert = NULL;
    }
    if (mqtt_server_key)
    {
        osFreeMem(mqtt_server_key);
        mqtt_server_key = NULL;
    }
}

static bool_t mqtt_mark_toniebox2_settings_pending(uint8_t overlay_id, const char *setting_name)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (!mqtt_is_toniebox2_overlay(settings))
    {
        return FALSE;
    }

    size_t marked = 0;
    bool_t pending = mqtt_toniebox2_settings_mark_pending(settings, setting_name, &marked);
    if (pending && marked > 0)
    {
        TRACE_INFO("Marked %" PRIuSIZE " TB2 desired setting(s) pending for %s overlay=%u\r\n",
                   marked,
                   settings->commonName,
                   (unsigned)overlay_id);
    }
    return pending;
}

bool_t mqtt_server_publish_toniebox2_settings_desired_for_overlay(uint8_t overlay_id)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (settings == NULL || !settings->internal.config_used || !settings->internal.toniebox2SettingsDesiredPending)
    {
        return FALSE;
    }

    bool_t published = FALSE;
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (conn->active &&
            conn->box_connection &&
            conn->client_ctx.settings != NULL &&
            conn->client_ctx.settings->internal.overlayNumber == overlay_id)
        {
            if (mqtt_publish_pending_settings_desired_to_connection(conn, TRUE, TRUE))
            {
                published = TRUE;
            }
        }
    }

    if (published)
    {
        TRACE_INFO("MQTT settings desired queued for confirmation on %s\r\n", settings->commonName);
    }
    else
    {
        TRACE_INFO("MQTT settings desired pending for %s, no subscribed box connection active\r\n", settings->commonName);
    }
    return published;
}

void mqtt_server_mark_toniebox2_setting_changed(uint8_t overlay_id, const char *setting_name)
{
    if (overlay_id == 0)
    {
        for (uint8_t i = 1; i < MAX_OVERLAYS; i++)
        {
            settings_t *settings = get_settings_id(i);
            if (mqtt_is_toniebox2_overlay(settings))
            {
                if (mqtt_mark_toniebox2_settings_pending(i, setting_name))
                {
                    mqtt_server_publish_toniebox2_settings_desired_for_overlay(i);
                }
            }
        }
        return;
    }

    settings_t *settings = get_settings_id(overlay_id);
    if (mqtt_is_toniebox2_overlay(settings))
    {
        if (mqtt_mark_toniebox2_settings_pending(overlay_id, setting_name))
        {
            mqtt_server_publish_toniebox2_settings_desired_for_overlay(overlay_id);
        }
    }
}

void mqtt_server_mark_toniebox2_settings_changed(uint8_t overlay_id)
{
    mqtt_server_mark_toniebox2_setting_changed(overlay_id, NULL);
}

static void mqtt_record_app_control_stl(uint8_t overlay_id, const char *payload)
{
    if (overlay_id >= MAX_OVERLAYS || payload == NULL)
    {
        return;
    }

    MqttAppControlStlState *state = &app_control_stl_state[overlay_id];
    state->valid = TRUE;
    state->sent_at = (uint32_t)time(NULL);
    state->sequence++;
    if (state->sequence == 0)
    {
        state->sequence = 1;
    }
    state->payload_hash = mqtt_payload_hash(payload);
}

static bool_t mqtt_server_publish_app_control_for_overlay(uint8_t overlay_id, const char *command, const char *payload_json, bool_t record_stl)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (!mqtt_is_toniebox2_overlay(settings))
    {
        return FALSE;
    }
    if (!mqtt_validate_json_payload(payload_json, command))
    {
        return FALSE;
    }

    char topic[128];
    if (!mqtt_app_control_topic(settings, command, topic, sizeof(topic)))
    {
        return FALSE;
    }

    bool_t published = FALSE;
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (mqtt_connection_matches_box_overlay(conn, settings) && mqtt_connection_has_exact_sub(conn, topic))
        {
            if (mqtt_connection_publish(conn, topic, payload_json))
            {
                published = TRUE;
            }
        }
    }

    if (published)
    {
        if (record_stl)
        {
            mqtt_record_app_control_stl(settings->internal.overlayNumber, payload_json);
        }
        TRACE_INFO("MQTT app-control/%s sent for %s\r\n", command, settings->commonName);
    }
    else
    {
        TRACE_INFO("MQTT app-control/%s not sent for %s, no subscribed box connection active\r\n", command, settings->commonName);
    }

    return published;
}

static bool_t mqtt_server_has_app_control_subscription(uint8_t overlay_id, const char *command)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (!mqtt_is_toniebox2_overlay(settings))
    {
        return FALSE;
    }

    char topic[128];
    if (!mqtt_app_control_topic(settings, command, topic, sizeof(topic)))
    {
        return FALSE;
    }

    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (mqtt_connection_matches_box_overlay(conn, settings) && mqtt_connection_has_exact_sub(conn, topic))
        {
            return TRUE;
        }
    }

    return FALSE;
}

bool_t mqtt_server_has_playback_control(uint8_t overlay_id)
{
    return mqtt_server_has_app_control_subscription(overlay_id, "playback");
}

bool_t mqtt_server_has_volume_control(uint8_t overlay_id)
{
    return mqtt_server_has_app_control_subscription(overlay_id, "volume");
}

bool_t mqtt_server_has_ping_control(uint8_t overlay_id)
{
    return mqtt_server_has_app_control_subscription(overlay_id, "ping");
}

static const char *mqtt_server_playback_action_name(mqtt_server_playback_action_t action)
{
    switch (action)
    {
    case MQTT_SERVER_PLAYBACK_START:
        return "start";
    case MQTT_SERVER_PLAYBACK_PAUSE:
        return "pause";
    case MQTT_SERVER_PLAYBACK_NEXT:
        return "next";
    case MQTT_SERVER_PLAYBACK_PREV:
        return "prev";
    case MQTT_SERVER_PLAYBACK_RESTART:
        return "restart";
    default:
        return NULL;
    }
}

static void mqtt_server_record_playback_command_state(uint8_t overlay_id,
                                                      toniebox_state_tb2_playback_status_t status)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (!mqtt_is_toniebox2_overlay(settings))
    {
        return;
    }

    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (mqtt_connection_matches_box_overlay(conn, settings))
        {
            tbs_toniebox2_playback_command_state(&conn->client_ctx, status);
            return;
        }
    }
}

bool_t mqtt_server_publish_playback_for_overlay(uint8_t overlay_id, mqtt_server_playback_action_t action)
{
    const char *action_name = mqtt_server_playback_action_name(action);
    if (action_name == NULL)
    {
        return FALSE;
    }

    char payload[48];
    osSnprintf(payload, sizeof(payload), "{\"action\":\"%s\"}", action_name);
    bool_t published = mqtt_server_publish_app_control_for_overlay(overlay_id, "playback", payload, FALSE);
    if (published && action == MQTT_SERVER_PLAYBACK_PAUSE)
    {
        mqtt_server_record_playback_command_state(overlay_id, TBS_TB2_PLAYBACK_STATUS_PAUSED);
    }
    else if (published && action == MQTT_SERVER_PLAYBACK_START)
    {
        mqtt_server_record_playback_command_state(overlay_id, TBS_TB2_PLAYBACK_STATUS_PLAYING);
    }
    return published;
}

bool_t mqtt_server_publish_playback_position_for_overlay(uint8_t overlay_id, uint32_t chapter, uint32_t position_ms)
{
    if (chapter >= TONIEFILE_MAX_CHAPTERS)
    {
        return FALSE;
    }

    char payload[96];
    osSnprintf(payload, sizeof(payload),
               "{\"action\":\"setPosition\",\"chapter\":%" PRIu32 ",\"ms\":%" PRIu32 "}",
               chapter, position_ms);
    return mqtt_server_publish_app_control_for_overlay(overlay_id, "playback", payload, FALSE);
}

bool_t mqtt_server_publish_volume_for_overlay(uint8_t overlay_id, uint32_t level)
{
    if (level > TBS_TB2_VOLUME_LEVEL_MAX)
    {
        return FALSE;
    }

    char payload[32];
    osSnprintf(payload, sizeof(payload), "{\"level\":%" PRIu32 "}", level);
    return mqtt_server_publish_app_control_for_overlay(overlay_id, "volume", payload, FALSE);
}

bool_t mqtt_server_publish_ping_for_overlay(uint8_t overlay_id, char *request_id, size_t request_id_size)
{
    if (overlay_id >= MAX_OVERLAYS || request_id == NULL || request_id_size < TBS_TB2_REQUEST_ID_MAX)
    {
        return FALSE;
    }

    MqttAppControlPingState *state = &app_control_ping_state[overlay_id];
    uint32_t sequence = state->sequence + 1;
    if (sequence == 0)
    {
        sequence = 1;
    }

    osSnprintf(request_id, request_id_size, "teddycloud-%" PRIu32, sequence);
    char payload[96];
    osSnprintf(payload, sizeof(payload), "{\"requestId\":\"%s\"}", request_id);
    if (!mqtt_server_publish_app_control_for_overlay(overlay_id, "ping", payload, FALSE))
    {
        request_id[0] = '\0';
        return FALSE;
    }

    state->valid = TRUE;
    state->sequence = sequence;
    state->sent_at = osGetSystemTime();
    osStrncpy(state->request_id, request_id, sizeof(state->request_id) - 1);
    state->request_id[sizeof(state->request_id) - 1] = '\0';
    return TRUE;
}

bool_t mqtt_server_publish_app_control_stl_for_overlay(uint8_t overlay_id, const char *payload_json)
{
    return mqtt_server_publish_app_control_for_overlay(overlay_id, "stl", payload_json, TRUE);
}

static void mqtt_uid_to_ruid(uint64_t uid, char ruid[17])
{
    osSnprintf(ruid, 17, "%016" PRIX64, bswap_64(uid));
}

static bool_t mqtt_fresh_tonies_topic(settings_t *settings, char *topic, size_t topic_size)
{
    if (settings == NULL || settings->commonName == NULL || settings->commonName[0] == '\0')
    {
        return FALSE;
    }

    osSnprintf(topic, topic_size, "toniebox/%s/fresh-tonies", settings->commonName);
    return TRUE;
}

static MqttFreshToniesPublishState *mqtt_fresh_tonies_publish_state(settings_t *settings)
{
    if (settings == NULL || settings->internal.overlayNumber >= MAX_OVERLAYS)
    {
        return NULL;
    }

    return &fresh_tonies_publish_state[settings->internal.overlayNumber];
}

static void mqtt_mark_fresh_tonies_pending(settings_t *settings, const char *reason)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL)
    {
        return;
    }

    uint32_t now = (uint32_t)time(NULL);
    if (!state->pending)
    {
        state->pending = TRUE;
        state->pending_since = now;
        state->coalesced_count = 0;
    }
    state->coalesced_count++;
    osStrncpy(state->reason, reason != NULL && reason[0] != '\0' ? reason : "unknown", sizeof(state->reason) - 1);
    state->reason[sizeof(state->reason) - 1] = '\0';
}

static void mqtt_clear_fresh_tonies_pending(settings_t *settings)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL)
    {
        return;
    }

    state->pending = FALSE;
    state->pending_since = 0;
    state->coalesced_count = 0;
    state->reason[0] = '\0';
}

static bool_t mqtt_fresh_tonies_active_playback_in_cache(settings_t *settings, const uint64_t *freshness_cache,
                                                         size_t freshness_cache_len, char active_ruid[17])
{
    if (settings == NULL || freshness_cache == NULL || active_ruid == NULL || settings->internal.overlayNumber >= MAX_OVERLAYS)
    {
        return FALSE;
    }

    toniebox_state_t *box_state = get_toniebox_state_id(settings->internal.overlayNumber);
    if (box_state == NULL || !box_state->playback_state.valid || !box_state->playback_state.ruid_valid)
    {
        return FALSE;
    }

    osStrncpy(active_ruid, box_state->playback_state.ruid, 16);
    active_ruid[16] = '\0';
    for (size_t i = 0; i < freshness_cache_len; i++)
    {
        char cache_ruid[17];
        mqtt_uid_to_ruid(freshness_cache[i], cache_ruid);
        if (osStrcasecmp(cache_ruid, active_ruid) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static bool_t mqtt_fresh_tonies_can_publish(settings_t *settings, const uint64_t *freshness_cache,
                                            size_t freshness_cache_len, char active_ruid[17])
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL || !state->pending)
    {
        return FALSE;
    }

    uint32_t now = (uint32_t)time(NULL);
    if (now >= state->pending_since && (now - state->pending_since) < MQTT_FRESH_TONIES_DEBOUNCE_SEC)
    {
        if (state->last_delay_log_at != now)
        {
            TRACE_INFO("MQTT fresh-tonies coalesced for %s reason=%s coalesced=%u debounce=%u\r\n",
                       settings->commonName,
                       state->reason[0] != '\0' ? state->reason : "unknown",
                       (unsigned)state->coalesced_count,
                       (unsigned)MQTT_FRESH_TONIES_DEBOUNCE_SEC);
            state->last_delay_log_at = now;
        }
        return FALSE;
    }

    active_ruid[0] = '\0';
    if (mqtt_fresh_tonies_active_playback_in_cache(settings, freshness_cache, freshness_cache_len, active_ruid))
    {
        if (state->last_delay_log_at != now)
        {
            TRACE_INFO("MQTT fresh-tonies held for %s reason=%s activePlayback=%s coalesced=%u\r\n",
                       settings->commonName,
                       state->reason[0] != '\0' ? state->reason : "unknown",
                       active_ruid,
                       (unsigned)state->coalesced_count);
            state->last_delay_log_at = now;
        }
        return FALSE;
    }

    return TRUE;
}

static void mqtt_finish_fresh_tonies_publish(settings_t *settings, size_t freshness_cache_len, const char *active_ruid)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL)
    {
        return;
    }

    uint32_t now = (uint32_t)time(NULL);
    TRACE_INFO("MQTT fresh-tonies invalidation sent for %s reason=%s entries=%" PRIuSIZE " coalesced=%u activePlayback=%s at=%u, keeping pending until content request\r\n",
               settings->commonName,
               state->reason[0] != '\0' ? state->reason : "unknown",
               freshness_cache_len,
               (unsigned)state->coalesced_count,
               active_ruid != NULL && active_ruid[0] != '\0' ? active_ruid : "-",
               (unsigned)now);
    state->last_publish_at = now;
    state->last_delay_log_at = 0;
    mqtt_clear_fresh_tonies_pending(settings);
}

static char *mqtt_build_fresh_tonies_payload(settings_t *settings)
{
    if (settings == NULL)
    {
        return NULL;
    }

    size_t freshnessCacheLen = 0;
    uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    if (freshnessCache == NULL || freshnessCacheLen == 0)
    {
        return NULL;
    }

    cJSON *array = cJSON_CreateArray();
    if (array == NULL)
    {
        return NULL;
    }

    size_t unique_count = 0;
    bool_t truncated = FALSE;
    for (size_t i = 0; i < freshnessCacheLen; i++)
    {
        bool_t duplicate = FALSE;
        for (size_t j = 0; j < i; j++)
        {
            if (freshnessCache[j] == freshnessCache[i])
            {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        if (unique_count >= MQTT_FRESH_TONIES_MAX)
        {
            truncated = TRUE;
            continue;
        }

        char ruidStr[17];
        mqtt_uid_to_ruid(freshnessCache[i], ruidStr);
        cJSON *ruidItem = cJSON_CreateString(ruidStr);
        if (ruidItem == NULL || !cJSON_AddItemToArray(array, ruidItem))
        {
            cJSON_Delete(ruidItem);
            cJSON_Delete(array);
            return NULL;
        }
        unique_count++;
    }

    if (truncated)
    {
        TRACE_WARNING("V3 freshness MQTT payload truncated to %u entries\r\n", MQTT_FRESH_TONIES_MAX);
    }

    char *payload = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return payload;
}

static bool_t mqtt_fresh_tonies_payload_matches_last(settings_t *settings, const char *payload, uint32_t *payload_hash, size_t *payload_len)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL || payload == NULL || !state->last_payload_valid)
    {
        return FALSE;
    }

    uint32_t hash = mqtt_payload_hash(payload);
    size_t len = osStrlen(payload);
    if (payload_hash != NULL)
    {
        *payload_hash = hash;
    }
    if (payload_len != NULL)
    {
        *payload_len = len;
    }

    return state->last_payload_hash == hash && state->last_payload_len == len;
}

static void mqtt_record_fresh_tonies_payload(settings_t *settings, const char *payload)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL || payload == NULL)
    {
        return;
    }

    state->last_payload_hash = mqtt_payload_hash(payload);
    state->last_payload_len = osStrlen(payload);
    state->last_payload_valid = TRUE;
}

static bool_t mqtt_fresh_tonies_background_payload_already_sent(settings_t *settings)
{
    MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
    if (state == NULL || !state->last_payload_valid)
    {
        return FALSE;
    }

    char *payload = mqtt_build_fresh_tonies_payload(settings);
    if (payload == NULL)
    {
        return FALSE;
    }

    uint32_t payload_hash = 0;
    size_t payload_len = 0;
    bool_t duplicate = mqtt_fresh_tonies_payload_matches_last(settings, payload, &payload_hash, &payload_len);
    osFreeMem(payload);
    if (!duplicate)
    {
        return FALSE;
    }

    uint32_t now = (uint32_t)time(NULL);
    if (state->last_duplicate_log_at == 0 ||
        now < state->last_duplicate_log_at ||
        (now - state->last_duplicate_log_at) >= MQTT_FRESH_TONIES_DUPLICATE_LOG_INTERVAL_SEC)
    {
        TRACE_INFO("MQTT fresh-tonies background duplicate suppressed for %s hash=%08" PRIX32 " len=%" PRIuSIZE "\r\n",
                   settings->commonName,
                   payload_hash,
                   payload_len);
        state->last_duplicate_log_at = now;
    }

    mqtt_clear_fresh_tonies_pending(settings);
    return TRUE;
}

static bool_t mqtt_publish_fresh_tonies_to_connection(MqttClientConnection *conn, settings_t *settings, bool_t finish_on_success, bool_t allow_duplicate)
{
    if (!mqtt_connection_matches_box_overlay(conn, settings))
    {
        return FALSE;
    }

    char topic[128];
    if (!mqtt_fresh_tonies_topic(settings, topic, sizeof(topic)))
    {
        return FALSE;
    }
    if (!mqtt_connection_has_sub(conn, topic))
    {
        return FALSE;
    }

    size_t freshnessCacheLen = 0;
    uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    if (freshnessCacheLen == 0)
    {
        settings_set_bool_id("internal.freshnessCacheChanged", false, settings->internal.overlayNumber);
        mqtt_clear_fresh_tonies_pending(settings);
        return TRUE;
    }

    char active_ruid[17];
    if (!mqtt_fresh_tonies_can_publish(settings, freshnessCache, freshnessCacheLen, active_ruid))
    {
        return FALSE;
    }

    char *payload = mqtt_build_fresh_tonies_payload(settings);
    if (payload == NULL)
    {
        return FALSE;
    }
    if (!allow_duplicate && mqtt_fresh_tonies_payload_matches_last(settings, payload, NULL, NULL))
    {
        osFreeMem(payload);
        mqtt_clear_fresh_tonies_pending(settings);
        return TRUE;
    }

    bool_t published = mqtt_connection_publish(conn, topic, payload);
    if (published)
    {
        mqtt_record_fresh_tonies_payload(settings, payload);
    }
    osFreeMem(payload);
    if (published && finish_on_success)
    {
        mqtt_finish_fresh_tonies_publish(settings, freshnessCacheLen, active_ruid);
    }
    return published;
}

bool_t mqtt_server_publish_fresh_tonies(client_ctx_t *client_ctx)
{
    if (client_ctx == NULL || client_ctx->settings == NULL || client_ctx->mqtt_connection == NULL)
    {
        return FALSE;
    }
    if (!client_ctx->settings->internal.freshnessCacheChanged)
    {
        return FALSE;
    }

    mqtt_mark_fresh_tonies_pending(client_ctx->settings, "connection");
    return mqtt_publish_fresh_tonies_to_connection((MqttClientConnection *)client_ctx->mqtt_connection, client_ctx->settings, TRUE, TRUE);
}

bool_t mqtt_server_publish_fresh_tonies_for_overlay(uint8_t overlay_id)
{
    settings_t *settings = get_settings_id(overlay_id);
    if (settings == NULL || !settings->internal.config_used || !settings->internal.freshnessCacheChanged)
    {
        return FALSE;
    }

    size_t freshnessCacheLen = 0;
    settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    if (freshnessCacheLen == 0)
    {
        settings_set_bool_id("internal.freshnessCacheChanged", false, settings->internal.overlayNumber);
        mqtt_clear_fresh_tonies_pending(settings);
        return TRUE;
    }

    mqtt_mark_fresh_tonies_pending(settings, "overlay");

    bool_t published = FALSE;
    for (size_t i = 0; i < MQTT_MAX_CONNECTIONS; i++)
    {
        MqttClientConnection *conn = &connections[i];
        if (mqtt_publish_fresh_tonies_to_connection(conn, settings, FALSE, TRUE))
        {
            published = TRUE;
        }
    }

    if (published)
    {
        mqtt_finish_fresh_tonies_publish(settings, freshnessCacheLen, NULL);
    }
    else
    {
        MqttFreshToniesPublishState *state = mqtt_fresh_tonies_publish_state(settings);
        TRACE_INFO("MQTT fresh-tonies invalidation queued for %s reason=%s coalesced=%u, not sent yet\r\n",
                   settings->commonName,
                   state != NULL && state->reason[0] != '\0' ? state->reason : "unknown",
                   state != NULL ? (unsigned)state->coalesced_count : 0);
    }
    return published;
}
