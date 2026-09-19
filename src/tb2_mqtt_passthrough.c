#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "compiler_port.h"
#include "core/net.h"
#include "core/socket.h"
#include "core/tcp.h"
#include "core/tcp_misc.h"
#include "date_time.h"
#include "debug.h"
#include "encoding/base64.h"
#include "fs_ext.h"
#include "fs_port.h"
#include "handler.h"
#include "http/http_client.h"
#include "http/http_client_transport.h"
#include "http/http_server_misc.h"
#include "mqtt_forward_filter.h"
#include "mqtt_nocloud_filter.h"
#include "mqtt_response_history.h"
#include <stdatomic.h>
#include "mutex_manager.h"
#include "os_ext.h"
#include "os_port.h"
#include "platform.h"
#include "rand.h"
#include "server_helpers.h"
#include "settings.h"
#include "tb2_mqtt_passthrough.h"
#include "tb2_mqtt_upstream.h"
#include "tls.h"
#include "tls_adapter.h"

#define TB2_MQTT_TUNNEL_BUFFER_SIZE 16384
#define TB2_MQTT_TUNNEL_IO_TIMEOUT_MS 500
#define TB2_MQTT_CAPTURE_MIB_BYTES (1024ULL * 1024ULL)
#define TB2_MQTT_MAX_REMAINING_LENGTH 268435455U
#define TB2_MQTT_PACKET_PUBLISH 3U
#define TB2_MQTT_PACKET_PUBACK 4U
#define TB2_MQTT_PACKET_PUBREC 5U
#define TB2_MQTT_PACKET_PUBREL 6U
#define TB2_MQTT_PACKET_PUBCOMP 7U
#define TB2_MQTT_PACKET_SUBSCRIBE 8U
#define TB2_MQTT_PACKET_UNSUBSCRIBE 10U
#define TB2_MQTT_LOCAL_RESPONSE_HISTORY_MAX 32U

typedef struct
{
    uint8_t *data;
    size_t length;
    size_t capacity;
} tb2_mqtt_stream_t;

typedef struct tb2_mqtt_qos2_entry
{
    uint16_t packet_id;
    bool_t completed;
    bool_t count_blocked;
    const char *filter_id;
    const char *capture_action;
    struct tb2_mqtt_qos2_entry *next;
} tb2_mqtt_qos2_entry_t;

typedef struct tb2_mqtt_packet_id_entry
{
    uint16_t original_id;
    uint16_t wire_id;
    uint8_t qos;
    bool_t local;
    bool_t pubrel;
    uint8_t packet_type;
    uint8_t *packet;
    size_t packet_length;
    struct tb2_mqtt_packet_id_entry *next;
} tb2_mqtt_packet_id_entry_t;

typedef struct
{
    bool_t initialized;
    OsMutex mutex;
    uint32_t session_counter;
    uint32_t active_sessions;
    uint32_t upstream_sessions;
    char state[16];
    char error_code[32];
    uint64_t bytes_box_to_upstream;
    uint64_t bytes_upstream_to_box;
    uint64_t messages_forwarded_box_to_upstream;
    uint64_t messages_forwarded_upstream_to_box;
    uint64_t messages_blocked_box_to_upstream;
    uint64_t messages_blocked_upstream_to_box;
    uint64_t messages_rewritten_box_to_upstream;
    uint64_t messages_rewritten_upstream_to_box;
    uint64_t nocloud_items_removed_box_to_upstream;
    uint64_t nocloud_items_removed_upstream_to_box;
    time_t last_attempt;
    time_t last_success;
} tb2_mqtt_passthrough_status_t;

typedef struct
{
    char session_id[48];
    char directory[512];
    char traffic_path[576];
    char session_path[576];
    FsFile *traffic;
    uint64_t sequence;
    uint64_t bytes_box_to_upstream;
    uint64_t bytes_upstream_to_box;
    uint64_t messages_forwarded_box_to_upstream;
    uint64_t messages_forwarded_upstream_to_box;
    uint64_t messages_blocked_box_to_upstream;
    uint64_t messages_blocked_upstream_to_box;
    uint64_t messages_rewritten_box_to_upstream;
    uint64_t messages_rewritten_upstream_to_box;
    uint64_t nocloud_items_removed_box_to_upstream;
    uint64_t nocloud_items_removed_upstream_to_box;
    time_t started_at;
} tb2_mqtt_capture_t;

#define TB2_MQTT_INFLIGHT_MAX 32U
#define TB2_MQTT_BUFFER_LIMIT (1024U * 1024U)
#define TB2_MQTT_SESSION_MAX 32U
#define TB2_MQTT_HANDSHAKE_TIMEOUT_MS 10000U
#define TB2_MQTT_POLICY_CHECK_MS 1000U

typedef struct {
    uint64_t token;
    uint8_t *original;
    size_t original_length;
    uint8_t *wire;
    size_t wire_length;
    size_t budget_bytes;
    char *topic;
    const char *action;
    const char *filter_id;
    mqtt_forward_filter_result_t decision;
    bool_t has_decision;
    uint16_t original_id;
    uint16_t wire_id;
    uint8_t packet_type;
    bool_t generated;
    bool_t rewritten;
    size_t removed;
} tb2_mqtt_pending_write_t;

typedef enum {
    TB2_MQTT_CLOUD_DOWN, TB2_MQTT_CLOUD_CONNACK,
    TB2_MQTT_CLOUD_SUBACK, TB2_MQTT_CLOUD_READY
} tb2_mqtt_cloud_phase_t;

struct tb2_mqtt_passthrough_session {
    TlsContext *box_tls;
    Socket *box_socket;
    settings_t *box_settings;
    tb2_mqtt_publish_observer_t observer;
    tb2_mqtt_control_observer_t control_observer;
    tb2_mqtt_publish_completed_t publish_completed;
    tb2_mqtt_subscription_snapshot_t subscription_snapshot;
    tb2_mqtt_subscription_apply_t subscription_apply;
    tb2_mqtt_subscription_qos_t subscription_qos;
    void *observer_context;
    tb2_mqtt_stream_t box_stream;
    tb2_mqtt_stream_t upstream_stream;
    tb2_mqtt_qos2_entry_t *blocked_qos2_box;
    tb2_mqtt_qos2_entry_t *blocked_qos2_upstream;
    tb2_mqtt_packet_id_entry_t *packet_ids;
    tb2_mqtt_packet_id_entry_t *cloud_ids;
    uint16_t next_local_packet_id;
    uint16_t next_cloud_packet_id;
    tb2_mqtt_capture_t capture;
    bool_t capture_opened;
    atomic_bool capture_failed;
    atomic_bool privacy_failed;
    OsMutex io_mutex;
    OsMutex ids_mutex;
    tb2_mqtt_upstream_worker_t *worker;
    uint64_t owner;
    uint64_t epoch;
    uint64_t confirmed_epoch;
    uint64_t next_token;
    tb2_mqtt_pending_write_t writes[TB2_MQTT_INFLIGHT_MAX];
    size_t writes_bytes;
    tb2_mqtt_cloud_phase_t cloud_phase;
    uint8_t *connect_packet;
    size_t connect_length;
    size_t connect_header;
    size_t connect_flags_offset;
    size_t will_begin;
    size_t will_end;
    uint8_t *cloud_connect;
    size_t cloud_connect_length;
    uint16_t keepalive;
    uint32_t last_box_rx;
    uint32_t last_cloud_tx;
    uint32_t cloud_started_at;
    uint32_t ping_sent_at;
    bool_t ping_pending;
    bool_t established;
    bool_t clean_session;
    bool_t clean_disconnect;
    bool_t upstream_configured;
    bool_t ever_connected;
    char client_id[256];
    char upstream_error[48];
    tb2_mqtt_subscription_t synced[TB2_MQTT_SUBSCRIPTIONS_MAX];
    size_t synced_count;
    bool_t subscriptions_dirty;
    int saved_slot;
    bool_t configure_attempted;
    atomic_bool box_write_failed;
    /* API snapshot: protected by the global status mutex, never a live
     * cross-thread read of worker/parser state. */
    bool_t runtime_local;
    bool_t runtime_cloud;
    char runtime_state[16];
    char runtime_error[48];
    uint32_t last_configure_at;
    uint32_t last_will_check;
    mqtt_response_history_t response_history;
};

typedef struct {
    bool_t used;
    uint64_t owner;
    bool_t persistent;
    tb2_mqtt_passthrough_session_t *active;
    char box[32];
    char client_id[256];
    tb2_mqtt_subscription_t subscriptions[TB2_MQTT_SUBSCRIPTIONS_MAX];
    size_t subscription_count;
    tb2_mqtt_qos2_entry_t *incoming;
    tb2_mqtt_packet_id_entry_t *outgoing;
    uint16_t next_id;
    mqtt_response_history_t response_history;
} tb2_mqtt_saved_session_t;
static tb2_mqtt_saved_session_t saved_sessions[TB2_MQTT_SESSION_MAX];
static uint64_t next_owner;
static void tb2_mqtt_cloud_reset(tb2_mqtt_passthrough_session_t *session, const char *reason);
static error_t tb2_mqtt_process_stream(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *data, size_t length);
static void tb2_mqtt_packet_ids_clear(tb2_mqtt_packet_id_entry_t **list);
static void tb2_mqtt_pending_writes_clear(tb2_mqtt_passthrough_session_t *session);
static bool_t tb2_mqtt_utf8_valid(const uint8_t *text, size_t length);
static bool_t tb2_mqtt_cloud_available(const tb2_mqtt_passthrough_session_t *session);
static void tb2_mqtt_notify_publish_completed(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *packet, size_t length);

static tb2_mqtt_passthrough_status_t mqtt_passthrough_status;

static void tb2_mqtt_trace_error(const char *stage, error_t error)
{
    TRACE_ERROR("TB2 MQTT upstream stage=%s failed error=%s code=%d\r\n",
                stage, error2text(error), (int)error);
}

static void tb2_mqtt_set_private_permissions(const char *path, bool_t directory)
{
#ifdef _WIN32
    (void)directory;
    _chmod(path, _S_IREAD | _S_IWRITE);
#else
    chmod(path, directory ? 0700 : 0600);
#endif
}

static void tb2_mqtt_format_utc(time_t timestamp, char *output, size_t output_size)
{
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    strftime(output, output_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static char *tb2_mqtt_resolve_capture_root(settings_t *settings)
{
    char *resolved = osAllocMem(512);
    if (resolved == NULL)
    {
        return NULL;
    }
    resolved[0] = '\0';
    settings_resolve_dir(&resolved, settings->mqtt_client_upstream.capture_dir,
                         settings->internal.basedirfull);
    return resolved;
}

static settings_t *tb2_mqtt_settings_from_certificate(TlsContext *tls_context)
{
    if (tls_context == NULL)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=tls_context_missing\r\n");
        return NULL;
    }

    const char *subject = tls_context->client_cert_subject;
    const char *issuer = tls_context->client_cert_issuer;
    if (subject == NULL || subject[0] == '\0')
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=client_certificate_missing\r\n");
        return NULL;
    }
    TRACE_DEBUG("TB2 MQTT upstream stage=box_client_auth certificate_present=true"
                " subject='%s' issuer='%s' serial='%s'\r\n",
                subject, issuer != NULL && issuer[0] != '\0' ? issuer : "<missing>",
                tls_context->client_cert_serial);

    char common_name[13] = "";
    settings_t *settings = settings_get_existing_tb2_from_certificate_subject(
        subject, common_name, sizeof(common_name));
    if (settings == NULL)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=%s"
                    " subject='%s' issuer='%s'\r\n",
                    common_name[0] != '\0' ? "tb2_overlay_not_found" : "subject_box_id_invalid",
                    subject, issuer != NULL && issuer[0] != '\0' ? issuer : "<missing>");
        return NULL;
    }

    TRACE_DEBUG("TB2 MQTT upstream stage=map_box_identity success box=%s overlay=%u"
                " issuer_checked=false\r\n",
                common_name, (unsigned)settings->internal.overlayNumber);
    return settings;
}

static bool_t tb2_mqtt_has_original_identity(settings_t *settings)
{
    return settings != NULL && settings->internal.client_tb2.ca != NULL &&
           settings->internal.client_tb2.crt != NULL && settings->internal.client_tb2.key != NULL &&
           settings->internal.client_tb2.ca[0] != '\0' && settings->internal.client_tb2.crt[0] != '\0' &&
           settings->internal.client_tb2.key[0] != '\0';
}

static bool_t tb2_mqtt_overlay_has_identity_override(const settings_t *settings)
{
    static const char *const identity_options[] = {
        "core.client_cert_tb2.file.ca",
        "core.client_cert_tb2.file.crt",
        "core.client_cert_tb2.file.key",
        "core.client_cert_tb2.data.ca",
        "core.client_cert_tb2.data.crt",
        "core.client_cert_tb2.data.key",
    };

    if (settings == NULL || settings->internal.overlayNumber == 0 ||
        settings->internal.overlayUniqueId == NULL)
    {
        return FALSE;
    }

    for (size_t i = 0; i < sizeof(identity_options) / sizeof(identity_options[0]); i++)
    {
        setting_item_t *option = settings_get_by_name_ovl(identity_options[i],
                                                           settings->internal.overlayUniqueId);
        if (option != NULL && option->overlayed)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static settings_t *tb2_mqtt_select_identity_settings(settings_t *box_settings)
{
    bool_t overlay_override = tb2_mqtt_overlay_has_identity_override(box_settings);
    settings_t *identity_settings = overlay_override ? box_settings : get_settings();
    TRACE_DEBUG("TB2 MQTT upstream stage=select_identity source=%s overlay_override=%s\r\n",
                overlay_override ? "box_overlay" : "global_default",
                overlay_override ? "true" : "false");
    return identity_settings;
}

static error_t tb2_mqtt_capture_open(tb2_mqtt_capture_t *capture, settings_t *settings)
{
    osMemset(capture, 0, sizeof(*capture));
    capture->started_at = time(NULL);

    char *root = tb2_mqtt_resolve_capture_root(settings);
    if (root == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    error_t error = fsCreateDirEx(root, TRUE);
    if (error && !fsDirExists(root))
    {
        osFreeMem(root);
        return error;
    }
    tb2_mqtt_set_private_permissions(root, TRUE);

    char compact_time[24];
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &capture->started_at);
#else
    gmtime_r(&capture->started_at, &utc);
#endif
    strftime(compact_time, sizeof(compact_time), "%Y%m%dT%H%M%SZ", &utc);

    bool_t directory_created = FALSE;
    for (uint32_t attempt = 0; attempt < 1024 && !directory_created; attempt++)
    {
        osAcquireMutex(&mqtt_passthrough_status.mutex);
        uint32_t counter = ++mqtt_passthrough_status.session_counter;
        osReleaseMutex(&mqtt_passthrough_status.mutex);

        osSnprintf(capture->session_id, sizeof(capture->session_id), "%s-%08u", compact_time,
                   (unsigned)counter);
        osSnprintf(capture->directory, sizeof(capture->directory), "%s%c%s", root,
                   PATH_SEPARATOR, capture->session_id);
        error = fsCreateDir(capture->directory);
        if (!error)
        {
            directory_created = TRUE;
        }
        else if (!fsDirExists(capture->directory))
        {
            osFreeMem(root);
            return error;
        }
    }
    if (!directory_created)
    {
        osFreeMem(root);
        return ERROR_OPEN_FAILED;
    }

    osSnprintf(capture->traffic_path, sizeof(capture->traffic_path), "%s%ctraffic.jsonl",
               capture->directory, PATH_SEPARATOR);
    osSnprintf(capture->session_path, sizeof(capture->session_path), "%s%csession.json",
               capture->directory, PATH_SEPARATOR);
    osFreeMem(root);

    tb2_mqtt_set_private_permissions(capture->directory, TRUE);
    capture->traffic = fsOpenFile(capture->traffic_path,
                                  FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    if (capture->traffic == NULL)
    {
        return ERROR_OPEN_FAILED;
    }
    tb2_mqtt_set_private_permissions(capture->traffic_path, FALSE);
    return NO_ERROR;
}

static const char *tb2_mqtt_packet_type_name(uint8_t type)
{
    static const char *names[] = {
        "reserved", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL",
        "PUBCOMP", "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK", "PINGREQ",
        "PINGRESP", "DISCONNECT", "AUTH",
    };
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "unknown";
}

static char *tb2_mqtt_base64_data(const uint8_t *data, size_t length)
{
    size_t encoded_length = 0;
    base64Encode(data, length, NULL, &encoded_length);
    char *encoded = osAllocMem(encoded_length + 1);
    if (encoded == NULL)
    {
        return NULL;
    }
    base64Encode(data, length, encoded, &encoded_length);
    encoded[encoded_length] = '\0';
    return encoded;
}

static error_t tb2_mqtt_capture_packet_ex(tb2_mqtt_capture_t *capture,
                                          const char *direction,
                                          const uint8_t *data, size_t length,
                                          const uint8_t *wire_data, size_t wire_length,
                                          uint8_t packet_type, const char *topic,
                                          bool_t forwarded, const char *filter_id,
                                           bool_t generated, bool_t packet_complete,
                                           const char *action, uint16_t packet_id,
                                           uint16_t wire_packet_id,
                                           size_t removed_count,
                                           const mqtt_forward_filter_result_t *manual_decision)
{
    char *encoded = tb2_mqtt_base64_data(data, length);
    if (encoded == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    char *wire_encoded = NULL;
    if (wire_data != NULL &&
        (wire_length != length || osMemcmp(wire_data, data, length) != 0))
    {
        wire_encoded = tb2_mqtt_base64_data(wire_data, wire_length);
        if (wire_encoded == NULL)
        {
            osFreeMem(encoded);
            return ERROR_OUT_OF_MEMORY;
        }
    }

    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL)
    {
        osFreeMem(encoded);
        osFreeMem(wire_encoded);
        return ERROR_OUT_OF_MEMORY;
    }
    cJSON_AddNumberToObject(entry, "sequence", (double)++capture->sequence);
    cJSON_AddNumberToObject(entry, "timestamp_ms", (double)time(NULL) * 1000.0);
    cJSON_AddStringToObject(entry, "direction", direction);
    cJSON_AddStringToObject(entry, "data_base64", encoded);
    if (wire_encoded != NULL)
    {
        cJSON_AddStringToObject(entry, "wire_data_base64", wire_encoded);
    }
    cJSON_AddStringToObject(entry, "packet_type", tb2_mqtt_packet_type_name(packet_type));
    if (topic != NULL)
    {
        cJSON_AddStringToObject(entry, "topic", topic);
    }
    cJSON_AddBoolToObject(entry, "forwarded", forwarded);
    if (filter_id != NULL)
    {
        cJSON_AddStringToObject(entry, "filter_id", filter_id);
    }
    cJSON_AddBoolToObject(entry, "generated", generated);
    cJSON_AddBoolToObject(entry, "packet_complete", packet_complete);
    if (packet_type == TB2_MQTT_PACKET_PUBLISH && manual_decision != NULL)
    {
        // Rewritten incoming packets can also be generated. Only the actual
        // ingress/local sender determines the route, never that legacy flag.
        cJSON_AddStringToObject(entry, "publish_route",
                               mqtt_forward_route_name(manual_decision->route));
        cJSON_AddStringToObject(entry, "manual_filter_decision",
                               mqtt_forward_reason_name(manual_decision->reason));
        if (manual_decision->setting_id != NULL)
            cJSON_AddStringToObject(entry, "manual_filter_id",
                                   manual_decision->setting_id);
    }
    if (action != NULL)
    {
        cJSON_AddStringToObject(entry, "action", action);
    }
    if (packet_id != 0)
    {
        cJSON_AddNumberToObject(entry, "packet_id", packet_id);
    }
    if (wire_packet_id != 0 && wire_packet_id != packet_id)
    {
        cJSON_AddNumberToObject(entry, "wire_packet_id", wire_packet_id);
    }
    if (removed_count > 0 ||
        (action != NULL && osStrncmp(action, "nocloud_", 8) == 0))
    {
        cJSON_AddNumberToObject(entry, "removed_count",
                               (double)removed_count);
    }
    osFreeMem(encoded);
    osFreeMem(wire_encoded);

    char *line = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (line == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    error_t error = fsWriteFile(capture->traffic, line, osStrlen(line));
    if (!error)
    {
        error = fsWriteFile(capture->traffic, "\n", 1);
    }
    if (!error)
    {
        error = fsFlushFile(capture->traffic);
    }
    cJSON_free(line);
    return error;
}

static error_t tb2_mqtt_capture_packet(tb2_mqtt_capture_t *capture,
                                       const char *direction,
                                       const uint8_t *data, size_t length,
                                       uint8_t packet_type, const char *topic,
                                       bool_t forwarded, const char *filter_id,
                                       bool_t generated, bool_t packet_complete)
{
    return tb2_mqtt_capture_packet_ex(capture, direction, data, length, NULL, 0,
                                      packet_type, topic, forwarded, filter_id,
                                      generated, packet_complete, NULL, 0, 0, 0, NULL);
}

static error_t tb2_mqtt_capture_finish(tb2_mqtt_capture_t *capture, settings_t *settings,
                                       const char *result_code)
{
    error_t error = NO_ERROR;
    if (capture->traffic != NULL)
    {
        error = fsFlushFile(capture->traffic);
        fsCloseFile(capture->traffic);
        capture->traffic = NULL;
    }
    if (error)
    {
        return error;
    }

    cJSON *session = cJSON_CreateObject();
    if (session == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    char started[32];
    char finished[32];
    tb2_mqtt_format_utc(capture->started_at, started, sizeof(started));
    tb2_mqtt_format_utc(time(NULL), finished, sizeof(finished));
    cJSON_AddStringToObject(session, "session_id", capture->session_id);
    cJSON_AddStringToObject(session, "started_at", started);
    cJSON_AddStringToObject(session, "finished_at", finished);
    cJSON_AddStringToObject(session, "hostname", settings->mqtt_client_upstream.hostname);
    cJSON_AddNumberToObject(session, "port", settings->mqtt_client_upstream.port);
    cJSON_AddNumberToObject(session, "bytes_box_to_upstream",
                           (double)capture->bytes_box_to_upstream);
    cJSON_AddNumberToObject(session, "bytes_upstream_to_box",
                           (double)capture->bytes_upstream_to_box);
    cJSON_AddNumberToObject(session, "messages_forwarded_box_to_upstream",
                           (double)capture->messages_forwarded_box_to_upstream);
    cJSON_AddNumberToObject(session, "messages_forwarded_upstream_to_box",
                           (double)capture->messages_forwarded_upstream_to_box);
    cJSON_AddNumberToObject(session, "messages_blocked_box_to_upstream",
                           (double)capture->messages_blocked_box_to_upstream);
    cJSON_AddNumberToObject(session, "messages_blocked_upstream_to_box",
                           (double)capture->messages_blocked_upstream_to_box);
    cJSON_AddNumberToObject(session, "messages_rewritten_box_to_upstream",
                           (double)capture->messages_rewritten_box_to_upstream);
    cJSON_AddNumberToObject(session, "messages_rewritten_upstream_to_box",
                           (double)capture->messages_rewritten_upstream_to_box);
    cJSON_AddNumberToObject(
        session, "nocloud_items_removed_box_to_upstream",
        (double)capture->nocloud_items_removed_box_to_upstream);
    cJSON_AddNumberToObject(
        session, "nocloud_items_removed_upstream_to_box",
        (double)capture->nocloud_items_removed_upstream_to_box);
    cJSON_AddStringToObject(session, "result", result_code);

    char *json = cJSON_PrintUnformatted(session);
    cJSON_Delete(session);
    if (json == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    char temporary_path[640];
    osSnprintf(temporary_path, sizeof(temporary_path), "%s.tmp", capture->session_path);
    FsFile *file = fsOpenFile(temporary_path,
                              FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    if (file == NULL)
    {
        cJSON_free(json);
        return ERROR_OPEN_FAILED;
    }
    error = fsWriteFile(file, json, osStrlen(json));
    if (!error)
    {
        error = fsFlushFile(file);
    }
    fsCloseFile(file);
    cJSON_free(json);

    if (!error)
    {
        error = fsRenameFile(temporary_path, capture->session_path);
    }
    if (error)
    {
        fsDeleteFile(temporary_path);
        return error;
    }
    tb2_mqtt_set_private_permissions(capture->session_path, FALSE);
    return NO_ERROR;
}

static uint64_t tb2_mqtt_completed_capture_size(const char *root, const char *name)
{
    char path[640];
    uint32_t size = 0;
    uint64_t total = 0;
    osSnprintf(path, sizeof(path), "%s%c%s%csession.json", root, PATH_SEPARATOR, name,
               PATH_SEPARATOR);
    if (!fsFileExists(path))
    {
        return 0;
    }
    if (fsGetFileSize(path, &size) == NO_ERROR)
    {
        total += size;
    }
    osSnprintf(path, sizeof(path), "%s%c%s%ctraffic.jsonl", root, PATH_SEPARATOR, name,
               PATH_SEPARATOR);
    if (fsGetFileSize(path, &size) == NO_ERROR)
    {
        total += size;
    }
    return total;
}

static void tb2_mqtt_rotate_completed_captures(settings_t *settings)
{
    char *root = tb2_mqtt_resolve_capture_root(settings);
    if (root == NULL)
    {
        return;
    }
    uint64_t limit = (uint64_t)settings->mqtt_client_upstream.capture_max_mib *
                     TB2_MQTT_CAPTURE_MIB_BYTES;
    uint64_t total = 0;
    FsDir *dir = fsOpenDir(root);
    FsDirEntry entry;
    if (dir != NULL)
    {
        while (fsReadDir(dir, &entry) == NO_ERROR)
        {
            if ((entry.attributes & FS_FILE_ATTR_DIRECTORY) != 0 && entry.name[0] != '.')
            {
                total += tb2_mqtt_completed_capture_size(root, entry.name);
            }
        }
        fsCloseDir(dir);
    }

    while (total > limit)
    {
        char oldest_name[FS_MAX_NAME_LEN + 1] = "";
        time_t oldest_time = 0;
        uint64_t oldest_size = 0;
        dir = fsOpenDir(root);
        if (dir == NULL)
        {
            break;
        }
        while (fsReadDir(dir, &entry) == NO_ERROR)
        {
            if ((entry.attributes & FS_FILE_ATTR_DIRECTORY) == 0 || entry.name[0] == '.')
            {
                continue;
            }
            uint64_t size = tb2_mqtt_completed_capture_size(root, entry.name);
            if (size == 0)
            {
                continue;
            }
            time_t modified = convertDateToUnixTime(&entry.modified);
            if (oldest_name[0] == '\0' || modified < oldest_time)
            {
                oldest_time = modified;
                oldest_size = size;
                osStrncpy(oldest_name, entry.name, sizeof(oldest_name) - 1);
            }
        }
        fsCloseDir(dir);
        if (oldest_name[0] == '\0')
        {
            break;
        }

        char session_dir[640];
        char session_path[704];
        char traffic_path[704];
        osSnprintf(session_dir, sizeof(session_dir), "%s%c%s", root, PATH_SEPARATOR, oldest_name);
        osSnprintf(session_path, sizeof(session_path), "%s%csession.json", session_dir,
                   PATH_SEPARATOR);
        osSnprintf(traffic_path, sizeof(traffic_path), "%s%ctraffic.jsonl", session_dir,
                   PATH_SEPARATOR);
        fsDeleteFile(traffic_path);
        fsDeleteFile(session_path);
        if (fsRemoveDir(session_dir) != NO_ERROR)
        {
            break;
        }
        total = oldest_size <= total ? total - oldest_size : 0;
    }
    osFreeMem(root);
}

static void tb2_mqtt_status_start(void)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    mqtt_passthrough_status.active_sessions++;
    mqtt_passthrough_status.last_attempt = time(NULL);
    osStrcpy(mqtt_passthrough_status.state, "connecting");
    mqtt_passthrough_status.error_code[0] = '\0';
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}


static void tb2_mqtt_status_add_bytes(bool_t box_to_upstream, size_t length)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    if (box_to_upstream)
    {
        mqtt_passthrough_status.bytes_box_to_upstream += length;
    }
    else
    {
        mqtt_passthrough_status.bytes_upstream_to_box += length;
    }
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

static void tb2_mqtt_status_finish(bool_t success, const char *error_code)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    if (mqtt_passthrough_status.active_sessions > 0)
    {
        mqtt_passthrough_status.active_sessions--;
    }
    if (!osStrcmp(error_code, "disabled"))
    {
        mqtt_passthrough_status.error_code[0] = '\0';
        osStrcpy(mqtt_passthrough_status.state,
                 mqtt_passthrough_status.active_sessions > 0 ? "connected" : "ready");
    }
    else if (success)
    {
        mqtt_passthrough_status.last_success = time(NULL);
        mqtt_passthrough_status.error_code[0] = '\0';
        osStrcpy(mqtt_passthrough_status.state,
                 mqtt_passthrough_status.active_sessions > 0 ? "connected" : "ready");
    }
    else
    {
        osStrncpy(mqtt_passthrough_status.error_code, error_code,
                  sizeof(mqtt_passthrough_status.error_code) - 1);
        if (mqtt_passthrough_status.active_sessions == 0)
        {
            osStrcpy(mqtt_passthrough_status.state, "error");
        }
    }
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

static void tb2_mqtt_status_attempt_failed(const char *error_code)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    mqtt_passthrough_status.last_attempt = time(NULL);
    osStrcpy(mqtt_passthrough_status.state, "error");
    osStrncpy(mqtt_passthrough_status.error_code, error_code,
              sizeof(mqtt_passthrough_status.error_code) - 1);
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

static error_t tb2_mqtt_tls_write_all(TlsContext *destination, const uint8_t *data,
                                      size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        size_t written = 0;
        error_t error = tlsWrite(destination, data + offset, length - offset, &written, 0);
        if (error)
        {
            tb2_mqtt_trace_error("tls_write", error);
            return error;
        }
        if (written == 0)
        {
            tb2_mqtt_trace_error("tls_write_zero", ERROR_WRITE_FAILED);
            return ERROR_WRITE_FAILED;
        }
        offset += written;
    }
    return NO_ERROR;
}

static void tb2_mqtt_status_add_message(bool_t box_to_upstream, bool_t blocked)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    uint64_t *counter;
    if (box_to_upstream)
    {
        counter = blocked ? &mqtt_passthrough_status.messages_blocked_box_to_upstream :
                            &mqtt_passthrough_status.messages_forwarded_box_to_upstream;
    }
    else
    {
        counter = blocked ? &mqtt_passthrough_status.messages_blocked_upstream_to_box :
                            &mqtt_passthrough_status.messages_forwarded_upstream_to_box;
    }
    (*counter)++;
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

static void tb2_mqtt_add_nocloud_stats(
    tb2_mqtt_passthrough_session_t *session, bool_t box_to_upstream,
    bool_t rewritten, size_t removed_count)
{
    if (removed_count == 0)
        return;

    if (box_to_upstream)
    {
        session->capture.nocloud_items_removed_box_to_upstream += removed_count;
        if (rewritten)
            session->capture.messages_rewritten_box_to_upstream++;
    }
    else
    {
        session->capture.nocloud_items_removed_upstream_to_box += removed_count;
        if (rewritten)
            session->capture.messages_rewritten_upstream_to_box++;
    }

    osAcquireMutex(&mqtt_passthrough_status.mutex);
    if (box_to_upstream)
    {
        mqtt_passthrough_status.nocloud_items_removed_box_to_upstream +=
            removed_count;
        if (rewritten)
            mqtt_passthrough_status.messages_rewritten_box_to_upstream++;
    }
    else
    {
        mqtt_passthrough_status.nocloud_items_removed_upstream_to_box +=
            removed_count;
        if (rewritten)
            mqtt_passthrough_status.messages_rewritten_upstream_to_box++;
    }
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

static void tb2_mqtt_pending_write_free(tb2_mqtt_pending_write_t *write)
{
    osFreeMem(write->original);
    osFreeMem(write->wire);
    osFreeMem(write->topic);
    osMemset(write, 0, sizeof(*write));
}

static void tb2_mqtt_pending_writes_clear(tb2_mqtt_passthrough_session_t *session)
{
    for (size_t i = 0; i < TB2_MQTT_INFLIGHT_MAX; i++)
        tb2_mqtt_pending_write_free(&session->writes[i]);
    session->writes_bytes = 0;
}

/* Caller holds io_mutex. A failed handoff is transport loss, not a policy block. */
static void tb2_mqtt_capture_transport_drop(tb2_mqtt_passthrough_session_t *session,
    const uint8_t *data, size_t length, const uint8_t *wire, size_t wire_length,
    uint8_t type, const char *topic, const char *reason,
    const mqtt_forward_filter_result_t *decision)
{
    if (session->capture_opened && !session->capture_failed &&
        tb2_mqtt_capture_packet_ex(&session->capture, "box_to_upstream",
            data, length, wire, wire_length, type, topic, FALSE, NULL, FALSE,
            TRUE, reason, 0, 0, 0, decision))
        session->capture_failed = TRUE;
    TRACE_WARNING("TB2 MQTT cloud handoff dropped reason=%s; local connection retained\r\n", reason);
}

/* io_mutex serializes box TLS writes and capture. Never invoke observers while
 * holding it: HTTP controls acquire their own app-control lock first. */
static error_t tb2_mqtt_record_packet_ex(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *data, size_t length,
    const uint8_t *wire_data, size_t wire_length, uint8_t packet_type, const char *topic,
    bool_t forwarded, const char *filter_id, bool_t generated, bool_t packet_complete,
    const char *action, uint16_t packet_id, uint16_t wire_packet_id,
    bool_t count_blocked, bool_t rewritten, size_t removed_count,
    const mqtt_forward_filter_result_t *manual_decision)
{
    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    const uint8_t *outgoing = wire_data != NULL ? wire_data : data;
    size_t outgoing_length = wire_data != NULL ? wire_length : length;
    osAcquireMutex(&session->io_mutex);
    bool_t cloud_send = box_to_upstream && forwarded;
    bool_t cloud_available = session->worker != NULL && session->epoch != 0 &&
        !session->capture_failed && !session->privacy_failed && tb2_mqtt_passthrough_is_enabled() &&
        (packet_type != TB2_MQTT_PACKET_PUBLISH ||
         session->cloud_phase == TB2_MQTT_CLOUD_READY);
    bool_t box_send = !box_to_upstream && forwarded;
    const char *capture_action = cloud_send ?
        (cloud_available ? "upstream_queued" : "upstream_unavailable") :
        box_send ? "box_write_pending" : action;
    error_t capture_error = session->capture_opened && !session->capture_failed ?
        tb2_mqtt_capture_packet_ex(&session->capture, direction, data, length,
            wire_data, wire_length, packet_type, topic, cloud_send || box_send ? FALSE : forwarded,
            filter_id, generated, packet_complete, capture_action, packet_id,
            wire_packet_id, removed_count, manual_decision) : ERROR_WRITE_FAILED;
    if (capture_error && !session->capture_failed)
    {
        session->capture_failed = TRUE;
        osStrcpy(session->upstream_error, "capture_write_failed");
        TRACE_WARNING("TB2 MQTT capture failed; Internet relay suspended, local session retained\r\n");
    }
    if (cloud_send)
    {
        if (!cloud_available || session->capture_failed)
        {
            osReleaseMutex(&session->io_mutex);
            return NO_ERROR;
        }
        tb2_mqtt_pending_write_t *pending = NULL;
        for (size_t i = 0; i < TB2_MQTT_INFLIGHT_MAX; i++)
            if (session->writes[i].token == 0) { pending = &session->writes[i]; break; }
        /* Count original capture bytes, wire bytes, the worker's owned copy
         * and the diagnostic topic against the same handoff budget. */
        size_t topic_bytes = topic != NULL ? osStrlen(topic) + 1 : 0;
        size_t budget = length + 2U * outgoing_length + topic_bytes;
        if (pending == NULL || length > TB2_MQTT_BUFFER_LIMIT ||
            outgoing_length > TB2_MQTT_BUFFER_LIMIT || budget > TB2_MQTT_BUFFER_LIMIT ||
            session->writes_bytes > TB2_MQTT_BUFFER_LIMIT - budget)
        {
            osStrcpy(session->upstream_error, "upstream_buffer_full");
            tb2_mqtt_capture_transport_drop(session, data, length, wire_data, wire_length,
                packet_type, topic, "upstream_buffer_full", manual_decision);
            tb2_mqtt_upstream_reconnect(session->worker, ERROR_OUT_OF_RESOURCES);
            osReleaseMutex(&session->io_mutex);
            return NO_ERROR;
        }
        pending->original = osAllocMem(length);
        pending->wire = osAllocMem(outgoing_length);
        pending->topic = topic != NULL ? osAllocMem(osStrlen(topic) + 1) : NULL;
        if (pending->topic != NULL) osStrcpy(pending->topic, topic);
        uint8_t *transport_copy = osAllocMem(outgoing_length);
        if (pending->original == NULL || pending->wire == NULL ||
            transport_copy == NULL || (topic != NULL && pending->topic == NULL))
        {
            osFreeMem(transport_copy);
            tb2_mqtt_pending_write_free(pending);
            tb2_mqtt_capture_transport_drop(session, data, length, wire_data, wire_length,
                packet_type, topic, "upstream_out_of_memory", manual_decision);
            tb2_mqtt_upstream_reconnect(session->worker, ERROR_OUT_OF_MEMORY);
            osReleaseMutex(&session->io_mutex);
            return NO_ERROR;
        }
        osMemcpy(pending->original, data, length);
        osMemcpy(pending->wire, outgoing, outgoing_length);
        osMemcpy(transport_copy, outgoing, outgoing_length);
        pending->budget_bytes = budget;
        pending->original_length = length;
        pending->wire_length = outgoing_length;
        pending->action = action;
        pending->filter_id = filter_id;
        pending->original_id = packet_id;
        pending->wire_id = wire_packet_id;
        pending->packet_type = packet_type;
        pending->generated = generated;
        pending->rewritten = rewritten;
        pending->removed = removed_count;
        pending->has_decision = manual_decision != NULL;
        if (manual_decision != NULL) pending->decision = *manual_decision;
        pending->token = ++session->next_token;
        error_t error = tb2_mqtt_upstream_send(session->worker, session->epoch,
            pending->token, transport_copy, outgoing_length);
        if (error)
        {
            osFreeMem(transport_copy);
            tb2_mqtt_pending_write_free(pending);
            osStrcpy(session->upstream_error, "upstream_unavailable");
            tb2_mqtt_capture_transport_drop(session, data, length, wire_data, wire_length,
                packet_type, topic, error == ERROR_OUT_OF_RESOURCES ?
                "upstream_buffer_full" : "upstream_unavailable", manual_decision);
        }
        else session->writes_bytes += budget;
        osReleaseMutex(&session->io_mutex);
        return NO_ERROR;
    }
    if (!forwarded)
    {
        if (count_blocked)
        {
            if (box_to_upstream) session->capture.messages_blocked_box_to_upstream++;
            else session->capture.messages_blocked_upstream_to_box++;
            tb2_mqtt_status_add_message(box_to_upstream, TRUE);
        }
        tb2_mqtt_add_nocloud_stats(session, box_to_upstream, FALSE, removed_count);
        osReleaseMutex(&session->io_mutex);
        return NO_ERROR;
    }
    if ((session->capture_failed || session->privacy_failed) && manual_decision != NULL &&
        manual_decision->route == MQTT_FORWARD_ROUTE_TONIES_TO_BOX)
    { osReleaseMutex(&session->io_mutex); return NO_ERROR; }
    /* Local control and protocol ACKs must still work when capture storage fails. */
    error_t error = tb2_mqtt_tls_write_all(session->box_tls, outgoing, outgoing_length);
    if (error) session->box_write_failed = TRUE;
    if (session->capture_opened && !session->capture_failed &&
        tb2_mqtt_capture_packet_ex(&session->capture, direction, data, length,
            wire_data, wire_length, packet_type, topic, !error, filter_id,
            generated, packet_complete, error ? "box_write_failed" :
                action != NULL ? action : "box_write_complete",
            packet_id, wire_packet_id, removed_count, manual_decision))
    {
        session->capture_failed = TRUE;
        osStrcpy(session->upstream_error, "capture_write_failed");
        TRACE_WARNING("TB2 MQTT capture completion failed; Internet relay suspended\r\n");
    }
    if (!error)
    {
        session->capture.bytes_upstream_to_box += outgoing_length;
        session->capture.messages_forwarded_upstream_to_box++;
        tb2_mqtt_status_add_bytes(FALSE, outgoing_length);
        tb2_mqtt_status_add_message(FALSE, FALSE);
        tb2_mqtt_add_nocloud_stats(session, FALSE, rewritten, removed_count);
    }
    osReleaseMutex(&session->io_mutex);
    if (!error && packet_type == TB2_MQTT_PACKET_PUBLISH && manual_decision != NULL &&
        manual_decision->route == MQTT_FORWARD_ROUTE_TONIES_TO_BOX)
        tb2_mqtt_notify_publish_completed(session, FALSE, outgoing, outgoing_length);
    return error;
}

static error_t tb2_mqtt_stream_append(tb2_mqtt_stream_t *stream, const uint8_t *data,
                                      size_t length)
{
    if (length > TB2_MQTT_BUFFER_LIMIT || stream->length > TB2_MQTT_BUFFER_LIMIT - length)
    {
        return ERROR_INVALID_LENGTH;
    }
    size_t required = stream->length + length;
    if (required > stream->capacity)
    {
        size_t capacity = stream->capacity > 0 ? stream->capacity : TB2_MQTT_TUNNEL_BUFFER_SIZE;
        while (capacity < required)
        {
            if (capacity > SIZE_MAX / 2)
            {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        uint8_t *replacement = osAllocMem(capacity);
        if (replacement == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        if (stream->length > 0)
        {
            osMemcpy(replacement, stream->data, stream->length);
        }
        osFreeMem(stream->data);
        stream->data = replacement;
        stream->capacity = capacity;
    }
    osMemcpy(stream->data + stream->length, data, length);
    stream->length += length;
    return NO_ERROR;
}

static error_t tb2_mqtt_packet_size(const uint8_t *data, size_t length,
                                    size_t *packet_size, size_t *fixed_header_size)
{
    if (length < 2)
    {
        return ERROR_WOULD_BLOCK;
    }
    uint32_t remaining = 0;
    uint32_t multiplier = 1;
    for (size_t index = 1; index <= 4; index++)
    {
        if (index >= length)
        {
            return ERROR_WOULD_BLOCK;
        }
        uint8_t digit = data[index];
        remaining += (uint32_t)(digit & 0x7FU) * multiplier;
        if ((digit & 0x80U) == 0)
        {
            if (remaining > TB2_MQTT_MAX_REMAINING_LENGTH)
            {
                return ERROR_INVALID_LENGTH;
            }
            *fixed_header_size = index + 1;
            *packet_size = *fixed_header_size + remaining;
            return NO_ERROR;
        }
        if (index == 4)
        {
            return ERROR_INVALID_LENGTH;
        }
        multiplier *= 128U;
    }
    return ERROR_INVALID_LENGTH;
}

static tb2_mqtt_qos2_entry_t **tb2_mqtt_qos2_list(tb2_mqtt_passthrough_session_t *session,
                                                   bool_t box_to_upstream)
{
    return box_to_upstream ? &session->blocked_qos2_box : &session->blocked_qos2_upstream;
}

static tb2_mqtt_qos2_entry_t *tb2_mqtt_qos2_find(tb2_mqtt_qos2_entry_t *entry,
                                                  uint16_t packet_id)
{
    while (entry != NULL)
    {
        if (entry->packet_id == packet_id)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static error_t tb2_mqtt_qos2_begin(tb2_mqtt_qos2_entry_t **list,
                                    uint16_t packet_id, bool_t count_blocked,
                                    const char *filter_id,
                                    const char *capture_action)
{
    tb2_mqtt_qos2_entry_t *existing = tb2_mqtt_qos2_find(*list, packet_id);
    if (existing != NULL)
    {
        existing->completed = FALSE;
        existing->count_blocked = count_blocked;
        existing->filter_id = filter_id;
        existing->capture_action = capture_action;
        return NO_ERROR;
    }
    size_t count = 0;
    for (tb2_mqtt_qos2_entry_t *p = *list; p; p = p->next) count++;
    if (count >= TB2_MQTT_INFLIGHT_MAX) return ERROR_OUT_OF_RESOURCES;
    tb2_mqtt_qos2_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL)
        return ERROR_OUT_OF_MEMORY;
    entry->packet_id = packet_id;
    entry->completed = FALSE;
    entry->count_blocked = count_blocked;
    entry->filter_id = filter_id;
    entry->capture_action = capture_action;
    entry->next = *list;
    *list = entry;
    return NO_ERROR;
}

static void tb2_mqtt_qos2_remove(tb2_mqtt_qos2_entry_t **list,
                                  tb2_mqtt_qos2_entry_t *target)
{
    while (*list != NULL)
    {
        if (*list == target)
        {
            *list = target->next;
            osFreeMem(target);
            return;
        }
        list = &(*list)->next;
    }
}

static void tb2_mqtt_qos2_free(tb2_mqtt_qos2_entry_t **list)
{
    while (*list != NULL)
    {
        tb2_mqtt_qos2_entry_t *removed = *list;
        *list = removed->next;
        osFreeMem(removed);
    }
}

static tb2_mqtt_packet_id_entry_t *tb2_mqtt_id_find(tb2_mqtt_packet_id_entry_t *list, uint16_t id)
{
    for (; list != NULL; list = list->next)
        if (list->wire_id == id) return list;
    return NULL;
}
static tb2_mqtt_packet_id_entry_t *tb2_mqtt_packet_id_find_wire(
    tb2_mqtt_passthrough_session_t *session, uint16_t id)
{
    return tb2_mqtt_id_find(session->packet_ids, id);
}
static void tb2_mqtt_id_remove(tb2_mqtt_packet_id_entry_t **list,
                              tb2_mqtt_packet_id_entry_t *target)
{
    for (; *list != NULL; list = &(*list)->next)
        if (*list == target)
        {
            *list = target->next;
            osFreeMem(target->packet);
            osFreeMem(target);
            return;
        }
}
static void tb2_mqtt_packet_id_remove(tb2_mqtt_passthrough_session_t *session,
                                     tb2_mqtt_packet_id_entry_t *target)
{
    tb2_mqtt_id_remove(&session->packet_ids, target);
}
static void tb2_mqtt_packet_ids_clear(tb2_mqtt_packet_id_entry_t **list)
{
    while (*list != NULL) tb2_mqtt_id_remove(list, *list);
}
static error_t tb2_mqtt_packet_id_add(tb2_mqtt_passthrough_session_t *session,
    uint16_t original_id, uint16_t wire_id, uint8_t qos, bool_t local)
{
    size_t count = 0;
    for (tb2_mqtt_packet_id_entry_t *p = session->packet_ids; p; p = p->next) count++;
    if (count >= TB2_MQTT_INFLIGHT_MAX) return ERROR_OUT_OF_RESOURCES;
    tb2_mqtt_packet_id_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL) return ERROR_OUT_OF_MEMORY;
    osMemset(entry, 0, sizeof(*entry));
    entry->original_id = original_id;
    entry->wire_id = wire_id;
    entry->qos = qos;
    entry->local = local;
    entry->packet_type = TB2_MQTT_PACKET_PUBLISH;
    entry->next = session->packet_ids;
    session->packet_ids = entry;
    return NO_ERROR;
}
static error_t tb2_mqtt_allocate_wire_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t *packet_id)
{
    for (uint32_t i = 0; i < UINT16_MAX; i++)
    {
        uint16_t id = session->next_local_packet_id;
        if (id == 0) id = UINT16_MAX;
        session->next_local_packet_id = id > 1 ? id - 1 : UINT16_MAX;
        if (tb2_mqtt_packet_id_find_wire(session, id) == NULL)
        { *packet_id = id; return NO_ERROR; }
    }
    return ERROR_OUT_OF_RESOURCES;
}
static error_t tb2_mqtt_cloud_id_allocate(tb2_mqtt_passthrough_session_t *session,
                                         uint8_t type, uint8_t qos, uint16_t *id)
{
    size_t count = 0;
    for (tb2_mqtt_packet_id_entry_t *p = session->cloud_ids; p; p = p->next) count++;
    if (count >= TB2_MQTT_INFLIGHT_MAX) return ERROR_OUT_OF_RESOURCES;
    do { if (++session->next_cloud_packet_id == 0) session->next_cloud_packet_id = 1; }
    while (tb2_mqtt_id_find(session->cloud_ids, session->next_cloud_packet_id));
    tb2_mqtt_packet_id_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL) return ERROR_OUT_OF_MEMORY;
    osMemset(entry, 0, sizeof(*entry));
    entry->wire_id = *id = session->next_cloud_packet_id;
    entry->packet_type = type;
    entry->qos = qos;
    entry->next = session->cloud_ids;
    session->cloud_ids = entry;
    return NO_ERROR;
}
/* Bound the retransmission state independently of worker handoff queues. */
static error_t tb2_mqtt_id_store_packet(tb2_mqtt_packet_id_entry_t *list,
    tb2_mqtt_packet_id_entry_t *entry, const uint8_t *packet, size_t length)
{
    size_t bytes = length;
    if (length > TB2_MQTT_BUFFER_LIMIT) return ERROR_OUT_OF_RESOURCES;
    for (tb2_mqtt_packet_id_entry_t *p = list; p; p = p->next)
        if (p != entry)
        {
            if (p->packet_length > TB2_MQTT_BUFFER_LIMIT - bytes) return ERROR_OUT_OF_RESOURCES;
            bytes += p->packet_length;
        }
    uint8_t *copy = osAllocMem(length);
    if (copy == NULL) return ERROR_OUT_OF_MEMORY;
    osMemcpy(copy, packet, length);
    osFreeMem(entry->packet);
    entry->packet = copy;
    entry->packet_length = length;
    return NO_ERROR;
}

static error_t tb2_mqtt_rewrite_packet_id(const uint8_t *packet,
                                           size_t packet_size,
                                           size_t packet_id_offset,
                                           uint16_t packet_id,
                                           uint8_t **rewritten)
{
    if (packet_id_offset + 2 > packet_size)
        return ERROR_INVALID_LENGTH;
    *rewritten = osAllocMem(packet_size);
    if (*rewritten == NULL)
        return ERROR_OUT_OF_MEMORY;
    osMemcpy(*rewritten, packet, packet_size);
    (*rewritten)[packet_id_offset] = (uint8_t)(packet_id >> 8);
    (*rewritten)[packet_id_offset + 1] = (uint8_t)packet_id;
    return NO_ERROR;
}

static error_t tb2_mqtt_send_generated_ack(tb2_mqtt_passthrough_session_t *session,
                                           bool_t source_box_to_upstream, uint8_t type,
                                           uint16_t packet_id,
                                           const char *capture_action)
{
    uint8_t packet[] = {(uint8_t)((type << 4) | (type == TB2_MQTT_PACKET_PUBREL ? 2 : 0)), 0x02U,
                        (uint8_t)(packet_id >> 8), (uint8_t)packet_id};
    bool_t ack_box_to_upstream = !source_box_to_upstream;
    TRACE_DEBUG("TB2 MQTT proxy generated=%s direction=%s packet_id=%u\r\n",
                tb2_mqtt_packet_type_name(type),
                ack_box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                (unsigned)packet_id);
    return tb2_mqtt_record_packet_ex(
        session, ack_box_to_upstream, packet, sizeof(packet), NULL, 0,
        type, NULL, TRUE, NULL, TRUE, TRUE, capture_action,
        packet_id, packet_id, FALSE, FALSE, 0, NULL);
}

static error_t tb2_mqtt_parse_publish(const uint8_t *packet, size_t packet_size,
                                      size_t fixed_header_size, char **topic,
                                      const uint8_t **payload, size_t *payload_len,
                                      uint8_t *qos, uint16_t *packet_id,
                                      size_t *packet_id_offset)
{
    *qos = (packet[0] >> 1) & 0x03U;
    if (*qos == 3 || (*qos == 0 && (packet[0] & 8)) || fixed_header_size + 2 > packet_size)
        return ERROR_INVALID_LENGTH;
    size_t offset = fixed_header_size;
    size_t topic_len = ((size_t)packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    if (topic_len == 0 || topic_len > packet_size - offset)
        return ERROR_INVALID_LENGTH;
    *topic = osAllocMem(topic_len + 1);
    if (*topic == NULL)
        return ERROR_OUT_OF_MEMORY;
    osMemcpy(*topic, packet + offset, topic_len);
    (*topic)[topic_len] = '\0';
    if (!tb2_mqtt_utf8_valid((const uint8_t *)*topic, topic_len) ||
        memchr(*topic, '+', topic_len) || memchr(*topic, '#', topic_len))
    {
        osFreeMem(*topic);
        *topic = NULL;
        return ERROR_INVALID_LENGTH;
    }
    offset += topic_len;
    *packet_id = 0;
    *packet_id_offset = 0;
    if (*qos > 0)
    {
        if (packet_size - offset < 2)
        {
            osFreeMem(*topic);
            *topic = NULL;
            return ERROR_INVALID_LENGTH;
        }
        *packet_id_offset = offset;
        *packet_id = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
        offset += 2;
        if (*packet_id == 0)
        {
            osFreeMem(*topic);
            *topic = NULL;
            return ERROR_INVALID_LENGTH;
        }
    }
    *payload = packet + offset;
    *payload_len = packet_size - offset;
    return NO_ERROR;
}

/* Called only after the complete packet was written, never on queue acceptance. */
static void tb2_mqtt_notify_publish_completed(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *packet, size_t length)
{
    if (session->publish_completed == NULL) return;
    size_t size, header, offset, payload_length;
    uint8_t qos; uint16_t id; char *topic = NULL; const uint8_t *payload = NULL;
    if (!tb2_mqtt_packet_size(packet, length, &size, &header) &&
        !tb2_mqtt_parse_publish(packet, size, header, &topic, &payload,
            &payload_length, &qos, &id, &offset))
        session->publish_completed(session->observer_context, box_to_upstream,
            topic, payload, payload_length);
    osFreeMem(topic);
}

static error_t tb2_mqtt_rebuild_publish(
    const uint8_t *packet, size_t packet_size, size_t fixed_header_size,
    const uint8_t *payload, const uint8_t *replacement_payload,
    size_t replacement_payload_len, size_t packet_id_offset,
    uint8_t **rebuilt, size_t *rebuilt_size,
    size_t *rebuilt_packet_id_offset)
{
    if (packet == NULL || payload == NULL || replacement_payload == NULL ||
        rebuilt == NULL || rebuilt_size == NULL ||
        rebuilt_packet_id_offset == NULL ||
        payload < packet + fixed_header_size || payload > packet + packet_size)
    {
        return ERROR_INVALID_PARAMETER;
    }

    size_t variable_header_len =
        (size_t)(payload - (packet + fixed_header_size));
    if (variable_header_len > TB2_MQTT_MAX_REMAINING_LENGTH ||
        replacement_payload_len > TB2_MQTT_MAX_REMAINING_LENGTH -
                                      variable_header_len)
    {
        return ERROR_INVALID_LENGTH;
    }
    size_t remaining_len = variable_header_len + replacement_payload_len;

    uint8_t remaining_bytes[4];
    size_t remaining_count = 0;
    size_t value = remaining_len;
    do
    {
        uint8_t encoded = (uint8_t)(value % 128U);
        value /= 128U;
        if (value > 0)
            encoded |= 0x80U;
        remaining_bytes[remaining_count++] = encoded;
    } while (value > 0 && remaining_count < sizeof(remaining_bytes));
    if (value > 0)
        return ERROR_INVALID_LENGTH;

    size_t output_size = 1 + remaining_count + remaining_len;
    uint8_t *output = osAllocMem(output_size);
    if (output == NULL)
        return ERROR_OUT_OF_MEMORY;

    size_t position = 0;
    output[position++] = packet[0];
    osMemcpy(output + position, remaining_bytes, remaining_count);
    position += remaining_count;
    osMemcpy(output + position, packet + fixed_header_size,
             variable_header_len);
    position += variable_header_len;
    osMemcpy(output + position, replacement_payload, replacement_payload_len);
    position += replacement_payload_len;

    *rebuilt_packet_id_offset = packet_id_offset == 0 ? 0 :
        1 + remaining_count + (packet_id_offset - fixed_header_size);
    *rebuilt = output;
    *rebuilt_size = position;
    return NO_ERROR;
}

/* MQTT strings are validated before they become a topic, client ID or path key. */
static bool_t tb2_mqtt_utf8_valid(const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length;)
    {
        uint32_t cp = text[i++];
        unsigned trailing = 0;
        uint32_t minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; trailing = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; trailing = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; trailing = 3; minimum = 0x10000; }
        else if (cp >= 0x80) return FALSE;
        if (trailing > length - i) return FALSE;
        while (trailing--)
        {
            if ((text[i] & 0xc0) != 0x80) return FALSE;
            cp = (cp << 6) | (text[i++] & 0x3f);
        }
        if (cp < minimum || cp == 0 || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xfdd0 && cp <= 0xfdef) ||
            (cp & 0xffff) == 0xfffe || (cp & 0xffff) == 0xffff) return FALSE;
    }
    return TRUE;
}
bool_t tb2_mqtt_topic_filter_valid(const uint8_t *text, size_t length)
{
    if (length == 0 || !tb2_mqtt_utf8_valid(text, length)) return FALSE;
    for (size_t i = 0; i < length; i++)
    {
        if (text[i] == '#' && (i + 1 != length || (i && text[i - 1] != '/'))) return FALSE;
        if (text[i] == '+' && ((i && text[i - 1] != '/') ||
            (i + 1 < length && text[i + 1] != '/'))) return FALSE;
    }
    return TRUE;
}
static bool_t tb2_mqtt_read_field(const uint8_t *packet, size_t length, size_t *offset,
    const uint8_t **value, size_t *size, bool_t utf8)
{
    if (*offset > length || length - *offset < 2) return FALSE;
    *size = ((size_t)packet[*offset] << 8) | packet[*offset + 1];
    *offset += 2;
    if (*size > length - *offset) return FALSE;
    *value = packet + *offset;
    *offset += *size;
    return !utf8 || tb2_mqtt_utf8_valid(*value, *size);
}
static uint8_t *tb2_mqtt_make_packet(uint8_t first, const uint8_t *body,
                                    size_t length, size_t *packet_length)
{
    if (length > TB2_MQTT_MAX_REMAINING_LENGTH) return NULL;
    uint8_t header[5] = {first};
    size_t count = 1, remaining = length;
    do {
        uint8_t digit = remaining % 128;
        remaining /= 128;
        header[count++] = digit | (remaining ? 0x80 : 0);
    } while (remaining);
    uint8_t *packet = osAllocMem(count + length);
    if (packet == NULL) return NULL;
    osMemcpy(packet, header, count);
    if (length) osMemcpy(packet + count, body, length);
    *packet_length = count + length;
    return packet;
}
/* Apply the current local grant, including an UNSUBSCRIBE that raced with
 * already received Cloud bytes. Keep the source leg's QoS handshake separate. */
static error_t tb2_mqtt_limit_publish_qos(const uint8_t *packet, size_t length,
    size_t id_offset, uint8_t qos, uint8_t **output, size_t *output_length)
{
    size_t size, header;
    error_t error = tb2_mqtt_packet_size(packet, length, &size, &header);
    if (error || id_offset < header || id_offset + 2 > size) return ERROR_INVALID_LENGTH;
    size_t body_length = size - header - (qos == 0 ? 2 : 0);
    uint8_t *body = osAllocMem(body_length);
    if (body == NULL) return ERROR_OUT_OF_MEMORY;
    size_t prefix = id_offset - header;
    osMemcpy(body, packet + header, prefix);
    osMemcpy(body + prefix, packet + id_offset + (qos == 0 ? 2 : 0),
        body_length - prefix);
    uint8_t first = (packet[0] & (uint8_t)~6U) | (qos << 1);
    if (qos == 0) first &= (uint8_t)~8U;
    *output = tb2_mqtt_make_packet(first, body, body_length, output_length);
    osFreeMem(body);
    return *output != NULL ? NO_ERROR : ERROR_OUT_OF_MEMORY;
}

static error_t tb2_mqtt_local_reply(tb2_mqtt_passthrough_session_t *session,
    uint8_t first, const uint8_t *body, size_t length, const char *action)
{
    size_t packet_length = 0;
    uint8_t *packet = tb2_mqtt_make_packet(first, body, length, &packet_length);
    if (packet == NULL) return ERROR_OUT_OF_MEMORY;
    error_t error = tb2_mqtt_record_packet_ex(session, FALSE, packet, packet_length,
        NULL, 0, first >> 4, NULL, TRUE, NULL, TRUE, TRUE, action, 0, 0, FALSE, FALSE, 0, NULL);
    osFreeMem(packet);
    return error;
}
static error_t tb2_mqtt_cloud_packet(tb2_mqtt_passthrough_session_t *session,
    uint8_t first, const uint8_t *body, size_t length, const char *action)
{
    size_t packet_length = 0;
    uint8_t *packet = tb2_mqtt_make_packet(first, body, length, &packet_length);
    if (packet == NULL) return ERROR_OUT_OF_MEMORY;
    error_t error = tb2_mqtt_record_packet_ex(session, TRUE, packet, packet_length,
        NULL, 0, first >> 4, NULL, TRUE, NULL, TRUE, TRUE, action, 0, 0, FALSE, FALSE, 0, NULL);
    osFreeMem(packet);
    return error;
}
static void tb2_mqtt_saved_clear(tb2_mqtt_saved_session_t *saved)
{
    tb2_mqtt_qos2_free(&saved->incoming);
    tb2_mqtt_packet_ids_clear(&saved->outgoing);
    mqtt_response_history_reset(&saved->response_history);
    osMemset(saved, 0, sizeof(*saved));
}
/* Snapshot subscriptions while callbacks are safe, not from close() which is
 * already called under the server app-control mutex. */
static void tb2_mqtt_save_subscriptions(tb2_mqtt_passthrough_session_t *session)
{
    if (session->saved_slot < 0 || session->subscription_snapshot == NULL) return;
    tb2_mqtt_saved_session_t *saved = &saved_sessions[session->saved_slot];
    if (saved->owner != session->owner) return;
    saved->subscription_count = session->subscription_snapshot(session->observer_context,
        saved->subscriptions, TB2_MQTT_SUBSCRIPTIONS_MAX);
}
static error_t tb2_mqtt_restore_subscriptions(tb2_mqtt_passthrough_session_t *session,
                                              tb2_mqtt_saved_session_t *saved)
{
    if (!saved->subscription_count || session->subscription_apply == NULL) return NO_ERROR;
    uint8_t body[2 + TB2_MQTT_SUBSCRIPTIONS_MAX * 259];
    size_t offset = 2;
    body[0] = 0; body[1] = 1;
    for (size_t i = 0; i < saved->subscription_count; i++)
    {
        size_t length = osStrlen(saved->subscriptions[i].topic);
        body[offset++] = (uint8_t)(length >> 8);
        body[offset++] = (uint8_t)length;
        osMemcpy(body + offset, saved->subscriptions[i].topic, length);
        offset += length;
        body[offset++] = saved->subscriptions[i].qos;
    }
    uint8_t codes[TB2_MQTT_SUBSCRIPTIONS_MAX];
    size_t count = 0;
    return session->subscription_apply(session->observer_context, FALSE, body, offset,
        codes, sizeof(codes), &count);
}
static error_t tb2_mqtt_accept_connect(tb2_mqtt_passthrough_session_t *session,
    const uint8_t *packet, size_t length, size_t header)
{
    if (session->established || session->connect_packet != NULL) return ERROR_INVALID_TYPE;
    size_t pos = header, n = 0;
    const uint8_t *field = NULL;
    if (!tb2_mqtt_read_field(packet, length, &pos, &field, &n, TRUE) ||
        length - pos < 4) return ERROR_INVALID_LENGTH;
    uint8_t level = packet[pos++], flags = packet[pos++];
    if (!((n == 4 && !osMemcmp(field, "MQTT", 4) && level == 4) ||
          (n == 6 && !osMemcmp(field, "MQIsdp", 6) && level == 3))) return ERROR_INVALID_VERSION;
    if ((flags & 1) || (!(flags & 4) && (flags & 0x38)) ||
        ((flags >> 3) & 3) == 3 || ((flags & 0x40) && !(flags & 0x80)))
        return ERROR_INVALID_TYPE;
    session->connect_flags_offset = pos - 1;
    session->keepalive = ((uint16_t)packet[pos] << 8) | packet[pos + 1];
    pos += 2;
    session->clean_session = (flags & 2) != 0;
    if (!tb2_mqtt_read_field(packet, length, &pos, &field, &n, TRUE) ||
        n == 0 || n >= sizeof(session->client_id)) return ERROR_INVALID_LENGTH;
    osMemcpy(session->client_id, field, n);
    session->client_id[n] = '\0';
    session->will_begin = pos;
    if (flags & 4)
    {
        if (!tb2_mqtt_read_field(packet, length, &pos, &field, &n, TRUE) || n == 0 ||
            memchr(field, '+', n) || memchr(field, '#', n) ||
            !tb2_mqtt_read_field(packet, length, &pos, &field, &n, FALSE))
            return ERROR_INVALID_LENGTH;
    }
    session->will_end = pos;
    if ((flags & 0x80) && !tb2_mqtt_read_field(packet, length, &pos, &field, &n, TRUE))
        return ERROR_INVALID_LENGTH;
    if ((flags & 0x40) && !tb2_mqtt_read_field(packet, length, &pos, &field, &n, FALSE))
        return ERROR_INVALID_LENGTH;
    if (pos != length) return ERROR_INVALID_LENGTH;

    int slot = -1, free_slot = -1;
    for (size_t i = 0; i < TB2_MQTT_SESSION_MAX; i++)
    {
        if (!saved_sessions[i].used && free_slot < 0) free_slot = (int)i;
        if (saved_sessions[i].used &&
            !osStrcasecmp(saved_sessions[i].box, session->box_settings->commonName))
        { slot = (int)i; break; }
    }
    if (slot < 0) slot = free_slot;
    if (slot < 0)
    {
        const uint8_t unavailable[] = {0, 3};
        tb2_mqtt_local_reply(session, 0x20, unavailable, sizeof(unavailable), "local_connack_rejected");
        return ERROR_OUT_OF_RESOURCES;
    }
    tb2_mqtt_saved_session_t *saved = &saved_sessions[slot];
    bool_t resume = saved->used && saved->persistent && !session->clean_session &&
        !osStrcmp(saved->client_id, session->client_id);
    if (saved->active != NULL)
    {
        tb2_mqtt_passthrough_session_t *old = saved->active;
        osAcquireMutex(&old->ids_mutex);
        if (resume)
        {
            saved->incoming = old->blocked_qos2_box;
            old->blocked_qos2_box = NULL;
            /* Overlay Freshness retries own local IDs; they are not resumed
             * through a different concrete connection. */
            tb2_mqtt_packet_id_entry_t *p = old->packet_ids;
            while (p != NULL)
            {
                tb2_mqtt_packet_id_entry_t *next = p->next;
                if (p->local) tb2_mqtt_packet_id_remove(old, p);
                p = next;
            }
            saved->response_history = old->response_history;
            mqtt_response_history_init(&old->response_history);
            saved->outgoing = old->packet_ids;
            old->packet_ids = NULL;
            saved->next_id = old->next_local_packet_id;
        }
        osReleaseMutex(&old->ids_mutex);
    }
    if (!resume) tb2_mqtt_saved_clear(saved);
    session->saved_slot = slot;
    saved->used = TRUE;
    saved->owner = session->owner;
    saved->persistent = !session->clean_session;
    saved->active = session;
    osStrncpy(saved->box, session->box_settings->commonName, sizeof(saved->box) - 1);
    osStrcpy(saved->client_id, session->client_id);
    if (resume)
    {
        session->blocked_qos2_box = saved->incoming; saved->incoming = NULL;
        session->packet_ids = saved->outgoing; saved->outgoing = NULL;
        session->next_local_packet_id = saved->next_id;
        session->response_history = saved->response_history;
        mqtt_response_history_init(&saved->response_history);
        error_t error = tb2_mqtt_restore_subscriptions(session, saved);
        if (error) return error;
    }
    session->connect_packet = osAllocMem(length);
    if (session->connect_packet == NULL) return ERROR_OUT_OF_MEMORY;
    osMemcpy(session->connect_packet, packet, length);
    session->connect_length = length;
    session->connect_header = header;
    const uint8_t connack[] = {resume ? 1 : 0, 0};
    error_t error = tb2_mqtt_local_reply(session, 0x20, connack, sizeof(connack), "local_connack");
    if (error) return error;
    session->established = TRUE;
    session->last_box_rx = osGetSystemTime();
    tb2_mqtt_save_subscriptions(session);
    /* Resume only already transmitted protocol exchanges, never an offline queue. */
    for (tb2_mqtt_packet_id_entry_t *p = session->packet_ids; p; p = p->next)
        if (!p->local && p->packet != NULL)
        {
            if (!p->pubrel) p->packet[0] |= 0x08;
            error = tb2_mqtt_record_packet_ex(session, FALSE, p->packet, p->packet_length,
                NULL, 0, p->pubrel ? 6 : 3, NULL, TRUE, NULL, TRUE, TRUE,
                "local_session_resume", p->wire_id, p->wire_id, FALSE, FALSE, 0, NULL);
            if (error) return error;
        }
    TRACE_INFO("TB2 MQTT local session established overlay=%u resumed=%s\r\n",
        (unsigned)session->box_settings->internal.overlayNumber, resume ? "true" : "false");
    return NO_ERROR;
}

static error_t tb2_mqtt_sync_subscriptions(tb2_mqtt_passthrough_session_t *session)
{
    tb2_mqtt_subscription_t current[TB2_MQTT_SUBSCRIPTIONS_MAX];
    size_t count = session->subscription_snapshot != NULL ?
        session->subscription_snapshot(session->observer_context, current, TB2_MQTT_SUBSCRIPTIONS_MAX) : 0;
    uint8_t body[2 + TB2_MQTT_SUBSCRIPTIONS_MAX * 259];
    size_t offset = 2;
    for (size_t i = 0; i < session->synced_count; i++)
    {
        bool_t present = FALSE;
        for (size_t j = 0; j < count; j++)
            if (!osStrcmp(session->synced[i].topic, current[j].topic)) present = TRUE;
        if (!present)
        {
            size_t length = osStrlen(session->synced[i].topic);
            body[offset++] = (uint8_t)(length >> 8); body[offset++] = (uint8_t)length;
            osMemcpy(body + offset, session->synced[i].topic, length); offset += length;
        }
    }
    uint16_t id;
    if (offset > 2)
    {
        error_t error = tb2_mqtt_cloud_id_allocate(session, 10, 0, &id);
        if (error) return error;
        body[0] = (uint8_t)(id >> 8); body[1] = (uint8_t)id;
        error = tb2_mqtt_cloud_packet(session, 0xa2, body, offset, "upstream_unsubscribe");
        if (error) return error;
    }
    offset = 2;
    size_t changed = 0;
    for (size_t i = 0; i < count; i++)
    {
        bool_t same = FALSE;
        for (size_t j = 0; j < session->synced_count; j++)
            if (!osStrcmp(current[i].topic, session->synced[j].topic) &&
                current[i].qos == session->synced[j].qos) same = TRUE;
        if (!same)
        {
            size_t length = osStrlen(current[i].topic);
            body[offset++] = (uint8_t)(length >> 8); body[offset++] = (uint8_t)length;
            osMemcpy(body + offset, current[i].topic, length); offset += length;
            body[offset++] = current[i].qos;
            changed++;
        }
    }
    if (changed)
    {
        error_t error = tb2_mqtt_cloud_id_allocate(session, 8, 0, &id);
        if (error) return error;
        tb2_mqtt_id_find(session->cloud_ids, id)->original_id = (uint16_t)changed;
        body[0] = (uint8_t)(id >> 8); body[1] = (uint8_t)id;
        error = tb2_mqtt_cloud_packet(session, 0x82, body, offset, "upstream_subscribe");
        if (error) return error;
    }
    osMemcpy(session->synced, current, count * sizeof(*current));
    session->synced_count = count;
    session->subscriptions_dirty = FALSE;
    session->cloud_phase = TB2_MQTT_CLOUD_READY;
    for (tb2_mqtt_packet_id_entry_t *p = session->cloud_ids; p; p = p->next)
        if (p->packet_type == 8 || p->packet_type == 10) session->cloud_phase = TB2_MQTT_CLOUD_SUBACK;
    session->cloud_started_at = osGetSystemTime();
    return NO_ERROR;
}

static error_t tb2_mqtt_local_control(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *packet, size_t length,
    size_t header, bool_t *handled)
{
    uint8_t type = packet[0] >> 4;
    *handled = type != 3 && !(type >= 4 && type <= 7);
    if (!*handled) return NO_ERROR;
    error_t error = tb2_mqtt_record_packet_ex(session, box_to_upstream, packet, length,
        NULL, 0, type, NULL, FALSE, NULL, FALSE, TRUE, "session_control",
        0, 0, FALSE, FALSE, 0, NULL);
    if (error) return error;
    if (box_to_upstream)
    {
        if (type == 1) return tb2_mqtt_accept_connect(session, packet, length, header);
        if (!session->established) return ERROR_INVALID_TYPE;
        if (type == 12 && length == header)
            return tb2_mqtt_local_reply(session, 0xd0, NULL, 0, "local_pingresp");
        if (type == 14 && length == header)
        { session->clean_disconnect = TRUE; return ERROR_END_OF_STREAM; }
        if (type != 8 && type != 10) return ERROR_INVALID_TYPE;
        if (session->subscription_apply == NULL || length - header < 2) return ERROR_INVALID_LENGTH;
        uint8_t codes[TB2_MQTT_SUBSCRIPTIONS_MAX], reply[2 + TB2_MQTT_SUBSCRIPTIONS_MAX];
        size_t count = 0;
        error = session->subscription_apply(session->observer_context, type == 10,
            packet + header, length - header, codes, sizeof(codes), &count);
        if (error) return error;
        reply[0] = packet[header]; reply[1] = packet[header + 1];
        if (type == 8) osMemcpy(reply + 2, codes, count);
        error = tb2_mqtt_local_reply(session, type == 8 ? 0x90 : 0xb0,
            reply, type == 8 ? count + 2 : 2, type == 8 ? "local_suback" : "local_unsuback");
        if (error) return error;
        session->subscriptions_dirty = TRUE;
        tb2_mqtt_save_subscriptions(session);
        if (session->control_observer != NULL)
            session->control_observer(session->observer_context,
                type == 8 ? TB2_MQTT_CONTROL_SUBSCRIBE : TB2_MQTT_CONTROL_UNSUBSCRIBE,
                ((uint16_t)reply[0] << 8) | reply[1], packet + header, length - header);
        return NO_ERROR;
    }
    if (type == 2)
    {
        if (session->cloud_phase != TB2_MQTT_CLOUD_CONNACK ||
            length - header != 2 || packet[header] != 0 || packet[header + 1] != 0)
            return ERROR_ACCESS_DENIED;
        session->upstream_error[0] = '\0';
        session->ever_connected = TRUE;
        return tb2_mqtt_sync_subscriptions(session);
    }
    if (type == 13 && length == header)
    { session->ping_pending = FALSE; return NO_ERROR; }
    if (type != 9 && type != 11) return ERROR_INVALID_TYPE;
    if (length - header < 2) return ERROR_INVALID_LENGTH;
    uint16_t id = ((uint16_t)packet[header] << 8) | packet[header + 1];
    tb2_mqtt_packet_id_entry_t *entry = tb2_mqtt_id_find(session->cloud_ids, id);
    if (entry == NULL || entry->packet_type != (type == 9 ? 8 : 10)) return ERROR_INVALID_TYPE;
    if (type == 9)
    {
        if (length - header != 2U + entry->original_id) return ERROR_INVALID_LENGTH;
        for (size_t i = header + 2; i < length; i++)
            if (packet[i] > 2) return ERROR_ACCESS_DENIED;
    }
    else if (length - header != 2) return ERROR_INVALID_LENGTH;
    tb2_mqtt_id_remove(&session->cloud_ids, entry);
    session->cloud_phase = TB2_MQTT_CLOUD_READY;
    for (tb2_mqtt_packet_id_entry_t *p = session->cloud_ids; p; p = p->next)
        if (p->packet_type == 8 || p->packet_type == 10) session->cloud_phase = TB2_MQTT_CLOUD_SUBACK;
    return NO_ERROR;
}

/* Register only a Will that passes the same Internet protection as a PUBLISH.
 * Credentials are copied byte-for-byte; only CleanSession and sanitized Will
 * fields are changed. No decoded credentials are logged. */
static error_t tb2_mqtt_build_cloud_connect(tb2_mqtt_passthrough_session_t *session,
                                           uint8_t **output, size_t *output_length)
{
    uint8_t *packet = session->connect_packet;
    size_t length = session->connect_length;
    uint8_t flags = packet[session->connect_flags_offset] | 2;
    bool_t will = (flags & 4) != 0;
    const uint8_t *topic = NULL, *payload = NULL;
    size_t topic_length = 0, payload_length = 0, pos = session->will_begin;
    mqtt_nocloud_filter_result_t privacy = {0};
    if (will)
    {
        if (!tb2_mqtt_read_field(packet, length, &pos, &topic, &topic_length, TRUE) ||
            !tb2_mqtt_read_field(packet, length, &pos, &payload, &payload_length, FALSE))
            return ERROR_INVALID_LENGTH;
        char *name = osAllocMem(topic_length + 1);
        if (name == NULL) return ERROR_OUT_OF_MEMORY;
        osMemcpy(name, topic, topic_length); name[topic_length] = '\0';
        mqtt_nocloud_filter_publish(session->box_settings, TRUE, name, payload, payload_length, &privacy);
        if (privacy.action == MQTT_NOCLOUD_REWRITE)
        { payload = (const uint8_t *)privacy.payload; payload_length = privacy.payload_len; }
        mqtt_forward_filter_result_t decision = mqtt_forward_filter_evaluate(session->box_settings,
            MQTT_FORWARD_ROUTE_BOX_TO_TONIES, name, payload, payload_length);
        will = privacy.action != MQTT_NOCLOUD_BLOCK && decision.action == MQTT_FORWARD_ACTION_FORWARD &&
            payload_length <= UINT16_MAX;
        osFreeMem(name);
    }
    if (!will) flags &= (uint8_t)~0x3cU;
    size_t body_length = session->will_begin - session->connect_header +
        (will ? 4 + topic_length + payload_length : 0) + length - session->will_end;
    uint8_t *body = osAllocMem(body_length);
    if (body == NULL) { mqtt_nocloud_filter_result_free(&privacy); return ERROR_OUT_OF_MEMORY; }
    pos = session->will_begin - session->connect_header;
    osMemcpy(body, packet + session->connect_header, pos);
    body[session->connect_flags_offset - session->connect_header] = flags;
    if (will)
    {
        body[pos++] = (uint8_t)(topic_length >> 8); body[pos++] = (uint8_t)topic_length;
        osMemcpy(body + pos, topic, topic_length); pos += topic_length;
        body[pos++] = (uint8_t)(payload_length >> 8); body[pos++] = (uint8_t)payload_length;
        osMemcpy(body + pos, payload, payload_length); pos += payload_length;
    }
    osMemcpy(body + pos, packet + session->will_end, length - session->will_end);
    *output = tb2_mqtt_make_packet(0x10, body, body_length, output_length);
    osFreeMem(body);
    mqtt_nocloud_filter_result_free(&privacy);
    return *output != NULL ? NO_ERROR : ERROR_OUT_OF_MEMORY;
}

static error_t tb2_mqtt_process_mapped_control(
    tb2_mqtt_passthrough_session_t *session, bool_t box_to_upstream,
    const uint8_t *packet, size_t packet_size, size_t fixed_header_size,
    uint8_t type, bool_t *handled)
{
    *handled = type >= TB2_MQTT_PACKET_PUBACK && type <= TB2_MQTT_PACKET_PUBCOMP;
    if (!*handled) return NO_ERROR;
    if (packet_size - fixed_header_size != 2) return ERROR_INVALID_LENGTH;
    uint16_t id = ((uint16_t)packet[fixed_header_size] << 8) | packet[fixed_header_size + 1];
    if (id == 0) return ERROR_INVALID_LENGTH;
    error_t error = tb2_mqtt_record_packet_ex(session, box_to_upstream, packet,
        packet_size, NULL, 0, type, NULL, FALSE, NULL, FALSE, TRUE,
        "local_protocol_ack", id, id, FALSE, FALSE, 0, NULL);
    if (error) return error;
    if (type == TB2_MQTT_PACKET_PUBREL)
    {
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        tb2_mqtt_qos2_entry_t *entry = tb2_mqtt_qos2_find(*list, id);
        if (entry != NULL) tb2_mqtt_qos2_remove(list, entry);
        return tb2_mqtt_send_generated_ack(session, box_to_upstream,
            TB2_MQTT_PACKET_PUBCOMP, id, "local_pubcomp");
    }
    osAcquireMutex(&session->ids_mutex);
    tb2_mqtt_packet_id_entry_t **list = box_to_upstream ?
        &session->packet_ids : &session->cloud_ids;
    tb2_mqtt_packet_id_entry_t *entry = tb2_mqtt_id_find(*list, id);
    bool_t local_ack = FALSE, send_pubrel = FALSE;
    if (entry != NULL)
    {
        if (type == TB2_MQTT_PACKET_PUBREC && entry->qos == 2)
        {
            uint8_t pubrel[] = {0x62, 2, (uint8_t)(id >> 8), (uint8_t)id};
            entry->pubrel = TRUE;
            error = tb2_mqtt_id_store_packet(*list, entry, pubrel, sizeof(pubrel));
            send_pubrel = !error;
        }
        else if ((type == TB2_MQTT_PACKET_PUBACK && entry->qos == 1) ||
                 (type == TB2_MQTT_PACKET_PUBCOMP && entry->qos == 2 && entry->pubrel))
        {
            local_ack = box_to_upstream && entry->local;
            tb2_mqtt_id_remove(list, entry);
        }
        else error = ERROR_INVALID_TYPE;
    }
    osReleaseMutex(&session->ids_mutex);
    if (!error && send_pubrel)
        error = tb2_mqtt_send_generated_ack(session, box_to_upstream,
            TB2_MQTT_PACKET_PUBREL, id, "local_pubrel");
    if (!error && local_ack && session->control_observer != NULL)
        session->control_observer(session->observer_context,
            TB2_MQTT_CONTROL_LOCAL_PUBACK, id, NULL, 0);
    return error;
}

static error_t tb2_mqtt_process_packet(tb2_mqtt_passthrough_session_t *session,
    bool_t box_to_upstream, const uint8_t *packet, size_t packet_size, size_t fixed_header_size)
{
    uint8_t type = packet[0] >> 4;
    uint8_t flags = packet[0] & 0x0fU;
    if (type == 0 || type > 14 ||
        (type != TB2_MQTT_PACKET_PUBLISH &&
         flags != ((type == 6 || type == 8 || type == 10) ? 2 : 0)))
        return ERROR_INVALID_TYPE;
    if (box_to_upstream && !session->established && type != 1)
        return ERROR_INVALID_TYPE;
    if (box_to_upstream) session->last_box_rx = osGetSystemTime();
    bool_t handled = FALSE;
    error_t error = tb2_mqtt_local_control(session, box_to_upstream,
        packet, packet_size, fixed_header_size, &handled);
    if (error || handled) return error;
    error = tb2_mqtt_process_mapped_control(session, box_to_upstream, packet,
        packet_size, fixed_header_size, type, &handled);
    if (error || handled) return error;
    if (type != TB2_MQTT_PACKET_PUBLISH || !session->established)
        return ERROR_INVALID_TYPE;

    char *topic = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    size_t packet_id_offset = 0;
    uint8_t qos = 0;
    uint16_t packet_id = 0;
    error = tb2_mqtt_parse_publish(packet, packet_size, fixed_header_size,
                                   &topic, &payload, &payload_len, &qos,
                                   &packet_id, &packet_id_offset);
    if (error)
        return error;

    /* QoS2 Method B: process once, retain only receipt identity until PUBREL.
     * Completed IDs are immediately reusable; DUP alone is not an identity. */
    if (qos == 2)
    {
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        if (tb2_mqtt_qos2_find(*list, packet_id) != NULL)
        {
            error = tb2_mqtt_record_packet_ex(session, box_to_upstream, packet,
                packet_size, NULL, 0, type, topic, FALSE, NULL, FALSE, TRUE,
                "local_qos2_duplicate", packet_id, packet_id, FALSE, FALSE, 0, NULL);
            if (!error) error = tb2_mqtt_send_generated_ack(session, box_to_upstream,
                TB2_MQTT_PACKET_PUBREC, packet_id, "local_pubrec");
            osFreeMem(topic);
            return error;
        }
        error = tb2_mqtt_qos2_begin(list, packet_id, FALSE, NULL, NULL);
        if (error)
        {
            /* Do not acknowledge acceptance without a QoS2 receipt slot.
             * The peer can retry after outstanding exchanges finish. */
            tb2_mqtt_record_packet_ex(session, box_to_upstream, packet, packet_size,
                NULL, 0, type, topic, FALSE, "transport.inflight_full", FALSE, TRUE,
                "qos2_backpressure", packet_id, 0, FALSE, FALSE, 0, NULL);
            osFreeMem(topic);
            return NO_ERROR;
        }
    }

    const mqtt_response_history_record_t *replay = NULL;
    if (box_to_upstream && qos == 1)
    {
        if (!(packet[0] & 8)) mqtt_response_history_forget(&session->response_history, packet_id);
        else replay = mqtt_response_history_find(&session->response_history, packet_id,
            packet, packet_size, osGetSystemTime());
    }
    tb2_mqtt_observer_result_t observer_result;
    osMemset(&observer_result, 0, sizeof(observer_result));
    observer_result.action = TB2_MQTT_OBSERVER_FORWARD;
    if (replay != NULL)
    {
        observer_result.action = replay->consume ? TB2_MQTT_OBSERVER_CONSUME : TB2_MQTT_OBSERVER_REWRITE;
        observer_result.locally_processed = TRUE;
        observer_result.capture_action = "local_response_replay";
        if (!replay->consume)
        {
            observer_result.payload = osAllocMem(replay->payload_len ? replay->payload_len : 1);
            if (observer_result.payload == NULL)
            {
                session->privacy_failed = TRUE;
                tb2_mqtt_cloud_reset(session, "local_response_history_full");
                observer_result.action = TB2_MQTT_OBSERVER_CONSUME;
            }
            else
            {
                if (replay->payload_len) osMemcpy(observer_result.payload, replay->payload, replay->payload_len);
                observer_result.payload_len = replay->payload_len;
            }
        }
    }
    else if (session->observer != NULL)
    {
        error = session->observer(session->observer_context, box_to_upstream,
            topic, payload, payload_len, qos, &observer_result);
        if (error)
        {
            osFreeMem(observer_result.payload);
            osFreeMem(topic);
            return error;
        }
        if (box_to_upstream && qos == 1 && observer_result.action != TB2_MQTT_OBSERVER_FORWARD &&
            mqtt_response_history_remember(&session->response_history, packet_id,
                packet, packet_size, observer_result.action == TB2_MQTT_OBSERVER_CONSUME,
                observer_result.payload, observer_result.payload_len, osGetSystemTime()))
        {
            /* Pending may already have been consumed. Fail closed for the
             * Internet leg, without disconnecting the healthy local box. */
            session->privacy_failed = TRUE;
            tb2_mqtt_cloud_reset(session, "local_response_history_full");
        }
    }

    const bool_t local_consume =
        observer_result.action == TB2_MQTT_OBSERVER_CONSUME;
    const bool_t local_rewrite =
        observer_result.action == TB2_MQTT_OBSERVER_REWRITE &&
        observer_result.payload != NULL;

    const uint8_t *effective_payload = local_rewrite ?
                                                   observer_result.payload : payload;
    size_t effective_payload_len = local_rewrite ?
                                               observer_result.payload_len : payload_len;
    const char *filter_id = local_consume || local_rewrite ?
                                observer_result.filter_id : NULL;
    bool_t blocked = local_consume;
    mqtt_nocloud_filter_result_t nocloud_result;
    osMemset(&nocloud_result, 0, sizeof(nocloud_result));
    nocloud_result.action = MQTT_NOCLOUD_ALLOW;
    if (!blocked)
    {
        mqtt_nocloud_filter_publish(session->box_settings, box_to_upstream,
                                    topic, effective_payload,
                                    effective_payload_len,
                                    &nocloud_result);
        if (nocloud_result.action == MQTT_NOCLOUD_BLOCK)
        {
            blocked = TRUE;
            filter_id = nocloud_result.filter_id;
        }
    }
    const uint8_t *filtered_payload =
        nocloud_result.action == MQTT_NOCLOUD_REWRITE ?
            (const uint8_t *)nocloud_result.payload : effective_payload;
    size_t filtered_payload_len =
        nocloud_result.action == MQTT_NOCLOUD_REWRITE ?
            nocloud_result.payload_len : effective_payload_len;
    mqtt_forward_filter_result_t manual_decision = {
        .route = box_to_upstream ? MQTT_FORWARD_ROUTE_BOX_TO_TONIES :
                                  MQTT_FORWARD_ROUTE_TONIES_TO_BOX,
        .action = MQTT_FORWARD_ACTION_BLOCK,
        .setting_id = NULL,
        .reason = local_consume ? MQTT_FORWARD_REASON_NOT_EVALUATED_LOCAL_CONSUME :
                                 MQTT_FORWARD_REASON_NOT_EVALUATED_AUTOMATIC_BLOCK,
    };
    if (nocloud_result.action == MQTT_NOCLOUD_REWRITE)
        filter_id = nocloud_result.filter_id;
    if (nocloud_result.manual_filter_applied)
    {
        manual_decision.action = blocked ? MQTT_FORWARD_ACTION_BLOCK :
                                           MQTT_FORWARD_ACTION_FORWARD;
        manual_decision.setting_id = "mqtt_client_upstream.forward.logs.*";
        manual_decision.reason = blocked ? MQTT_FORWARD_REASON_RULE_BLOCKED :
                                           MQTT_FORWARD_REASON_RULE_ALLOWED;
    }
    else if (!blocked)
    {
        manual_decision = mqtt_forward_filter_evaluate(
            session->box_settings, manual_decision.route, topic,
            filtered_payload, filtered_payload_len);
        blocked = manual_decision.action == MQTT_FORWARD_ACTION_BLOCK;
        if (blocked)
            filter_id = manual_decision.setting_id;
    }

    uint8_t *rebuilt_packet = NULL;
    size_t forwarded_packet_size = packet_size;
    size_t forwarded_packet_id_offset = packet_id_offset;
    const uint8_t *forwarded_packet = packet;
    // The last sanitation stage is authoritative for the wire packet and the
    // bounded replay entry above; never restore an earlier observer payload.
    if (!blocked && (local_rewrite ||
                     nocloud_result.action == MQTT_NOCLOUD_REWRITE))
    {
        error_t rebuild_error = tb2_mqtt_rebuild_publish(
            packet, packet_size, fixed_header_size, payload,
            filtered_payload, filtered_payload_len, packet_id_offset, &rebuilt_packet,
            &forwarded_packet_size, &forwarded_packet_id_offset);
        if (rebuild_error && local_rewrite)
        {
            error = rebuild_error;
        }
        else if (rebuild_error)
        {
            TRACE_WARNING("TB2 MQTT proxy direction=%s topic='%s' action=block"
                          " filter=%s rebuild_error=%s code=%d\r\n",
                          box_to_upstream ? "box_to_upstream" :
                                            "upstream_to_box",
                          topic,
                          nocloud_result.filter_id != NULL ?
                              nocloud_result.filter_id : "nocloud.invalid",
                          error2text(rebuild_error), (int)rebuild_error);
            blocked = TRUE;
            filter_id = nocloud_result.filter_id != NULL ?
                            nocloud_result.filter_id : "nocloud.invalid";
            nocloud_result.action = MQTT_NOCLOUD_BLOCK;
        }
        else
        {
            forwarded_packet = rebuilt_packet;
        }
    }

    const char *decision = local_consume ? "consume" :
                           blocked ? "block" :
                           local_rewrite ? "rewrite" :
                           nocloud_result.action == MQTT_NOCLOUD_REWRITE ?
                               "rewrite" : "forward";
    TRACE_DEBUG("TB2 MQTT proxy direction=%s packet_type=PUBLISH topic='%s' qos=%u"
                " packet_id=%u action=%s filter=%s payload_len=%" PRIuSIZE
                " removed=%" PRIuSIZE " route=%s manual_filter=%s manual_decision=%s\r\n",
                box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                topic, (unsigned)qos, (unsigned)packet_id,
                decision, filter_id != NULL ? filter_id : "-", filtered_payload_len,
                nocloud_result.removed_count,
                mqtt_forward_route_name(manual_decision.route),
                manual_decision.setting_id != NULL ? manual_decision.setting_id : "-",
                mqtt_forward_reason_name(manual_decision.reason));

    uint8_t *wire_packet = NULL;
    uint8_t *qos_packet = NULL;
    uint8_t target_qos = qos;
    bool_t transport_blocked = FALSE;
    if (!blocked && !box_to_upstream)
    {
        int grant = session->subscription_qos != NULL ?
            session->subscription_qos(session->observer_context, topic) : -1;
        if (grant < 0)
        {
            blocked = transport_blocked = TRUE;
            filter_id = "transport.not_subscribed";
        }
        else if (qos > grant)
        {
            target_qos = (uint8_t)grant;
            error = tb2_mqtt_limit_publish_qos(forwarded_packet, forwarded_packet_size,
                forwarded_packet_id_offset, target_qos, &qos_packet, &forwarded_packet_size);
            if (!error) forwarded_packet = qos_packet;
            else
            {
                blocked = transport_blocked = TRUE;
                filter_id = "transport.inflight_full";
                error = NO_ERROR;
            }
        }
    }
    tb2_mqtt_packet_id_entry_t *mapping = NULL;
    if (!error && !blocked && target_qos > 0 &&
        (!box_to_upstream || tb2_mqtt_cloud_available(session)))
    {
        uint16_t destination_id = 0;
        osAcquireMutex(&session->ids_mutex);
        error = box_to_upstream ?
            tb2_mqtt_cloud_id_allocate(session, type, target_qos, &destination_id) :
            tb2_mqtt_allocate_wire_packet_id(session, &destination_id);
        if (!error && !box_to_upstream)
            error = tb2_mqtt_packet_id_add(session, packet_id, destination_id, target_qos, FALSE);
        tb2_mqtt_packet_id_entry_t *list = box_to_upstream ? session->cloud_ids : session->packet_ids;
        if (!error) mapping = tb2_mqtt_id_find(list, destination_id);
        if (!error)
            error = tb2_mqtt_rewrite_packet_id(forwarded_packet, forwarded_packet_size,
                forwarded_packet_id_offset, destination_id, &wire_packet);
        if (!error && mapping != NULL)
            error = tb2_mqtt_id_store_packet(list, mapping, wire_packet, forwarded_packet_size);
        osReleaseMutex(&session->ids_mutex);
        if (error)
        {
            if (mapping != NULL)
            {
                osAcquireMutex(&session->ids_mutex);
                tb2_mqtt_id_remove(box_to_upstream ? &session->cloud_ids : &session->packet_ids, mapping);
                osReleaseMutex(&session->ids_mutex);
                mapping = NULL;
            }
            osFreeMem(wire_packet);
            wire_packet = NULL;
            /* A relay resource shortage must not close a healthy box leg. */
            tb2_mqtt_cloud_reset(session, "upstream_buffer_full");
            error = NO_ERROR;
            blocked = TRUE;
            filter_id = "transport.inflight_full";
            transport_blocked = TRUE;
        }
    }

    if (!error && transport_blocked)
    {
        error = tb2_mqtt_record_packet_ex(session, box_to_upstream, packet, packet_size,
            NULL, 0, type, topic, FALSE, filter_id, FALSE, TRUE, filter_id,
            packet_id, 0, FALSE, FALSE, 0, &manual_decision);
    }
    else if (!error && local_consume)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0, type,
            topic, FALSE, observer_result.filter_id, FALSE, TRUE,
            observer_result.capture_action != NULL ?
                observer_result.capture_action : "local_response_consume",
            packet_id, packet_id, FALSE, FALSE, 0, &manual_decision);
    }
    else if (!error && blocked && nocloud_result.action == MQTT_NOCLOUD_BLOCK)
    {
        bool_t local_content_block = filter_id != NULL &&
            osStrcmp(filter_id, "local_content.teddycloud_payload") == 0;
        const char *capture_action = local_content_block ?
            (observer_result.locally_processed ?
                 "local_status_local_content_block" : "local_content_block") :
            (observer_result.locally_processed ?
                 "local_status_nocloud_block" : "nocloud_block");
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0, type,
            topic, FALSE, filter_id, FALSE, TRUE, capture_action,
            packet_id, packet_id, TRUE, FALSE,
            nocloud_result.removed_count, &manual_decision);
    }
    else if (!error && !blocked && local_rewrite)
    {
        const uint8_t *actual_wire = wire_packet != NULL ?
                                         wire_packet : forwarded_packet;
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, actual_wire,
            forwarded_packet_size, type, topic, TRUE,
            filter_id, TRUE, TRUE,
            observer_result.capture_action != NULL ?
                observer_result.capture_action : "local_response_rewrite",
            packet_id, mapping != NULL ? mapping->wire_id : packet_id,
            FALSE, TRUE, nocloud_result.removed_count, &manual_decision);
    }
    else if (!error && !blocked &&
             nocloud_result.action == MQTT_NOCLOUD_REWRITE)
    {
        const uint8_t *actual_wire = wire_packet != NULL ?
                                         wire_packet : forwarded_packet;
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, actual_wire,
            forwarded_packet_size, type, topic, TRUE, filter_id, TRUE, TRUE,
            observer_result.locally_processed ?
                "local_status_nocloud_rewrite" : "nocloud_rewrite",
            packet_id,
            mapping != NULL ? mapping->wire_id : packet_id, FALSE, TRUE,
            nocloud_result.removed_count, &manual_decision);
    }
    else if (!error && !blocked && wire_packet != NULL)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, wire_packet,
            forwarded_packet_size, type, topic, TRUE, filter_id, FALSE, TRUE,
            "packet_id_remap", packet_id, mapping->wire_id, FALSE,
            FALSE, 0, &manual_decision);
    }
    else if (!error && !blocked && qos_packet != NULL)
    {
        error = tb2_mqtt_record_packet_ex(session, box_to_upstream, packet, packet_size,
            qos_packet, forwarded_packet_size, type, topic, TRUE, NULL, TRUE, TRUE,
            "subscription_qos_limit", packet_id, 0, FALSE, FALSE, 0, &manual_decision);
    }
    else if (!error && observer_result.locally_processed)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0, type,
            topic, !blocked, filter_id, FALSE, TRUE,
            blocked ? "local_status_manual_block" : "local_status_forward",
            packet_id, packet_id, blocked, FALSE, 0, &manual_decision);
    }
    else if (!error)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0,
            type, topic, !blocked, filter_id, FALSE, TRUE, NULL,
            packet_id, packet_id, blocked, FALSE, 0, &manual_decision);
    }
    osFreeMem(wire_packet);
    osFreeMem(qos_packet);
    osFreeMem(rebuilt_packet);
    osFreeMem(observer_result.payload);
    mqtt_nocloud_filter_result_free(&nocloud_result);
    if (!error && qos > 0)
        error = tb2_mqtt_send_generated_ack(session, box_to_upstream,
            qos == 1 ? TB2_MQTT_PACKET_PUBACK : TB2_MQTT_PACKET_PUBREC,
            packet_id, qos == 1 ? "local_puback" : "local_pubrec");
    osFreeMem(topic);
    return error;
}

static error_t tb2_mqtt_process_stream(tb2_mqtt_passthrough_session_t *session,
                                       bool_t box_to_upstream, const uint8_t *data,
                                       size_t length)
{
    tb2_mqtt_stream_t *stream = box_to_upstream ? &session->box_stream :
                                                  &session->upstream_stream;
    error_t error = tb2_mqtt_stream_append(stream, data, length);
    if (error)
        return error;

    while (stream->length > 0)
    {
        size_t packet_size = 0;
        size_t fixed_header_size = 0;
        error = tb2_mqtt_packet_size(stream->data, stream->length, &packet_size,
                                     &fixed_header_size);
        if (error == ERROR_WOULD_BLOCK || packet_size > stream->length)
            return NO_ERROR;
        if (error)
            return error;
        uint64_t epoch = session->epoch;
        error = tb2_mqtt_process_packet(session, box_to_upstream, stream->data,
                                        packet_size, fixed_header_size);
        if (error)
            return error;
        /* A bounded Cloud resource failure can reset this stream from inside
         * the packet handler. Never subtract from an already cleared buffer. */
        if (!box_to_upstream && session->epoch != epoch) return NO_ERROR;
        if (stream->length < packet_size) return NO_ERROR;
        stream->length -= packet_size;
        if (stream->length > 0)
        {
            osMemmove(stream->data, stream->data + packet_size, stream->length);
        }
    }
    return NO_ERROR;
}

static error_t tb2_mqtt_forward_ready(tb2_mqtt_passthrough_session_t *session,
                                      bool_t box_to_upstream)
{
    if (!box_to_upstream) return NO_ERROR; /* Cloud I/O belongs to its worker. */
    osAcquireMutex(&session->io_mutex);
    if (!tlsIsRxReady(session->box_tls) &&
        !(tcpWaitForEvents(session->box_socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY))
    { osReleaseMutex(&session->io_mutex); return NO_ERROR; }
    uint8_t buffer[TB2_MQTT_TUNNEL_BUFFER_SIZE];
    size_t received = 0;
    error_t error = tlsRead(session->box_tls, buffer, sizeof(buffer), &received, 0);
    osReleaseMutex(&session->io_mutex);
    /* A partial TLS read can return bytes together with a timeout. */
    if (received > 0)
    {
        error_t processed = tb2_mqtt_process_stream(session, TRUE, buffer, received);
        if (processed) return processed;
    }
    if (error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT) return NO_ERROR;
    if (error) return error;
    return received ? NO_ERROR : ERROR_END_OF_STREAM;
}

static void tb2_mqtt_cloud_clear(tb2_mqtt_passthrough_session_t *session, const char *reason)
{
    session->cloud_phase = TB2_MQTT_CLOUD_DOWN;
    if (osStrcmp(reason, "disabled")) tb2_mqtt_status_attempt_failed(reason);
    session->epoch = 0;
    session->upstream_stream.length = 0;
    session->synced_count = 0;
    session->subscriptions_dirty = TRUE;
    session->ping_pending = FALSE;
    osStrncpy(session->upstream_error, reason, sizeof(session->upstream_error) - 1);
    tb2_mqtt_packet_ids_clear(&session->cloud_ids);
    tb2_mqtt_qos2_free(&session->blocked_qos2_upstream);
    TRACE_INFO("TB2 MQTT cloud disconnected overlay=%u reason=%s; local connection retained\r\n",
        (unsigned)session->box_settings->internal.overlayNumber, reason);
}

static void tb2_mqtt_cloud_reset(tb2_mqtt_passthrough_session_t *session, const char *reason)
{
    tb2_mqtt_cloud_clear(session, reason);
    if (session->worker != NULL) tb2_mqtt_upstream_reconnect(session->worker, ERROR_FAILURE);
}
static void tb2_mqtt_cloud_shutdown(tb2_mqtt_passthrough_session_t *session,
                                   bool_t reconnect, const char *reason)
{
    const uint8_t packet[] = {0xe0, 0};
    tb2_mqtt_record_packet_ex(session, TRUE, packet, sizeof(packet), NULL, 0,
        14, NULL, FALSE, NULL, TRUE, TRUE, "upstream_disconnect_requested",
        0, 0, FALSE, FALSE, 0, NULL);
    tb2_mqtt_cloud_clear(session, reason);
    if (session->worker != NULL) tb2_mqtt_upstream_shutdown(session->worker, reconnect, NO_ERROR);
}
static void tb2_mqtt_capture_cloud_disconnect(tb2_mqtt_passthrough_session_t *session,
                                              error_t result)
{
    const uint8_t packet[] = {0xe0, 0};
    osAcquireMutex(&session->io_mutex);
    if (session->capture_opened && !session->capture_failed &&
        tb2_mqtt_capture_packet_ex(&session->capture, "box_to_upstream",
            packet, sizeof(packet), NULL, 0, 14, NULL, result == NO_ERROR,
            NULL, TRUE, TRUE, result ? "upstream_disconnect_failed" :
            "upstream_disconnect_complete", 0, 0, 0, NULL))
        session->capture_failed = TRUE;
    osReleaseMutex(&session->io_mutex);
}

static error_t tb2_mqtt_configure_worker(tb2_mqtt_passthrough_session_t *session)
{
    mutex_lock(MUTEX_SETTINGS);
    settings_t *identity = tb2_mqtt_select_identity_settings(session->box_settings);
    settings_t *global = get_settings();
    if (!tb2_mqtt_has_original_identity(identity))
    {
        mutex_unlock(MUTEX_SETTINGS);
        osStrcpy(session->upstream_error, "upstream_identity_unavailable");
        return ERROR_NOT_FOUND;
    }
    tb2_mqtt_upstream_config_t config = {
        .hostname = global->mqtt_client_upstream.hostname,
        .port = global->mqtt_client_upstream.port,
        .timeout_ms = global->core.http_client_timeout,
        .ca = identity->internal.client_tb2.ca,
        .certificate = identity->internal.client_tb2.crt,
        .private_key = identity->internal.client_tb2.key,
    };
    error_t error = session->worker == NULL ?
        tb2_mqtt_upstream_acquire(&config, session->owner, &session->worker) :
        tb2_mqtt_upstream_reconfigure(session->worker, &config, session->owner);
    mutex_unlock(MUTEX_SETTINGS);
    if (!error) session->upstream_configured = TRUE;
    else osStrcpy(session->upstream_error, "upstream_worker_unavailable");
    return error;
}

static void tb2_mqtt_complete_write(tb2_mqtt_passthrough_session_t *session,
                                    const tb2_mqtt_upstream_event_t *event)
{
    tb2_mqtt_pending_write_t *pending = NULL;
    for (size_t i = 0; i < TB2_MQTT_INFLIGHT_MAX; i++)
        if (session->writes[i].token == event->token) { pending = &session->writes[i]; break; }
    if (pending == NULL || event->token == 0) return;
    bool_t delivered = event->type == TB2_MQTT_UPSTREAM_TX_COMPLETE;
    osAcquireMutex(&session->io_mutex);
    if (session->capture_opened && !session->capture_failed)
    {
        error_t error = tb2_mqtt_capture_packet_ex(&session->capture, "box_to_upstream",
            pending->original, pending->original_length, pending->wire, pending->wire_length,
            pending->packet_type, pending->topic, delivered, pending->filter_id,
            pending->generated, TRUE, delivered ? "upstream_write_complete" : "upstream_write_failed",
            pending->original_id, pending->wire_id, pending->removed,
            pending->has_decision ? &pending->decision : NULL);
        if (error) session->capture_failed = TRUE;
    }
    if (delivered)
    {
        session->capture.bytes_box_to_upstream += pending->wire_length;
        session->capture.messages_forwarded_box_to_upstream++;
        tb2_mqtt_status_add_bytes(TRUE, pending->wire_length);
        tb2_mqtt_status_add_message(TRUE, FALSE);
        tb2_mqtt_add_nocloud_stats(session, TRUE, pending->rewritten, pending->removed);
        session->last_cloud_tx = osGetSystemTime();
    }
    osReleaseMutex(&session->io_mutex);
    if (delivered && pending->packet_type == TB2_MQTT_PACKET_PUBLISH)
        tb2_mqtt_notify_publish_completed(session, TRUE, pending->wire, pending->wire_length);
    session->writes_bytes -= pending->budget_bytes;
    tb2_mqtt_pending_write_free(pending);
}

error_t tb2_mqtt_passthrough_init(void)
{
    osMemset(&mqtt_passthrough_status, 0, sizeof(mqtt_passthrough_status));
    if (!osCreateMutex(&mqtt_passthrough_status.mutex)) return ERROR_OUT_OF_RESOURCES;
    mqtt_passthrough_status.initialized = TRUE;
    osStrcpy(mqtt_passthrough_status.state, "disabled");
    return NO_ERROR;
}
void tb2_mqtt_passthrough_deinit(void)
{
    for (size_t i = 0; i < TB2_MQTT_SESSION_MAX; i++) tb2_mqtt_saved_clear(&saved_sessions[i]);
    if (mqtt_passthrough_status.initialized)
    { osDeleteMutex(&mqtt_passthrough_status.mutex); mqtt_passthrough_status.initialized = FALSE; }
}
bool_t tb2_mqtt_passthrough_is_enabled(void)
{
    return get_settings()->mqtt_client_upstream.enabled;
}
static bool_t tb2_mqtt_cloud_available(const tb2_mqtt_passthrough_session_t *session)
{
    return session != NULL && session->cloud_phase == TB2_MQTT_CLOUD_READY &&
        session->epoch != 0 && !session->capture_failed && !session->privacy_failed &&
        tb2_mqtt_passthrough_is_enabled();
}

static void tb2_mqtt_runtime_update(tb2_mqtt_passthrough_session_t *session)
{
    osAcquireMutex(&session->io_mutex);
    bool_t connected = tb2_mqtt_cloud_available(session);
    if (connected && session->confirmed_epoch != session->epoch)
    {
        tb2_mqtt_upstream_confirm_session(session->worker, session->epoch);
        session->confirmed_epoch = session->epoch;
    }
    const char *state = !tb2_mqtt_passthrough_is_enabled() ? "disabled" :
        connected ? "connected" :
        session->upstream_error[0] ? "error" :
        session->ever_connected ? "reconnecting" : "connecting";
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    if (connected != session->runtime_cloud)
    {
        if (connected)
        {
            mqtt_passthrough_status.upstream_sessions++;
            mqtt_passthrough_status.last_success = time(NULL);
        }
        else if (mqtt_passthrough_status.upstream_sessions)
            mqtt_passthrough_status.upstream_sessions--;
    }
    session->runtime_cloud = connected;
    session->runtime_local = session->established;
    osStrcpy(session->runtime_state, state);
    osStrcpy(session->runtime_error, session->upstream_error);
    osReleaseMutex(&mqtt_passthrough_status.mutex);
    osReleaseMutex(&session->io_mutex);
}
bool_t tb2_mqtt_passthrough_is_established(const tb2_mqtt_passthrough_session_t *session)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    bool_t connected = session != NULL && session->runtime_local;
    osReleaseMutex(&mqtt_passthrough_status.mutex);
    return connected;
}
bool_t tb2_mqtt_passthrough_is_upstream_connected(const tb2_mqtt_passthrough_session_t *session)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    bool_t connected = session != NULL && session->runtime_cloud &&
        !session->capture_failed && !session->privacy_failed;
    osReleaseMutex(&mqtt_passthrough_status.mutex);
    return connected && tb2_mqtt_passthrough_is_enabled();
}
void tb2_mqtt_passthrough_add_runtime_status(const tb2_mqtt_passthrough_session_t *session,
                                            cJSON *mqtt)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    cJSON_AddBoolToObject(mqtt, "localConnected", session != NULL && session->runtime_local);
    cJSON_AddStringToObject(mqtt, "upstreamState", !tb2_mqtt_passthrough_is_enabled() ?
        "disabled" : session != NULL ? session->runtime_state : "connecting");
    cJSON_AddStringToObject(mqtt, "upstreamError", session != NULL ? session->runtime_error : "");
    osReleaseMutex(&mqtt_passthrough_status.mutex);
}

error_t tb2_mqtt_passthrough_start(TlsContext *box_tls, Socket *box_socket,
    size_t connection_slot, tb2_mqtt_passthrough_session_t **session, bool_t *handled,
    tb2_mqtt_publish_observer_t observer, tb2_mqtt_control_observer_t control_observer,
    tb2_mqtt_publish_completed_t publish_completed,
    tb2_mqtt_subscription_snapshot_t subscription_snapshot,
    tb2_mqtt_subscription_apply_t subscription_apply,
    tb2_mqtt_subscription_qos_t subscription_qos, void *observer_context,
    settings_t **box_settings_out)
{
    *session = NULL; *handled = FALSE;
    if (box_settings_out) *box_settings_out = NULL;
    settings_t *settings = tb2_mqtt_settings_from_certificate(box_tls);
    if (settings == NULL)
    {
        *handled = tb2_mqtt_passthrough_is_enabled();
        return *handled ? ERROR_FAILURE : NO_ERROR;
    }
    *handled = TRUE;
    if (connection_slot >= TB2_MQTT_SESSION_MAX) return ERROR_OUT_OF_RESOURCES;
    tb2_mqtt_passthrough_session_t *created = osAllocMem(sizeof(*created));
    if (created == NULL) return ERROR_OUT_OF_MEMORY;
    osMemset(created, 0, sizeof(*created));
    if (!osCreateMutex(&created->io_mutex))
    { osFreeMem(created); return ERROR_OUT_OF_RESOURCES; }
    if (!osCreateMutex(&created->ids_mutex))
    { osDeleteMutex(&created->io_mutex); osFreeMem(created); return ERROR_OUT_OF_RESOURCES; }
    created->box_tls = box_tls; created->box_socket = box_socket;
    created->box_settings = settings;
    created->observer = observer; created->control_observer = control_observer;
    created->publish_completed = publish_completed;
    created->subscription_snapshot = subscription_snapshot;
    created->subscription_apply = subscription_apply;
    created->subscription_qos = subscription_qos;
    created->observer_context = observer_context;
    created->next_local_packet_id = UINT16_MAX;
    created->owner = ++next_owner;
    created->saved_slot = -1;
    error_t error = tb2_mqtt_capture_open(&created->capture, get_settings());
    created->capture_opened = !error;
    created->capture_failed = error != NO_ERROR;
    if (error)
    {
        osStrcpy(created->upstream_error, "capture_open_failed");
        TRACE_WARNING("TB2 MQTT capture unavailable; Internet relay disabled, local connection retained\r\n");
    }
    tb2_mqtt_status_start();
    tb2_mqtt_runtime_update(created);
    socketSetTimeout(box_socket, TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
    *session = created;
    if (box_settings_out) *box_settings_out = settings;
    return NO_ERROR;
}
error_t tb2_mqtt_passthrough_forward_initial(tb2_mqtt_passthrough_session_t *session,
                                            const uint8_t *data, size_t length)
{
    if (session == NULL || data == NULL || length == 0) return ERROR_INVALID_PARAMETER;
    error_t error = tb2_mqtt_process_stream(session, TRUE, data, length);
    tb2_mqtt_runtime_update(session);
    return error;
}

error_t tb2_mqtt_passthrough_task(tb2_mqtt_passthrough_session_t *session)
{
    if (session == NULL) return ERROR_INVALID_PARAMETER;
    error_t error = tb2_mqtt_forward_ready(session, TRUE);
    if (error || !session->established) return error;
    uint32_t now = osGetSystemTime();
    if (session->keepalive && (uint32_t)(now - session->last_box_rx) >
        (uint32_t)session->keepalive * 1500U) return ERROR_TIMEOUT;

    bool_t enabled = tb2_mqtt_passthrough_is_enabled() && !session->capture_failed && !session->privacy_failed;
    if (!enabled && session->upstream_configured)
    {
        session->upstream_configured = FALSE;
        tb2_mqtt_cloud_shutdown(session, FALSE, session->capture_failed ? "capture_failed" : session->privacy_failed ? "local_response_history_full" : "disabled");
    }
    /* A failed credential lookup is retried at most once per second. */
    if (enabled && !session->upstream_configured &&
        (!session->configure_attempted || (uint32_t)(now - session->last_configure_at) >= TB2_MQTT_POLICY_CHECK_MS))
    {
        session->configure_attempted = TRUE;
        session->last_configure_at = now;
        tb2_mqtt_configure_worker(session);
    }
    tb2_mqtt_upstream_event_t event;
    while (session->worker != NULL && tb2_mqtt_upstream_poll(session->worker, &event))
    {
        if (event.owner_serial != session->owner)
        { tb2_mqtt_upstream_event_free(&event); continue; }
        if (event.type == TB2_MQTT_UPSTREAM_TX_COMPLETE || event.type == TB2_MQTT_UPSTREAM_TX_FAILED)
            tb2_mqtt_complete_write(session, &event);
        else if (event.type == TB2_MQTT_UPSTREAM_CONNECTED && enabled)
        {
            session->epoch = event.epoch;
            session->cloud_phase = TB2_MQTT_CLOUD_CONNACK;
            session->cloud_started_at = now;
            session->last_cloud_tx = now;
            session->upstream_error[0] = '\0';
            osFreeMem(session->cloud_connect); session->cloud_connect = NULL;
            error = tb2_mqtt_build_cloud_connect(session, &session->cloud_connect,
                &session->cloud_connect_length);
            if (!error) error = tb2_mqtt_record_packet_ex(session, TRUE,
                session->connect_packet, session->connect_length,
                session->cloud_connect, session->cloud_connect_length, 1, NULL,
                TRUE, NULL, TRUE, TRUE, "upstream_connect", 0, 0, FALSE, TRUE, 0, NULL);
            if (error) tb2_mqtt_cloud_reset(session, "connect_packet_failed");
        }
        else if (event.type == TB2_MQTT_UPSTREAM_RX && enabled && event.epoch == session->epoch)
        {
            error = tb2_mqtt_process_stream(session, FALSE, event.data, event.length);
            if (error) tb2_mqtt_cloud_reset(session, "upstream_protocol_error");
        }
        else if (event.type == TB2_MQTT_UPSTREAM_DISCONNECT)
            tb2_mqtt_capture_cloud_disconnect(session, event.error);
        else if (event.type == TB2_MQTT_UPSTREAM_DOWN)
            tb2_mqtt_cloud_clear(session, enabled ? "upstream_unavailable" :
                session->capture_failed ? "capture_failed" : "disabled");
        tb2_mqtt_upstream_event_free(&event);
    }
    if (session->cloud_phase == TB2_MQTT_CLOUD_READY && session->subscriptions_dirty)
        if (tb2_mqtt_sync_subscriptions(session))
            tb2_mqtt_cloud_reset(session, "subscription_sync_failed");
    if ((session->cloud_phase == TB2_MQTT_CLOUD_CONNACK ||
         session->cloud_phase == TB2_MQTT_CLOUD_SUBACK) &&
        (uint32_t)(now - session->cloud_started_at) >= TB2_MQTT_HANDSHAKE_TIMEOUT_MS)
        tb2_mqtt_cloud_reset(session, "upstream_handshake_timeout");

    uint32_t keepalive_ms = (uint32_t)session->keepalive * 1000U;
    if (session->cloud_phase == TB2_MQTT_CLOUD_READY && keepalive_ms)
    {
        if (session->ping_pending && (uint32_t)(now - session->ping_sent_at) >= keepalive_ms)
            tb2_mqtt_cloud_reset(session, "upstream_keepalive_timeout");
        else if (!session->ping_pending &&
                 (uint32_t)(now - session->last_cloud_tx) >= keepalive_ms / 2U)
        {
            tb2_mqtt_cloud_packet(session, 0xc0, NULL, 0, "upstream_pingreq");
            session->ping_pending = TRUE; session->ping_sent_at = now;
        }
    }
    /* Re-evaluate registered Will protection; a changed filter must not leave
     * an old Will installed when a graceful withdrawal is still possible. */
    if (enabled && session->epoch && (uint32_t)(now - session->last_will_check) >= TB2_MQTT_POLICY_CHECK_MS)
    {
        uint8_t *updated = NULL; size_t size = 0;
        session->last_will_check = now;
        if (!tb2_mqtt_build_cloud_connect(session, &updated, &size) &&
            (size != session->cloud_connect_length ||
             osMemcmp(updated, session->cloud_connect, size)))
            tb2_mqtt_cloud_shutdown(session, TRUE, "will_policy_changed");
        osFreeMem(updated);
    }
    tb2_mqtt_runtime_update(session);
    return session->box_write_failed ? ERROR_WRITE_FAILED : NO_ERROR;
}

error_t tb2_mqtt_passthrough_reserve_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t *packet_id)
{
    if (session == NULL || packet_id == NULL)
        return ERROR_INVALID_PARAMETER;

    osAcquireMutex(&session->ids_mutex);
    uint16_t reserved = 0;
    error_t error = tb2_mqtt_allocate_wire_packet_id(session, &reserved);
    if (!error)
        error = tb2_mqtt_packet_id_add(session, reserved, reserved, 1, TRUE);
    if (!error)
        *packet_id = reserved;
    osReleaseMutex(&session->ids_mutex);
    return error;
}

void tb2_mqtt_passthrough_release_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id)
{
    if (session == NULL || packet_id == 0)
        return;
    osAcquireMutex(&session->ids_mutex);
    tb2_mqtt_packet_id_entry_t *entry =
        tb2_mqtt_packet_id_find_wire(session, packet_id);
    if (entry != NULL && entry->local)
        tb2_mqtt_packet_id_remove(session, entry);
    osReleaseMutex(&session->ids_mutex);
}

error_t tb2_mqtt_passthrough_write_local_publish(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, const char *topic, uint16_t packet_id,
    const char *capture_action)
{
    if (session == NULL || packet == NULL || packet_size == 0 ||
        topic == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    uint8_t qos = (packet[0] >> 1) & 0x03U;
    if (qos > 1)
        return ERROR_INVALID_TYPE;
    if (qos == 1)
    {
        osAcquireMutex(&session->ids_mutex);
        tb2_mqtt_packet_id_entry_t *entry =
            tb2_mqtt_packet_id_find_wire(session, packet_id);
        bool_t valid = packet_id != 0 && entry != NULL && entry->local;
        osReleaseMutex(&session->ids_mutex);
        if (!valid) return ERROR_INVALID_PARAMETER;
    }

    const mqtt_forward_filter_result_t manual_decision = mqtt_forward_filter_evaluate(
        session->box_settings, MQTT_FORWARD_ROUTE_LOCAL_TO_BOX, topic, NULL, 0);
    TRACE_DEBUG("TB2 MQTT proxy generated=PUBLISH direction=upstream_to_box"
                " route=local_to_box manual_decision=local"
                " topic='%s' qos=%u packet_id=%u action=%s bytes=%" PRIuSIZE "\r\n",
                topic, (unsigned)qos, (unsigned)packet_id,
                capture_action != NULL ? capture_action : "local_publish",
                packet_size);
    return tb2_mqtt_record_packet_ex(
        session, FALSE, packet, packet_size, NULL, 0,
        TB2_MQTT_PACKET_PUBLISH, topic, TRUE, NULL, TRUE, TRUE,
        capture_action != NULL ? capture_action : "local_publish",
        packet_id, packet_id, FALSE, FALSE, 0, &manual_decision);
}

void tb2_mqtt_passthrough_close(tb2_mqtt_passthrough_session_t *session,
                                const char *result_code, bool_t success)
{
    if (session == NULL)
    {
        return;
    }
    if (session->worker != NULL)
    {
        if (session->clean_disconnect)
            tb2_mqtt_cloud_shutdown(session, FALSE, "local_disconnect");
        tb2_mqtt_upstream_release(session->worker);
    }
    if (session->saved_slot >= 0)
    {
        tb2_mqtt_saved_session_t *saved = &saved_sessions[session->saved_slot];
        if (saved->owner == session->owner)
        {
            saved->active = NULL;
            if (!session->clean_session && session->established)
            {
                saved->incoming = session->blocked_qos2_box;
                session->blocked_qos2_box = NULL;
                /* Freshness remains governed by its existing overlay retry cache. */
                tb2_mqtt_packet_id_entry_t *p = session->packet_ids;
                while (p != NULL)
                {
                    tb2_mqtt_packet_id_entry_t *next = p->next;
                    if (p->local) tb2_mqtt_packet_id_remove(session, p);
                    p = next;
                }
                saved->outgoing = session->packet_ids;
                session->packet_ids = NULL;
                saved->next_id = session->next_local_packet_id;
                saved->response_history = session->response_history;
                mqtt_response_history_init(&session->response_history);
            }
            else tb2_mqtt_saved_clear(saved);
        }
    }

    if (session->capture_opened)
    {
        if (session->box_stream.length > 0)
        {
            tb2_mqtt_capture_packet(&session->capture, "box_to_upstream",
                                    session->box_stream.data, session->box_stream.length,
                                    session->box_stream.data[0] >> 4, NULL, FALSE,
                                    "incomplete_packet", FALSE, FALSE);
        }
        if (session->upstream_stream.length > 0)
        {
            tb2_mqtt_capture_packet(&session->capture, "upstream_to_box",
                                    session->upstream_stream.data,
                                    session->upstream_stream.length,
                                    session->upstream_stream.data[0] >> 4, NULL, FALSE,
                                    "incomplete_packet", FALSE, FALSE);
        }
        error_t error = tb2_mqtt_capture_finish(&session->capture, get_settings(), result_code);
        if (error)
        {
            success = FALSE;
            result_code = "capture_finalize_failed";
        }
        tb2_mqtt_rotate_completed_captures(get_settings());
        TRACE_INFO("TB2 MQTT passthrough session=%s status=%s up=%llu down=%llu\r\n",
                   session->capture.session_id, result_code,
                   (unsigned long long)session->capture.bytes_box_to_upstream,
                   (unsigned long long)session->capture.bytes_upstream_to_box);
    }
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    if (session->runtime_cloud && mqtt_passthrough_status.upstream_sessions)
        mqtt_passthrough_status.upstream_sessions--;
    osReleaseMutex(&mqtt_passthrough_status.mutex);
    tb2_mqtt_status_finish(success, result_code);
    osFreeMem(session->box_stream.data);
    osFreeMem(session->upstream_stream.data);
    tb2_mqtt_qos2_free(&session->blocked_qos2_box);
    tb2_mqtt_qos2_free(&session->blocked_qos2_upstream);
    tb2_mqtt_packet_ids_clear(&session->packet_ids);
    tb2_mqtt_packet_ids_clear(&session->cloud_ids);
    tb2_mqtt_pending_writes_clear(session);
    mqtt_response_history_reset(&session->response_history);
    osFreeMem(session->connect_packet);
    osFreeMem(session->cloud_connect);
    osDeleteMutex(&session->ids_mutex);
    osDeleteMutex(&session->io_mutex);
    osFreeMem(session);
}

error_t tb2_mqtt_passthrough_write_status(HttpConnection *connection)
{
    settings_t *settings = get_settings();
    cJSON *json = cJSON_CreateObject();
    if (json == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    osAcquireMutex(&mqtt_passthrough_status.mutex);
    const char *state;
    if (!settings->mqtt_client_upstream.enabled)
    {
        state = "disabled";
    }
    else if (mqtt_passthrough_status.upstream_sessions > 0)
    {
        state = "connected";
    }
    else if (mqtt_passthrough_status.active_sessions == 0 &&
             mqtt_passthrough_status.error_code[0] == '\0')
    {
        state = "ready";
    }
    else
    {
        state = mqtt_passthrough_status.error_code[0] ? "error" : "connecting";
    }

    cJSON_AddBoolToObject(json, "enabled", settings->mqtt_client_upstream.enabled);
    /* Deprecated compatibility alias for older WebUI clients. */
    cJSON_AddBoolToObject(json, "passthrough_enabled",
                         settings->mqtt_client_upstream.enabled);
    cJSON_AddStringToObject(json, "state", state);
    cJSON_AddStringToObject(json, "hostname", settings->mqtt_client_upstream.hostname);
    cJSON_AddNumberToObject(json, "port", settings->mqtt_client_upstream.port);
    cJSON_AddNumberToObject(json, "bytes_box_to_upstream",
                           (double)mqtt_passthrough_status.bytes_box_to_upstream);
    cJSON_AddNumberToObject(json, "bytes_upstream_to_box",
                           (double)mqtt_passthrough_status.bytes_upstream_to_box);
    cJSON_AddNumberToObject(json, "messages_forwarded_box_to_upstream",
                           (double)mqtt_passthrough_status.messages_forwarded_box_to_upstream);
    cJSON_AddNumberToObject(json, "messages_forwarded_upstream_to_box",
                           (double)mqtt_passthrough_status.messages_forwarded_upstream_to_box);
    cJSON_AddNumberToObject(json, "messages_blocked_box_to_upstream",
                           (double)mqtt_passthrough_status.messages_blocked_box_to_upstream);
    cJSON_AddNumberToObject(json, "messages_blocked_upstream_to_box",
                           (double)mqtt_passthrough_status.messages_blocked_upstream_to_box);
    cJSON_AddNumberToObject(json, "messages_rewritten_box_to_upstream",
                           (double)mqtt_passthrough_status.messages_rewritten_box_to_upstream);
    cJSON_AddNumberToObject(json, "messages_rewritten_upstream_to_box",
                           (double)mqtt_passthrough_status.messages_rewritten_upstream_to_box);
    cJSON_AddNumberToObject(
        json, "nocloud_items_removed_box_to_upstream",
        (double)mqtt_passthrough_status.nocloud_items_removed_box_to_upstream);
    cJSON_AddNumberToObject(
        json, "nocloud_items_removed_upstream_to_box",
        (double)mqtt_passthrough_status.nocloud_items_removed_upstream_to_box);
    cJSON_AddNumberToObject(json, "upstream_sessions", mqtt_passthrough_status.upstream_sessions);
    cJSON_AddNumberToObject(json, "last_attempt", (double)mqtt_passthrough_status.last_attempt);
    cJSON_AddNumberToObject(json, "last_success", (double)mqtt_passthrough_status.last_success);
    cJSON_AddStringToObject(json, "error_code", mqtt_passthrough_status.error_code);
    osReleaseMutex(&mqtt_passthrough_status.mutex);

    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    httpPrepareHeader(connection, "application/json; charset=utf-8", osStrlen(body));
    return httpWriteResponse(connection, body, connection->response.contentLength, true);
}
