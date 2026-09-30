#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

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
#include "mqtt_time.h"
#include "os_ext.h"
#include "os_port.h"
#include "platform.h"
#include "rand.h"
#include "server_helpers.h"
#include "settings.h"
#include "tb2_mqtt_passthrough.h"
#include "tls.h"
#include "tls_adapter.h"

#define TB2_MQTT_TUNNEL_BUFFER_SIZE 16384
#define TB2_MQTT_TUNNEL_IO_TIMEOUT_MS 500
#define TB2_MQTT_BOX_IO_OPS_PER_TICK 4U
#define TB2_MQTT_BOX_IO_BYTES_PER_TICK 16384U
#define TB2_MQTT_BOX_WRITE_STALL_MS 120000U
#define TB2_MQTT_INFLIGHT_MAX 32U
#define TB2_MQTT_BUFFER_LIMIT (1024U * 1024U)
#define TB2_MQTT_PROTOCOL_RESERVE_BYTES (2U * TB2_MQTT_TUNNEL_BUFFER_SIZE)
#define TB2_MQTT_CLOUD_PACKET_BUDGET ((TB2_MQTT_BUFFER_LIMIT - TB2_MQTT_PROTOCOL_RESERVE_BYTES) / 3U)
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
    struct tb2_mqtt_packet_id_entry *next;
} tb2_mqtt_packet_id_entry_t;

typedef enum
{
    TB2_MQTT_LOCAL_RESPONSE_CONSUME = 0,
    TB2_MQTT_LOCAL_RESPONSE_BLOCK,
    TB2_MQTT_LOCAL_RESPONSE_REWRITE
} tb2_mqtt_local_response_action_t;

typedef struct tb2_mqtt_local_response_entry
{
    uint16_t packet_id;
    uint8_t qos;
    tb2_mqtt_local_response_action_t action;
    uint8_t *payload;
    size_t payload_len;
    const char *filter_id;
    bool_t completed;
    struct tb2_mqtt_local_response_entry *next;
} tb2_mqtt_local_response_entry_t;

typedef struct
{
    bool_t initialized;
    OsMutex mutex;
    uint32_t session_counter;
    uint32_t active_sessions;
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

typedef struct {
    uint8_t *original;
    size_t original_length;
    uint8_t *wire;
    size_t wire_length;
    size_t budget_bytes;
    char *topic;
    const char *action;
    const char *filter_id;
    uint16_t original_id;
    uint16_t wire_id;
    uint8_t packet_type;
    bool_t generated;
    bool_t rewritten;
    size_t removed;
} tb2_mqtt_pending_write_t;

typedef struct tb2_mqtt_box_write {
    tb2_mqtt_pending_write_t packet;
    size_t offset;
    char *owned_action;
    char *owned_filter;
    tb2_mqtt_local_write_completed_t completed;
    void *context;
    struct tb2_mqtt_box_write *next;
} tb2_mqtt_box_write_t;

struct tb2_mqtt_passthrough_session
{
    TlsContext *box_tls;
    Socket *box_socket;
    HttpClientContext upstream;
    bool_t upstream_initialized;
    settings_t *box_settings;
    tb2_mqtt_publish_observer_t observer;
    tb2_mqtt_control_observer_t control_observer;
    void *observer_context;
    tb2_mqtt_stream_t box_stream;
    tb2_mqtt_stream_t upstream_stream;
    tb2_mqtt_qos2_entry_t *blocked_qos2_box;
    tb2_mqtt_qos2_entry_t *blocked_qos2_upstream;
    tb2_mqtt_packet_id_entry_t *packet_ids;
    tb2_mqtt_local_response_entry_t *local_responses;
    size_t local_response_count;
    uint16_t next_local_packet_id;
    tb2_mqtt_capture_t capture;
    bool_t capture_opened;
    OsMutex io_mutex;
    atomic_int box_write_error;
    atomic_int local_publish_error;
    tb2_mqtt_box_write_t *box_write_head;
    tb2_mqtt_box_write_t *box_write_tail;
    size_t box_write_count;
    size_t box_write_bytes;
    size_t box_io_bytes;
    unsigned box_io_ops;
    uint32_t box_last_progress;
    bool_t box_progress_clock;
    bool_t box_write_stalled;
    bool_t box_write_active;
    bool_t box_control_active;
    bool_t processing_packet;
    bool_t connack_sent;
    uint16_t keepalive;
    uint32_t last_box_rx;
    uint64_t owner;
    TlsSocketSendCallback previous_send;
    TlsSocketReceiveCallback previous_receive;
    TlsSocketHandle previous_handle;
};

static tb2_mqtt_passthrough_status_t mqtt_passthrough_status;
static uint64_t next_owner;
#ifdef _MSC_VER
static __declspec(thread) tb2_mqtt_passthrough_session_t *parser_session;
#else
static _Thread_local tb2_mqtt_passthrough_session_t *parser_session;
#endif

static void tb2_mqtt_trace_error(const char *stage, error_t error)
{
    TRACE_ERROR("TB2 MQTT upstream stage=%s failed error=%s code=%d\r\n",
                stage, error2text(error), (int)error);
}

static void tb2_mqtt_trace_upstream_client_auth(const HttpClientContext *upstream)
{
    const TlsContext *tls_context = upstream != NULL ? upstream->tlsContext : NULL;
    if (tls_context == NULL)
    {
        TRACE_DEBUG("TB2 MQTT upstream stage=client_auth certificate_request=unknown"
                    " response=unknown reason=tls_context_missing\r\n");
        return;
    }

    const bool_t requested = tls_context->clientCertRequested;
    const bool_t certificate_sent = requested && tls_context->cert != NULL;
    const char *response = !requested ? "not_sent" :
                           certificate_sent ? "certificate_sent" : "empty_certificate";
    TRACE_DEBUG("TB2 MQTT upstream stage=client_auth certificate_request=%s"
                " response=%s resumed=%s\r\n",
                requested ? "received" : "not_received",
                response,
                tls_context->resume ? "true" : "false");
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

static error_t tb2_mqtt_outbound_tls_init(HttpClientContext *context, TlsContext *tls_context)
{
    settings_t *settings = (settings_t *)context->sourceCtx;
    TRACE_DEBUG("TB2 MQTT upstream stage=tls_init begin server=%s identity=%s\r\n",
                context->serverName,
                tb2_mqtt_has_original_identity(settings) ? "available" : "unavailable");
    if (!tb2_mqtt_has_original_identity(settings))
    {
        tb2_mqtt_trace_error("tls_identity", ERROR_FAILURE);
        return ERROR_FAILURE;
    }

    error_t error = tlsSetPrng(tls_context, rand_get_algo(), rand_get_context());
    if (error)
    {
        tb2_mqtt_trace_error("tls_prng", error);
    }
    if (!error)
    {
        error = tlsSetTrustedCaList(tls_context, settings->internal.client_tb2.ca,
                                    osStrlen(settings->internal.client_tb2.ca));
        if (error)
        {
            tb2_mqtt_trace_error("tls_ca", error);
        }
    }
    if (!error)
    {
        error = tlsAddCertificate(tls_context, settings->internal.client_tb2.crt,
                                  osStrlen(settings->internal.client_tb2.crt),
                                  settings->internal.client_tb2.key,
                                  osStrlen(settings->internal.client_tb2.key));
        if (error)
        {
            tb2_mqtt_trace_error("tls_client_certificate", error);
        }
    }
    if (!error)
    {
        error = tlsSetServerName(tls_context, context->serverName);
        if (error)
        {
            tb2_mqtt_trace_error("tls_server_name", error);
        }
    }
    if (!error)
    {
        tls_context_key_log_init(tls_context);
        TRACE_DEBUG("TB2 MQTT upstream stage=tls_init ready server=%s\r\n",
                    context->serverName);
    }
    return error;
}

static error_t tb2_mqtt_connect_upstream(settings_t *box_settings, HttpClientContext *upstream)
{
    settings_t *global = get_settings();
    upstream->serverName = global->mqtt_client_upstream.hostname;
    upstream->sourceCtx = box_settings;

    TRACE_DEBUG("TB2 MQTT upstream stage=connect begin target=%s:%u timeout_ms=%u\r\n",
                global->mqtt_client_upstream.hostname,
                (unsigned)global->mqtt_client_upstream.port,
                (unsigned)global->core.http_client_timeout);

    error_t error = httpClientSetTimeout(upstream, global->core.http_client_timeout);
    if (error)
    {
        tb2_mqtt_trace_error("configure_timeout", error);
    }
    if (!error)
    {
        error = httpClientRegisterTlsInitCallback(upstream, tb2_mqtt_outbound_tls_init);
        if (error)
        {
            tb2_mqtt_trace_error("register_tls_callback", error);
        }
    }
    if (error)
    {
        return error;
    }

    void *resolver = resolve_host(global->mqtt_client_upstream.hostname);
    if (resolver == NULL)
    {
        tb2_mqtt_trace_error("dns_resolve", ERROR_ADDRESS_NOT_FOUND);
        return ERROR_ADDRESS_NOT_FOUND;
    }
    TRACE_DEBUG("TB2 MQTT upstream stage=dns_resolve success host=%s\r\n",
                global->mqtt_client_upstream.hostname);

    error = ERROR_ADDRESS_NOT_FOUND;
    for (int position = 0;; position++)
    {
        IpAddr address;
        if (!resolve_get_ip(resolver, position, &address))
        {
            break;
        }
        TRACE_DEBUG("TB2 MQTT upstream stage=tcp_connect attempt=%d address=%s port=%u\r\n",
                    position + 1, ipAddrToString(&address, NULL),
                    (unsigned)global->mqtt_client_upstream.port);
        error = httpClientConnect(upstream, &address,
                                  (uint16_t)global->mqtt_client_upstream.port);
        if (!error)
        {
            tb2_mqtt_trace_upstream_client_auth(upstream);
            TRACE_DEBUG("TB2 MQTT upstream stage=tcp_tls_connect success attempt=%d address=%s\r\n",
                        position + 1, ipAddrToString(&address, NULL));
            break;
        }
        tb2_mqtt_trace_error("tcp_tls_connect", error);
    }
    resolve_free(resolver);

    if (!error && upstream->socket != NULL)
    {
        error_t timeout_error = socketSetTimeout(upstream->socket, TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
        if (timeout_error)
        {
            tb2_mqtt_trace_error("configure_tunnel_timeout", timeout_error);
            return timeout_error;
        }
        TRACE_DEBUG("TB2 MQTT upstream stage=connect ready target=%s:%u io_timeout_ms=%u\r\n",
                    global->mqtt_client_upstream.hostname,
                    (unsigned)global->mqtt_client_upstream.port,
                    (unsigned)TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
    }
    else if (error)
    {
        tb2_mqtt_trace_error("connect_exhausted", error);
    }
    return error;
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
                                           size_t removed_count)
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
                                      generated, packet_complete, NULL, 0, 0, 0);
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

static void tb2_mqtt_status_connected(void)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    osStrcpy(mqtt_passthrough_status.state, "connected");
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

error_t tb2_mqtt_passthrough_box_write_error(const tb2_mqtt_passthrough_session_t *session)
{
    return session != NULL ? atomic_load(&session->box_write_error) : NO_ERROR;
}

/* Session-local nonblocking adapters share a bounded per-mainloop I/O budget.
 * Exhausting that budget is not a transport stall. Only actual backpressure
 * rejects additional interactive commands until the queued write recovers. */
static error_t tb2_mqtt_box_send(TlsSocketHandle handle, const void *data,
    size_t length, size_t *written, uint_t flags)
{
    tb2_mqtt_passthrough_session_t *session = handle;
    (void)flags;
    *written = 0;
    if (!session->box_io_ops || !session->box_io_bytes) return ERROR_WOULD_BLOCK;
    length = MIN(length, session->box_io_bytes);
    session->box_io_ops--;
#ifdef _WIN32
    int n = send(session->box_socket->descriptor, data, (int)length, 0);
    int code = n == SOCKET_ERROR ? WSAGetLastError() : 0;
    bool_t blocked = code == WSAEWOULDBLOCK;
    bool_t interrupted = code == WSAEINTR;
#else
    ssize_t n = send(session->box_socket->descriptor, data, length, MSG_DONTWAIT | MSG_NOSIGNAL);
    int code = n < 0 ? errno : 0;
    bool_t blocked = code == EAGAIN || code == EWOULDBLOCK;
    bool_t interrupted = code == EINTR;
#endif
    if (n > 0)
    {
        *written = (size_t)n;
        session->box_io_bytes -= (size_t)n;
        session->box_progress_clock = mqtt_monotonic_ms(&session->box_last_progress);
        return session->box_progress_clock ? NO_ERROR : ERROR_FAILURE;
    }
    if (blocked && !session->box_write_stalled)
    {
        session->box_write_stalled = TRUE;
        TRACE_INFO("TB2 MQTT box_write_pending box=%s session=%" PRIu64
            " tls_state=%u reason=socket_backpressure\r\n", session->box_settings->commonName,
            session->owner, (unsigned)tlsGetState(session->box_tls));
    }
    return blocked || interrupted ? ERROR_WOULD_BLOCK : ERROR_WRITE_FAILED;
}

static error_t tb2_mqtt_box_receive(TlsSocketHandle handle, void *data,
    size_t length, size_t *received, uint_t flags)
{
    tb2_mqtt_passthrough_session_t *session = handle;
    (void)flags;
    *received = 0;
    if (!session->box_io_ops || !session->box_io_bytes) return ERROR_WOULD_BLOCK;
    length = MIN(length, session->box_io_bytes);
    session->box_io_ops--;
#ifdef _WIN32
    int n = recv(session->box_socket->descriptor, data, (int)length, 0);
    int code = n == SOCKET_ERROR ? WSAGetLastError() : 0;
    bool_t retry = code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    ssize_t n = recv(session->box_socket->descriptor, data, length, MSG_DONTWAIT);
    int code = n < 0 ? errno : 0;
    bool_t retry = code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
    if (n > 0)
    {
        *received = (size_t)n;
        session->box_io_bytes -= (size_t)n;
        return NO_ERROR;
    }
    return n == 0 ? ERROR_END_OF_STREAM : retry ? ERROR_WOULD_BLOCK : ERROR_READ_FAILED;
}

static error_t tb2_mqtt_box_terminal(tb2_mqtt_passthrough_session_t *session,
    error_t error, const char *reason)
{
    int expected = NO_ERROR;
    if (atomic_compare_exchange_strong(&session->box_write_error, &expected, error))
        TRACE_WARNING("TB2 MQTT %s box=%s session=%" PRIu64 " tls_state=%u code=%d decision=close\r\n",
            reason, session->box_settings->commonName, session->owner,
            (unsigned)tlsGetState(session->box_tls), error);
    return tb2_mqtt_passthrough_box_write_error(session);
}

static error_t tb2_mqtt_box_control_step(tb2_mqtt_passthrough_session_t *session)
{
    uint32_t now;
    if (!mqtt_monotonic_ms(&now))
        return tb2_mqtt_box_terminal(session, ERROR_FAILURE, "box_write_clock_failed");
    if (!session->box_control_active)
    {
        session->box_control_active = TRUE;
        session->box_last_progress = now;
        session->box_progress_clock = TRUE;
    }
    if (!session->box_progress_clock ||
        (uint32_t)(now - session->box_last_progress) >= TB2_MQTT_BOX_WRITE_STALL_MS)
        return tb2_mqtt_box_terminal(session, ERROR_TIMEOUT, "box_write_stalled");
    error_t error = tlsConnect(session->box_tls);
    if ((error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT) &&
        tlsGetState(session->box_tls) < TLS_STATE_CLOSING) return ERROR_WOULD_BLOCK;
    if (error) return tb2_mqtt_box_terminal(session, error, "box_control_failed");
    session->box_control_active = FALSE;
    session->box_write_stalled = FALSE;
    return NO_ERROR;
}

/* Cyclone retains partial ciphertext. Resume only the uncredited plaintext
 * suffix on this same context; do not interleave reads or other TLS messages. */
static error_t tb2_mqtt_tls_write_step(tb2_mqtt_passthrough_session_t *session,
    const uint8_t *data, size_t length, size_t *offset)
{
    error_t error = tb2_mqtt_passthrough_box_write_error(session);
    if (error) return error;
    uint32_t now;
    if (!mqtt_monotonic_ms(&now)) error = ERROR_FAILURE;
    else if (!session->box_write_active)
    {
        session->box_write_active = TRUE;
        session->box_last_progress = now;
        session->box_progress_clock = TRUE;
    }
    if (!error && (!session->box_progress_clock ||
        (uint32_t)(now - session->box_last_progress) >= TB2_MQTT_BOX_WRITE_STALL_MS))
        error = ERROR_TIMEOUT;
    size_t written = 0;
    if (!error)
    {
        error = tlsWrite(session->box_tls, data + *offset, length - *offset, &written, 0);
        if (written > length - *offset || (!error && !written)) error = ERROR_WRITE_FAILED;
        else *offset += written;
    }
    if ((error == ERROR_TIMEOUT || error == ERROR_WOULD_BLOCK) &&
        session->box_progress_clock && mqtt_monotonic_ms(&now) &&
        (uint32_t)(now - session->box_last_progress) < TB2_MQTT_BOX_WRITE_STALL_MS &&
        tlsGetState(session->box_tls) == TLS_STATE_APPLICATION_DATA && *offset < length)
        return ERROR_WOULD_BLOCK;
    if (error)
    {
        return tb2_mqtt_box_terminal(session, error,
            error == ERROR_TIMEOUT && session->box_progress_clock &&
            (uint32_t)(now - session->box_last_progress) >= TB2_MQTT_BOX_WRITE_STALL_MS ?
            "box_write_stalled" : "box_write_failed");
    }
    else
    {
        session->box_write_active = FALSE;
        if (session->box_write_stalled)
            TRACE_INFO("TB2 MQTT box_write_recovered box=%s session=%" PRIu64
                " written=%" PRIuSIZE "\r\n", session->box_settings->commonName, session->owner, *offset);
        session->box_write_stalled = FALSE;
    }
    return error;
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

static void tb2_mqtt_box_write_free(tb2_mqtt_box_write_t *write)
{
    if (write == NULL) return;
    if (write->packet.wire != write->packet.original) osFreeMem(write->packet.wire);
    osFreeMem(write->packet.original);
    osFreeMem(write->packet.topic);
    osFreeMem(write->owned_action);
    osFreeMem(write->owned_filter);
    osFreeMem(write);
}

/* Own caller bytes and metadata; the FIFO budget includes all allocations. */
static error_t tb2_mqtt_box_write_create(const uint8_t *data, size_t length,
    const uint8_t *wire, size_t wire_length, uint8_t type, const char *topic,
    const char *filter, bool_t generated, const char *action,
    uint16_t packet_id, uint16_t wire_id, bool_t rewritten, size_t removed,
    tb2_mqtt_box_write_t **result)
{
    *result = NULL;
    size_t topic_bytes = topic != NULL ? osStrlen(topic) + 1 : 0;
    size_t action_bytes = action != NULL ? osStrlen(action) + 1 : 0;
    size_t filter_bytes = filter != NULL ? osStrlen(filter) + 1 : 0;
    bool_t separate = wire != NULL && wire != data;
    if (data == NULL || !length || length > TB2_MQTT_BUFFER_LIMIT ||
        wire_length > TB2_MQTT_BUFFER_LIMIT) return ERROR_MESSAGE_TOO_LONG;
    size_t budget = sizeof(tb2_mqtt_box_write_t) + length +
        (separate ? wire_length : 0) + topic_bytes + action_bytes + filter_bytes;
    if (budget > TB2_MQTT_BUFFER_LIMIT) return ERROR_MESSAGE_TOO_LONG;
    tb2_mqtt_box_write_t *write = osAllocMem(sizeof(*write));
    if (write == NULL) return ERROR_OUT_OF_MEMORY;
    osMemset(write, 0, sizeof(*write));
    tb2_mqtt_pending_write_t *p = &write->packet;
    p->original = osAllocMem(length);
    p->wire = separate ? osAllocMem(wire_length) : p->original;
    p->topic = topic_bytes ? osAllocMem(topic_bytes) : NULL;
    write->owned_action = action_bytes ? osAllocMem(action_bytes) : NULL;
    write->owned_filter = filter_bytes ? osAllocMem(filter_bytes) : NULL;
    if (p->original == NULL || p->wire == NULL || (topic_bytes && p->topic == NULL) ||
        (action_bytes && write->owned_action == NULL) || (filter_bytes && write->owned_filter == NULL))
    { tb2_mqtt_box_write_free(write); return ERROR_OUT_OF_MEMORY; }
    osMemcpy(p->original, data, length);
    if (separate) osMemcpy(p->wire, wire, wire_length);
    if (topic_bytes) osMemcpy(p->topic, topic, topic_bytes);
    if (action_bytes) osMemcpy(write->owned_action, action, action_bytes);
    if (filter_bytes) osMemcpy(write->owned_filter, filter, filter_bytes);
    p->original_length = length;
    p->wire_length = separate ? wire_length : length;
    p->budget_bytes = budget;
    p->action = write->owned_action;
    p->filter_id = write->owned_filter;
    p->original_id = packet_id;
    p->wire_id = wire_id;
    p->packet_type = type;
    p->generated = generated;
    p->rewritten = rewritten;
    p->removed = removed;
    *result = write;
    return NO_ERROR;
}

static error_t tb2_mqtt_box_write_append(tb2_mqtt_passthrough_session_t *session,
    tb2_mqtt_box_write_t *write)
{
    error_t error = tb2_mqtt_passthrough_box_write_error(session);
    if (error) return error;
    if (session->box_write_count >= TB2_MQTT_INFLIGHT_MAX ||
        session->box_write_bytes > TB2_MQTT_BUFFER_LIMIT - write->packet.budget_bytes)
        return ERROR_WOULD_BLOCK;
    if (session->box_write_tail != NULL) session->box_write_tail->next = write;
    else session->box_write_head = write;
    session->box_write_tail = write;
    session->box_write_count++;
    session->box_write_bytes += write->packet.budget_bytes;
    return NO_ERROR;
}

static void tb2_mqtt_box_writes_cancel(tb2_mqtt_passthrough_session_t *session, error_t error)
{
    osAcquireMutex(&session->io_mutex);
    tb2_mqtt_box_write_t *write = session->box_write_head;
    session->box_write_head = session->box_write_tail = NULL;
    session->box_write_count = session->box_write_bytes = 0;
    while (write != NULL)
    {
        tb2_mqtt_box_write_t *next = write->next;
        tb2_mqtt_pending_write_t *p = &write->packet;
        if (session->capture_opened)
            tb2_mqtt_capture_packet_ex(&session->capture, "upstream_to_box", p->original,
                p->original_length, p->wire, p->wire_length, p->packet_type, p->topic,
                FALSE, p->filter_id, p->generated, TRUE, "box_write_failed",
                p->original_id, p->wire_id, p->removed);
        osReleaseMutex(&session->io_mutex);
        if (write->completed != NULL) write->completed(write->context, error);
        tb2_mqtt_box_write_free(write);
        osAcquireMutex(&session->io_mutex);
        write = next;
    }
    osReleaseMutex(&session->io_mutex);
}

/** Only the MQTT task writes established Box TLS. Completion callbacks run
 * outside io_mutex and before any response can be read from this connection. */
static error_t tb2_mqtt_box_write_pump(tb2_mqtt_passthrough_session_t *session)
{
    osAcquireMutex(&session->io_mutex);
    error_t error = tb2_mqtt_passthrough_box_write_error(session);
    if (!error) error = atomic_load(&session->local_publish_error);
    while (!error && session->box_write_head != NULL)
    {
        tb2_mqtt_box_write_t *write = session->box_write_head;
        tb2_mqtt_pending_write_t *p = &write->packet;
        if (!session->box_write_active &&
            (tlsIsTxReady(session->box_tls) || tlsGetState(session->box_tls) != TLS_STATE_APPLICATION_DATA))
        {
            error = tb2_mqtt_box_control_step(session);
            if (error) break;
        }
        error = tb2_mqtt_tls_write_step(session, p->wire, p->wire_length, &write->offset);
        if (error) break;
        error_t capture_error = tb2_mqtt_capture_packet_ex(&session->capture, "upstream_to_box",
            p->original, p->original_length, p->wire, p->wire_length, p->packet_type,
            p->topic, TRUE, p->filter_id, p->generated, TRUE,
            p->action != NULL ? p->action : "box_write_complete",
            p->original_id, p->wire_id, p->removed);
        session->capture.bytes_upstream_to_box += p->wire_length;
        session->capture.messages_forwarded_upstream_to_box++;
        tb2_mqtt_status_add_bytes(FALSE, p->wire_length);
        tb2_mqtt_status_add_message(FALSE, FALSE);
        tb2_mqtt_add_nocloud_stats(session, FALSE, p->rewritten, p->removed);
        if (p->packet_type == 2 && p->wire_length == 4 && p->wire[3] == 0)
            session->connack_sent = TRUE;
        session->box_write_head = write->next;
        if (session->box_write_head == NULL) session->box_write_tail = NULL;
        session->box_write_count--;
        session->box_write_bytes -= p->budget_bytes;
        osReleaseMutex(&session->io_mutex);
        if (write->completed != NULL) write->completed(write->context, NO_ERROR);
        tb2_mqtt_box_write_free(write);
        osAcquireMutex(&session->io_mutex);
        if (capture_error) error = tb2_mqtt_box_terminal(session, capture_error, "capture_write_failed");
        if (!session->box_io_ops || !session->box_io_bytes) break;
    }
    if (error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT)
        error = tb2_mqtt_passthrough_box_write_error(session);
    else if (error) error = tb2_mqtt_box_terminal(session, error, "box_write_failed");
    osReleaseMutex(&session->io_mutex);
    return error;
}

static error_t tb2_mqtt_record_packet_ex_locked(tb2_mqtt_passthrough_session_t *session,
                                         bool_t box_to_upstream,
                                         const uint8_t *data, size_t length,
                                         const uint8_t *wire_data, size_t wire_length,
                                         uint8_t packet_type, const char *topic,
                                         bool_t forwarded, const char *filter_id,
                                         bool_t generated, bool_t packet_complete,
                                         const char *action, uint16_t packet_id,
                                         uint16_t wire_packet_id,
                                         bool_t count_blocked, bool_t rewritten,
                                         size_t removed_count)
{
    error_t error = tb2_mqtt_passthrough_box_write_error(session);
    if (!error)
        error = atomic_load(&session->local_publish_error);
    if (error)
        return error;

    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    const bool_t box_delivery = !box_to_upstream && forwarded;
    error = tb2_mqtt_capture_packet_ex(&session->capture, direction,
                                               data, length, wire_data, wire_length,
                                               packet_type, topic, box_delivery ? FALSE : forwarded, filter_id,
                                               generated, packet_complete, box_delivery ? "box_write_pending" : action,
                                               packet_id, wire_packet_id,
                                               removed_count);
    if (error)
    {
        tb2_mqtt_trace_error("capture_write", error);
        return ERROR_WRITE_FAILED;
    }

    if (!forwarded)
    {
        if (count_blocked)
        {
            if (box_to_upstream)
                session->capture.messages_blocked_box_to_upstream++;
            else
                session->capture.messages_blocked_upstream_to_box++;
            tb2_mqtt_status_add_message(box_to_upstream, TRUE);
        }
        tb2_mqtt_add_nocloud_stats(session, box_to_upstream, FALSE,
                                   removed_count);
        return NO_ERROR;
    }

    if (box_delivery)
    {
        tb2_mqtt_box_write_t *write = NULL;
        error = tb2_mqtt_box_write_create(data, length, wire_data, wire_length,
            packet_type, topic, filter_id, generated, action, packet_id,
            wire_packet_id, rewritten, removed_count, &write);
        if (!error) error = tb2_mqtt_box_write_append(session, write);
        if (error) tb2_mqtt_box_write_free(write);
        return error;
    }
    const uint8_t *outgoing = wire_data != NULL ? wire_data : data;
    size_t outgoing_length = wire_data != NULL ? wire_length : length;
    error = tb2_mqtt_tls_write_all(session->upstream.tlsContext, outgoing, outgoing_length);
    if (error)
    {
        return error;
    }
    if (box_to_upstream)
    {
        session->capture.bytes_box_to_upstream += outgoing_length;
        session->capture.messages_forwarded_box_to_upstream++;
    }
    else
    {
        session->capture.bytes_upstream_to_box += outgoing_length;
        session->capture.messages_forwarded_upstream_to_box++;
    }
    tb2_mqtt_status_add_bytes(box_to_upstream, outgoing_length);
    tb2_mqtt_status_add_message(box_to_upstream, FALSE);
    tb2_mqtt_add_nocloud_stats(session, box_to_upstream, rewritten,
                               removed_count);
    return NO_ERROR;
}

/* Serialize capture and packet writes; observers run outside this lock. */
static error_t tb2_mqtt_record_packet_ex(tb2_mqtt_passthrough_session_t *session,
                                         bool_t box_to_upstream,
                                         const uint8_t *data, size_t length,
                                         const uint8_t *wire_data, size_t wire_length,
                                         uint8_t packet_type, const char *topic,
                                         bool_t forwarded, const char *filter_id,
                                         bool_t generated, bool_t packet_complete,
                                         const char *action, uint16_t packet_id,
                                         uint16_t wire_packet_id,
                                         bool_t count_blocked, bool_t rewritten,
                                         size_t removed_count)
{
    osAcquireMutex(&session->io_mutex);
    error_t error = tb2_mqtt_record_packet_ex_locked(session, box_to_upstream,
        data, length, wire_data, wire_length, packet_type, topic, forwarded, filter_id,
        generated, packet_complete, action, packet_id, wire_packet_id,
        count_blocked, rewritten, removed_count);
    osReleaseMutex(&session->io_mutex);
    return error;
}

static error_t tb2_mqtt_record_packet(tb2_mqtt_passthrough_session_t *session,
                                      bool_t box_to_upstream, const uint8_t *data,
                                      size_t length, uint8_t packet_type, const char *topic,
                                      bool_t forwarded, const char *filter_id, bool_t generated,
                                      bool_t packet_complete)
{
    return tb2_mqtt_record_packet_ex(session, box_to_upstream, data, length,
                                     NULL, 0, packet_type, topic, forwarded,
                                      filter_id, generated, packet_complete, NULL,
                                      0, 0, !forwarded, FALSE, 0);
}

static error_t tb2_mqtt_stream_append(tb2_mqtt_stream_t *stream, const uint8_t *data,
                                      size_t length)
{
    if (length > SIZE_MAX - stream->length)
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
    if (length) osMemcpy(stream->data + stream->length, data, length);
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

static tb2_mqtt_qos2_entry_t *tb2_mqtt_qos2_complete(
    tb2_mqtt_qos2_entry_t *list, uint16_t packet_id)
{
    tb2_mqtt_qos2_entry_t *entry = tb2_mqtt_qos2_find(list, packet_id);
    if (entry == NULL)
        return NULL;
    entry->completed = TRUE;
    return entry;
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

static tb2_mqtt_local_response_entry_t *tb2_mqtt_local_response_find(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id, uint8_t qos)
{
    tb2_mqtt_local_response_entry_t *entry = session->local_responses;
    while (entry != NULL)
    {
        if (entry->packet_id == packet_id && entry->qos == qos)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static void tb2_mqtt_local_response_remove(
    tb2_mqtt_passthrough_session_t *session,
    tb2_mqtt_local_response_entry_t *target)
{
    tb2_mqtt_local_response_entry_t **cursor = &session->local_responses;
    while (*cursor != NULL)
    {
        if (*cursor == target)
        {
            *cursor = target->next;
            osFreeMem(target->payload);
            osFreeMem(target);
            if (session->local_response_count > 0)
                session->local_response_count--;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static error_t tb2_mqtt_local_response_make_room(
    tb2_mqtt_passthrough_session_t *session)
{
    while (session->local_response_count >=
           TB2_MQTT_LOCAL_RESPONSE_HISTORY_MAX)
    {
        tb2_mqtt_local_response_entry_t *candidate = NULL;
        for (tb2_mqtt_local_response_entry_t *entry = session->local_responses;
             entry != NULL; entry = entry->next)
        {
            if (entry->completed)
                candidate = entry;
        }
        if (candidate == NULL)
            return ERROR_OUT_OF_RESOURCES;
        tb2_mqtt_local_response_remove(session, candidate);
    }
    return NO_ERROR;
}

static error_t tb2_mqtt_local_response_store(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id, uint8_t qos,
    tb2_mqtt_local_response_action_t action, const uint8_t *payload,
    size_t payload_len, const char *filter_id)
{
    if (packet_id == 0 || (qos != 1 && qos != 2) ||
        (action == TB2_MQTT_LOCAL_RESPONSE_REWRITE && payload == NULL))
    {
        return ERROR_INVALID_PARAMETER;
    }

    tb2_mqtt_local_response_entry_t *existing =
        tb2_mqtt_local_response_find(session, packet_id, qos);
    if (existing != NULL)
        tb2_mqtt_local_response_remove(session, existing);

    error_t error = tb2_mqtt_local_response_make_room(session);
    if (error)
        return error;

    tb2_mqtt_local_response_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL)
        return ERROR_OUT_OF_MEMORY;
    osMemset(entry, 0, sizeof(*entry));

    if (action == TB2_MQTT_LOCAL_RESPONSE_REWRITE)
    {
        entry->payload = osAllocMem(payload_len > 0 ? payload_len : 1);
        if (entry->payload == NULL)
        {
            osFreeMem(entry);
            return ERROR_OUT_OF_MEMORY;
        }
        if (payload_len > 0)
            osMemcpy(entry->payload, payload, payload_len);
    }

    entry->packet_id = packet_id;
    entry->qos = qos;
    entry->action = action;
    entry->payload_len = payload_len;
    entry->filter_id = filter_id;
    entry->next = session->local_responses;
    session->local_responses = entry;
    session->local_response_count++;
    return NO_ERROR;
}

static void tb2_mqtt_local_response_mark_completed(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id, uint8_t qos)
{
    tb2_mqtt_local_response_entry_t *entry =
        tb2_mqtt_local_response_find(session, packet_id, qos);
    if (entry != NULL)
        entry->completed = TRUE;
}

static void tb2_mqtt_local_responses_free(
    tb2_mqtt_passthrough_session_t *session)
{
    while (session->local_responses != NULL)
        tb2_mqtt_local_response_remove(session, session->local_responses);
}

static tb2_mqtt_packet_id_entry_t *tb2_mqtt_packet_id_find_wire(
    tb2_mqtt_passthrough_session_t *session, uint16_t wire_id)
{
    tb2_mqtt_packet_id_entry_t *entry = session->packet_ids;
    while (entry != NULL)
    {
        if (entry->wire_id == wire_id)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static tb2_mqtt_packet_id_entry_t *tb2_mqtt_packet_id_find_upstream(
    tb2_mqtt_passthrough_session_t *session, uint16_t original_id)
{
    tb2_mqtt_packet_id_entry_t *entry = session->packet_ids;
    while (entry != NULL)
    {
        if (!entry->local && entry->original_id == original_id)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static error_t tb2_mqtt_packet_id_add(tb2_mqtt_passthrough_session_t *session,
                                      uint16_t original_id, uint16_t wire_id,
                                      uint8_t qos, bool_t local)
{
    tb2_mqtt_packet_id_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL)
        return ERROR_OUT_OF_MEMORY;
    entry->original_id = original_id;
    entry->wire_id = wire_id;
    entry->qos = qos;
    entry->local = local;
    entry->next = session->packet_ids;
    session->packet_ids = entry;
    return NO_ERROR;
}

static void tb2_mqtt_packet_id_remove(tb2_mqtt_passthrough_session_t *session,
                                      tb2_mqtt_packet_id_entry_t *target)
{
    tb2_mqtt_packet_id_entry_t **cursor = &session->packet_ids;
    while (*cursor != NULL)
    {
        if (*cursor == target)
        {
            *cursor = target->next;
            osFreeMem(target);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static void tb2_mqtt_packet_ids_free(tb2_mqtt_passthrough_session_t *session)
{
    while (session->packet_ids != NULL)
    {
        tb2_mqtt_packet_id_entry_t *removed = session->packet_ids;
        session->packet_ids = removed->next;
        osFreeMem(removed);
    }
}

static error_t tb2_mqtt_allocate_wire_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t *packet_id)
{
    for (uint32_t attempt = 0; attempt < UINT16_MAX; attempt++)
    {
        uint16_t candidate = session->next_local_packet_id;
        if (candidate == 0)
            candidate = UINT16_MAX;
        session->next_local_packet_id = candidate > 1 ? candidate - 1 : UINT16_MAX;
        if (tb2_mqtt_packet_id_find_wire(session, candidate) == NULL)
        {
            *packet_id = candidate;
            return NO_ERROR;
        }
    }
    return ERROR_OUT_OF_RESOURCES;
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
    uint8_t packet[] = {(uint8_t)(type << 4), 0x02U,
                        (uint8_t)(packet_id >> 8), (uint8_t)packet_id};
    bool_t ack_box_to_upstream = !source_box_to_upstream;
    TRACE_DEBUG("TB2 MQTT proxy generated=%s direction=%s packet_id=%u\r\n",
                tb2_mqtt_packet_type_name(type),
                ack_box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                (unsigned)packet_id);
    return tb2_mqtt_record_packet_ex(
        session, ack_box_to_upstream, packet, sizeof(packet), NULL, 0,
        type, NULL, TRUE, NULL, TRUE, TRUE, capture_action,
        packet_id, packet_id, FALSE, FALSE, 0);
}

static error_t tb2_mqtt_parse_publish(const uint8_t *packet, size_t packet_size,
                                      size_t fixed_header_size, char **topic,
                                      const uint8_t **payload, size_t *payload_len,
                                      uint8_t *qos, uint16_t *packet_id,
                                      size_t *packet_id_offset)
{
    *qos = (packet[0] >> 1) & 0x03U;
    if (*qos == 3 || fixed_header_size + 2 > packet_size)
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
    if (osStrlen(*topic) != topic_len)
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

static error_t tb2_mqtt_replay_local_response(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, size_t fixed_header_size, const char *topic,
    const uint8_t *payload, size_t packet_id_offset,
    tb2_mqtt_local_response_entry_t *entry)
{
    TRACE_DEBUG("TB2 MQTT proxy direction=box_to_upstream packet_type=PUBLISH"
                " topic='%s' qos=%u packet_id=%u action=local_response_replay\r\n",
                topic, (unsigned)entry->qos, (unsigned)entry->packet_id);

    if (entry->action != TB2_MQTT_LOCAL_RESPONSE_REWRITE)
    {
        bool_t count_blocked =
            entry->action == TB2_MQTT_LOCAL_RESPONSE_BLOCK;
        error_t error = tb2_mqtt_record_packet_ex(
            session, TRUE, packet, packet_size, NULL, 0,
            TB2_MQTT_PACKET_PUBLISH, topic, FALSE, entry->filter_id, FALSE,
            TRUE, count_blocked ? "local_response_replay_block" :
                                  "local_response_replay_consume",
            entry->packet_id, entry->packet_id, count_blocked, FALSE, 0);
        if (!error)
        {
            error = tb2_mqtt_send_generated_ack(
                session, TRUE, entry->qos == 1 ? TB2_MQTT_PACKET_PUBACK :
                                                 TB2_MQTT_PACKET_PUBREC,
                entry->packet_id,
                count_blocked ?
                    (entry->qos == 1 ? "blocked_replay_puback" :
                                       "blocked_replay_pubrec") :
                    (entry->qos == 1 ? "local_response_replay_puback" :
                                       "local_response_replay_pubrec"));
        }
        if (!error && entry->qos == 1)
            entry->completed = TRUE;
        return error;
    }

    uint8_t *rebuilt = NULL;
    size_t rebuilt_size = 0;
    size_t rebuilt_packet_id_offset = 0;
    error_t error = tb2_mqtt_rebuild_publish(
        packet, packet_size, fixed_header_size, payload, entry->payload,
        entry->payload_len, packet_id_offset, &rebuilt, &rebuilt_size,
        &rebuilt_packet_id_offset);
    if (!error)
    {
        error = tb2_mqtt_record_packet_ex(
            session, TRUE, packet, packet_size, rebuilt, rebuilt_size,
            TB2_MQTT_PACKET_PUBLISH, topic, TRUE, entry->filter_id, TRUE,
            TRUE, "local_response_replay_rewrite", entry->packet_id,
            entry->packet_id, FALSE, TRUE, 0);
    }
    osFreeMem(rebuilt);
    return error;
}

static error_t tb2_mqtt_process_mapped_control(
    tb2_mqtt_passthrough_session_t *session, bool_t box_to_upstream,
    const uint8_t *packet, size_t packet_size, size_t fixed_header_size,
    uint8_t type, bool_t *handled)
{
    *handled = FALSE;
    bool_t from_box_ack = box_to_upstream &&
                          (type == TB2_MQTT_PACKET_PUBACK ||
                           type == TB2_MQTT_PACKET_PUBREC ||
                           type == TB2_MQTT_PACKET_PUBCOMP);
    bool_t from_upstream_pubrel = !box_to_upstream && type == TB2_MQTT_PACKET_PUBREL;
    if (!from_box_ack && !from_upstream_pubrel)
        return NO_ERROR;
    if (packet_size - fixed_header_size != 2)
        return ERROR_INVALID_LENGTH;

    uint16_t packet_id = ((uint16_t)packet[fixed_header_size] << 8) |
                         packet[fixed_header_size + 1];
    if (packet_id == 0)
        return ERROR_INVALID_LENGTH;

    tb2_mqtt_packet_id_entry_t *entry = from_box_ack ?
        tb2_mqtt_packet_id_find_wire(session, packet_id) :
        tb2_mqtt_packet_id_find_upstream(session, packet_id);
    if (entry == NULL)
        return NO_ERROR;

    if (entry->local)
    {
        if (type != TB2_MQTT_PACKET_PUBACK)
            return ERROR_INVALID_TYPE;
        *handled = TRUE;
        error_t error = tb2_mqtt_record_packet_ex(
            session, TRUE, packet, packet_size, NULL, 0, type, NULL, FALSE,
            NULL, FALSE, TRUE, "local_freshness_puback", packet_id,
            packet_id, FALSE, FALSE, 0);
        if (!error)
        {
            tb2_mqtt_packet_id_remove(session, entry);
            if (session->control_observer != NULL)
            {
                session->control_observer(session->observer_context,
                                          TB2_MQTT_CONTROL_LOCAL_PUBACK,
                                          packet_id, NULL, 0);
            }
        }
        return error;
    }

    if ((type == TB2_MQTT_PACKET_PUBACK && entry->qos != 1) ||
        ((type == TB2_MQTT_PACKET_PUBREC || type == TB2_MQTT_PACKET_PUBREL ||
          type == TB2_MQTT_PACKET_PUBCOMP) && entry->qos != 2))
    {
        return ERROR_INVALID_TYPE;
    }

    *handled = TRUE;
    uint16_t outgoing_id = from_box_ack ? entry->original_id : entry->wire_id;
    uint8_t *rewritten = NULL;
    error_t error = tb2_mqtt_rewrite_packet_id(packet, packet_size,
                                               fixed_header_size,
                                               outgoing_id, &rewritten);
    if (!error)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, rewritten,
            packet_size, type, NULL, TRUE, NULL, FALSE, TRUE,
            entry->wire_id == entry->original_id ? NULL : "packet_id_remap",
            entry->original_id, entry->wire_id, FALSE, FALSE, 0);
    }
    osFreeMem(rewritten);
    if (!error && (type == TB2_MQTT_PACKET_PUBACK ||
                   type == TB2_MQTT_PACKET_PUBCOMP))
    {
        tb2_mqtt_packet_id_remove(session, entry);
    }
    return error;
}

static error_t tb2_mqtt_process_packet(tb2_mqtt_passthrough_session_t *session,
                                       bool_t box_to_upstream, const uint8_t *packet,
                                       size_t packet_size, size_t fixed_header_size)
{
    uint8_t type = packet[0] >> 4;
    if (box_to_upstream)
    {
        if (!mqtt_monotonic_ms(&session->last_box_rx)) return ERROR_FAILURE;
        /* Inspect CONNECT only for its existing receive deadline. TONIES still
         * validates/authenticates and answers the unchanged CONNECT packet. */
        if (type == 1 && packet_size - fixed_header_size >= 2)
        {
            const uint8_t *body = packet + fixed_header_size;
            size_t protocol_size = ((size_t)body[0] << 8) | body[1];
            if (protocol_size <= packet_size - fixed_header_size - 2 &&
                packet_size - fixed_header_size - 2 - protocol_size >= 4)
                session->keepalive = ((uint16_t)body[2 + protocol_size + 2] << 8) |
                    body[2 + protocol_size + 3];
        }
    }
    if (type == TB2_MQTT_PACKET_PUBREL)
    {
        if (packet_size - fixed_header_size < 2)
            return ERROR_INVALID_LENGTH;
        uint16_t packet_id = ((uint16_t)packet[fixed_header_size] << 8) |
                             packet[fixed_header_size + 1];
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        tb2_mqtt_qos2_entry_t *qos2_entry =
            tb2_mqtt_qos2_complete(*list, packet_id);
        if (qos2_entry != NULL)
        {
            error_t error = tb2_mqtt_record_packet_ex(
                session, box_to_upstream, packet, packet_size, NULL, 0,
                type, NULL, FALSE, qos2_entry->filter_id, FALSE, TRUE,
                qos2_entry->capture_action != NULL ?
                    qos2_entry->capture_action : "qos2_blocked_publish",
                packet_id, packet_id, qos2_entry->count_blocked, FALSE, 0);
            if (!error)
            {
                error = tb2_mqtt_send_generated_ack(
                    session, box_to_upstream, TB2_MQTT_PACKET_PUBCOMP,
                    packet_id, qos2_entry->count_blocked ?
                                   "blocked_pubcomp" :
                                   "local_response_pubcomp");
            }
            if (!error)
                tb2_mqtt_local_response_mark_completed(session, packet_id, 2);
            return error;
        }
    }

    bool_t mapped_control_handled = FALSE;
    error_t error = tb2_mqtt_process_mapped_control(
        session, box_to_upstream, packet, packet_size, fixed_header_size,
        type, &mapped_control_handled);
    if (error || mapped_control_handled)
        return error;

    if (box_to_upstream &&
        (type == TB2_MQTT_PACKET_SUBSCRIBE ||
         type == TB2_MQTT_PACKET_UNSUBSCRIBE))
    {
        if (packet_size - fixed_header_size < 2)
            return ERROR_INVALID_LENGTH;
        uint16_t packet_id = ((uint16_t)packet[fixed_header_size] << 8) |
                             packet[fixed_header_size + 1];
        if (packet_id == 0)
            return ERROR_INVALID_LENGTH;
        if (session->control_observer != NULL)
        {
            session->control_observer(
                session->observer_context,
                type == TB2_MQTT_PACKET_SUBSCRIBE ?
                    TB2_MQTT_CONTROL_SUBSCRIBE : TB2_MQTT_CONTROL_UNSUBSCRIBE,
                packet_id, packet + fixed_header_size,
                packet_size - fixed_header_size);
        }
    }

    if (type != TB2_MQTT_PACKET_PUBLISH)
    {
        TRACE_DEBUG("TB2 MQTT proxy direction=%s packet_type=%s action=forward bytes=%" PRIuSIZE "\r\n",
                    box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                    tb2_mqtt_packet_type_name(type), packet_size);
        tb2_mqtt_local_response_entry_t *completed_response = NULL;
        if (!box_to_upstream &&
            (type == TB2_MQTT_PACKET_PUBACK ||
             type == TB2_MQTT_PACKET_PUBCOMP) &&
            packet_size - fixed_header_size == 2)
        {
            uint16_t packet_id =
                ((uint16_t)packet[fixed_header_size] << 8) |
                packet[fixed_header_size + 1];
            uint8_t qos = type == TB2_MQTT_PACKET_PUBACK ? 1 : 2;
            tb2_mqtt_local_response_entry_t *entry =
                tb2_mqtt_local_response_find(session, packet_id, qos);
            if (entry != NULL &&
                entry->action == TB2_MQTT_LOCAL_RESPONSE_REWRITE)
            {
                completed_response = entry;
            }
        }

        error = tb2_mqtt_record_packet(session, box_to_upstream, packet,
                                       packet_size, type, NULL, TRUE, NULL,
                                       FALSE, TRUE);
        if (!error && completed_response != NULL)
            tb2_mqtt_local_response_remove(session, completed_response);
        return error;
    }

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

    if (box_to_upstream && qos > 0)
    {
        bool_t duplicate = (packet[0] & 0x08U) != 0;
        tb2_mqtt_local_response_entry_t *entry =
            tb2_mqtt_local_response_find(session, packet_id, qos);
        if (duplicate && entry != NULL)
        {
            error = tb2_mqtt_replay_local_response(
                session, packet, packet_size, fixed_header_size, topic,
                payload, packet_id_offset, entry);
            osFreeMem(topic);
            return error;
        }
        if (!duplicate && entry != NULL)
            tb2_mqtt_local_response_remove(session, entry);
        if (!duplicate && qos == 2)
        {
            tb2_mqtt_qos2_entry_t **list =
                tb2_mqtt_qos2_list(session, box_to_upstream);
            tb2_mqtt_qos2_entry_t *stale =
                tb2_mqtt_qos2_find(*list, packet_id);
            if (stale != NULL)
                tb2_mqtt_qos2_remove(list, stale);
        }
    }

    tb2_mqtt_observer_result_t observer_result;
    osMemset(&observer_result, 0, sizeof(observer_result));
    observer_result.action = TB2_MQTT_OBSERVER_FORWARD;
    if (session->observer != NULL)
    {
        error = session->observer(session->observer_context, box_to_upstream,
                                  topic, payload, payload_len, qos,
                                  &observer_result);
        if (error)
        {
            osFreeMem(observer_result.payload);
            osFreeMem(topic);
            return error;
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
    if (!blocked)
    {
        const char *manual_filter_id = NULL;
        blocked = mqtt_forward_filter_should_block(
            session->box_settings, topic, filtered_payload,
            filtered_payload_len, &manual_filter_id);
        if (blocked)
            filter_id = manual_filter_id;
    }

    if (box_to_upstream && qos > 0 && (local_consume || local_rewrite))
    {
        tb2_mqtt_local_response_action_t response_action = local_consume ?
            TB2_MQTT_LOCAL_RESPONSE_CONSUME :
            (blocked ? TB2_MQTT_LOCAL_RESPONSE_BLOCK :
                       TB2_MQTT_LOCAL_RESPONSE_REWRITE);
        error = tb2_mqtt_local_response_store(
            session, packet_id, qos, response_action,
            observer_result.payload, observer_result.payload_len, filter_id);
        if (error)
        {
            TRACE_ERROR("TB2 MQTT proxy local response state rejected"
                        " packet_id=%u qos=%u error=%s code=%d\r\n",
                        (unsigned)packet_id, (unsigned)qos,
                        error2text(error), (int)error);
            error_t capture_error = tb2_mqtt_record_packet_ex(
                session, TRUE, packet, packet_size, NULL, 0, type, topic,
                FALSE, filter_id, FALSE, TRUE,
                "local_response_state_limit", packet_id, packet_id, FALSE,
                FALSE, 0);
            osFreeMem(observer_result.payload);
            mqtt_nocloud_filter_result_free(&nocloud_result);
            osFreeMem(topic);
            return capture_error ? capture_error : error;
        }
    }

    uint8_t *observer_packet = NULL;
    uint8_t *nocloud_packet = NULL;
    size_t forwarded_packet_size = packet_size;
    size_t forwarded_packet_id_offset = packet_id_offset;
    const uint8_t *forwarded_packet = packet;
    if (!blocked && local_rewrite)
    {
        error = tb2_mqtt_rebuild_publish(
            packet, packet_size, fixed_header_size, payload,
            observer_result.payload, observer_result.payload_len,
            packet_id_offset, &observer_packet, &forwarded_packet_size,
            &forwarded_packet_id_offset);
        if (!error)
            forwarded_packet = observer_packet;
    }
    else if (!blocked && nocloud_result.action == MQTT_NOCLOUD_REWRITE)
    {
        error_t rebuild_error = tb2_mqtt_rebuild_publish(
            packet, packet_size, fixed_header_size, payload,
            (const uint8_t *)nocloud_result.payload,
            nocloud_result.payload_len, packet_id_offset, &nocloud_packet,
            &forwarded_packet_size, &forwarded_packet_id_offset);
        if (rebuild_error)
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
            forwarded_packet = nocloud_packet;
            filter_id = nocloud_result.filter_id;
        }
    }

    const char *decision = local_consume ? "consume" :
                           blocked ? "block" :
                           local_rewrite ? "rewrite" :
                           nocloud_result.action == MQTT_NOCLOUD_REWRITE ?
                               "rewrite" : "forward";
    TRACE_DEBUG("TB2 MQTT proxy direction=%s packet_type=PUBLISH topic='%s' qos=%u"
                " packet_id=%u action=%s filter=%s payload_len=%" PRIuSIZE
                " removed=%" PRIuSIZE "\r\n",
                box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                topic, (unsigned)qos, (unsigned)packet_id,
                decision, filter_id != NULL ? filter_id : "-", filtered_payload_len,
                nocloud_result.removed_count);

    uint8_t *wire_packet = NULL;
    tb2_mqtt_packet_id_entry_t *mapping = NULL;
    if (!blocked && !box_to_upstream && qos > 0)
    {
        mapping = tb2_mqtt_packet_id_find_upstream(session, packet_id);
        if (mapping == NULL)
        {
            uint16_t wire_id = packet_id;
            if (tb2_mqtt_packet_id_find_wire(session, wire_id) != NULL)
            {
                error = tb2_mqtt_allocate_wire_packet_id(session, &wire_id);
            }
            if (!error)
            {
                error = tb2_mqtt_packet_id_add(session, packet_id, wire_id,
                                               qos, FALSE);
            }
            if (!error)
                mapping = tb2_mqtt_packet_id_find_upstream(session, packet_id);
        }
        if (!error && mapping != NULL && mapping->wire_id != packet_id)
        {
            error = tb2_mqtt_rewrite_packet_id(
                forwarded_packet, forwarded_packet_size,
                forwarded_packet_id_offset, mapping->wire_id, &wire_packet);
        }
    }

    if (!error && local_consume)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0, type,
            topic, FALSE, observer_result.filter_id, FALSE, TRUE,
            observer_result.capture_action != NULL ?
                observer_result.capture_action : "local_response_consume",
            packet_id, packet_id, FALSE, FALSE, 0);
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
            nocloud_result.removed_count);
    }
    else if (!error && !blocked && local_rewrite)
    {
        const uint8_t *actual_wire = wire_packet != NULL ?
                                         wire_packet : forwarded_packet;
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, actual_wire,
            forwarded_packet_size, type, topic, TRUE,
            observer_result.filter_id, TRUE, TRUE,
            observer_result.capture_action != NULL ?
                observer_result.capture_action : "local_response_rewrite",
            packet_id, mapping != NULL ? mapping->wire_id : packet_id,
            FALSE, TRUE, 0);
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
            nocloud_result.removed_count);
    }
    else if (!error && wire_packet != NULL)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, wire_packet,
            packet_size, type, topic, TRUE, NULL, FALSE, TRUE,
            "packet_id_remap", mapping->original_id, mapping->wire_id, FALSE,
            FALSE, 0);
    }
    else if (!error && observer_result.locally_processed)
    {
        error = tb2_mqtt_record_packet_ex(
            session, box_to_upstream, packet, packet_size, NULL, 0, type,
            topic, !blocked, filter_id, FALSE, TRUE,
            blocked ? "local_status_manual_block" : "local_status_forward",
            packet_id, packet_id, blocked, FALSE, 0);
    }
    else if (!error)
    {
        error = tb2_mqtt_record_packet(session, box_to_upstream, packet, packet_size,
                                       type, topic, !blocked, filter_id, FALSE, TRUE);
    }
    osFreeMem(wire_packet);
    osFreeMem(observer_packet);
    osFreeMem(nocloud_packet);
    osFreeMem(observer_result.payload);
    mqtt_nocloud_filter_result_free(&nocloud_result);
    if (!error && blocked && qos == 1)
    {
        error = tb2_mqtt_send_generated_ack(
            session, box_to_upstream, 4U, packet_id,
            local_consume ? "local_response_puback" : "blocked_puback");
        if (!error && (local_consume || local_rewrite))
            tb2_mqtt_local_response_mark_completed(session, packet_id, qos);
    }
    else if (!error && blocked && qos == 2)
    {
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        error = tb2_mqtt_qos2_begin(
            list, packet_id, !local_consume,
            local_consume ? observer_result.filter_id : filter_id,
            local_consume ? "local_response_consume" :
                            "qos2_blocked_publish");
        if (!error)
            error = tb2_mqtt_send_generated_ack(
                session, box_to_upstream, 5U, packet_id,
                local_consume ? "local_response_pubrec" : "blocked_pubrec");
    }
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

    unsigned processed = 0;
    while (stream->length > 0 && processed < TB2_MQTT_BOX_IO_OPS_PER_TICK)
    {
        size_t packet_size = 0;
        size_t fixed_header_size = 0;
        error = tb2_mqtt_packet_size(stream->data, stream->length, &packet_size,
                                     &fixed_header_size);
        if (error == ERROR_WOULD_BLOCK || packet_size > stream->length)
            return NO_ERROR;
        if (error)
            return error;
        if (!box_to_upstream && packet_size > TB2_MQTT_CLOUD_PACKET_BUDGET)
            return ERROR_OUT_OF_RESOURCES;
        osAcquireMutex(&session->io_mutex);
        /* Reserve an empty output FIFO before observers or QoS state mutate. */
        if (session->box_write_head != NULL)
        { osReleaseMutex(&session->io_mutex); return NO_ERROR; }
        session->processing_packet = TRUE;
        osReleaseMutex(&session->io_mutex);
        parser_session = session;
        error = tb2_mqtt_process_packet(session, box_to_upstream, stream->data,
                                        packet_size, fixed_header_size);
        parser_session = NULL;
        osAcquireMutex(&session->io_mutex);
        session->processing_packet = FALSE;
        osReleaseMutex(&session->io_mutex);
        if (error)
            return error;
        processed++;
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
    osAcquireMutex(&session->io_mutex);
    error_t error = tb2_mqtt_passthrough_box_write_error(session);
    if (!error)
        error = atomic_load(&session->local_publish_error);
    if (error)
    {
        osReleaseMutex(&session->io_mutex);
        return error;
    }
    if (session->box_write_head != NULL || session->box_write_active)
    { osReleaseMutex(&session->io_mutex); return NO_ERROR; }
    if (box_to_upstream &&
        (tlsIsTxReady(session->box_tls) || tlsGetState(session->box_tls) < TLS_STATE_APPLICATION_DATA))
    {
        error = tb2_mqtt_box_control_step(session);
        if (error)
        {
            osReleaseMutex(&session->io_mutex);
            return error == ERROR_WOULD_BLOCK ? NO_ERROR : error;
        }
    }
    TlsContext *source = box_to_upstream ? session->box_tls : session->upstream.tlsContext;
    Socket *source_socket = box_to_upstream ? session->box_socket : session->upstream.socket;
    if (!tlsIsRxReady(source) &&
        (tcpWaitForEvents(source_socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY) == 0)
    {
        osReleaseMutex(&session->io_mutex);
        return NO_ERROR;
    }

    uint8_t buffer[TB2_MQTT_TUNNEL_BUFFER_SIZE];
    size_t received = 0;
    error = tlsRead(source, buffer, sizeof(buffer), &received, 0);
    osReleaseMutex(&session->io_mutex);
    if (error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT)
        return NO_ERROR;
    if (error)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=tls_read direction=%s failed error=%s code=%d\r\n",
                    box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                    error2text(error), (int)error);
        return error;
    }
    if (received == 0)
        return ERROR_END_OF_STREAM;
    return tb2_mqtt_process_stream(session, box_to_upstream, buffer, received);
}

error_t tb2_mqtt_passthrough_init(void)
{
    osMemset(&mqtt_passthrough_status, 0, sizeof(mqtt_passthrough_status));
    if (!osCreateMutex(&mqtt_passthrough_status.mutex))
    {
        return ERROR_OUT_OF_RESOURCES;
    }
    mqtt_passthrough_status.initialized = TRUE;
    osStrcpy(mqtt_passthrough_status.state, "disabled");
    return NO_ERROR;
}

void tb2_mqtt_passthrough_deinit(void)
{
    if (mqtt_passthrough_status.initialized)
    {
        osDeleteMutex(&mqtt_passthrough_status.mutex);
        mqtt_passthrough_status.initialized = FALSE;
    }
}

bool_t tb2_mqtt_passthrough_is_enabled(void)
{
    settings_t *settings = get_settings();
    return settings->mqtt_client_upstream.enabled;
}

error_t tb2_mqtt_passthrough_start(TlsContext *box_tls, Socket *box_socket,
                                   tb2_mqtt_passthrough_session_t **session,
                                   bool_t *handled,
                                   tb2_mqtt_publish_observer_t observer,
                                   tb2_mqtt_control_observer_t control_observer,
                                   void *observer_context,
                                   settings_t **box_settings_out)
{
    *session = NULL;
    *handled = FALSE;
    if (box_settings_out != NULL)
        *box_settings_out = NULL;
    if (!tb2_mqtt_passthrough_is_enabled())
    {
        return NO_ERROR;
    }
    *handled = TRUE;
    TRACE_DEBUG("TB2 MQTT upstream stage=passthrough_start enabled=true\r\n");

    settings_t *box_settings = tb2_mqtt_settings_from_certificate(box_tls);
    if (box_settings == NULL)
    {
        tb2_mqtt_trace_error("map_box_identity", ERROR_FAILURE);
        tb2_mqtt_status_attempt_failed("identity_unavailable");
        return ERROR_FAILURE;
    }
    settings_t *identity_settings = tb2_mqtt_select_identity_settings(box_settings);
    if (!tb2_mqtt_has_original_identity(identity_settings))
    {
        TRACE_ERROR("TB2 MQTT upstream stage=select_upstream_identity identity_material"
                    " settings=available ca=%s certificate=%s key=%s\r\n",
                    identity_settings != NULL && identity_settings->internal.client_tb2.ca != NULL &&
                            identity_settings->internal.client_tb2.ca[0] != '\0' ? "available" : "missing",
                    identity_settings != NULL && identity_settings->internal.client_tb2.crt != NULL &&
                            identity_settings->internal.client_tb2.crt[0] != '\0' ? "available" : "missing",
                    identity_settings != NULL && identity_settings->internal.client_tb2.key != NULL &&
                            identity_settings->internal.client_tb2.key[0] != '\0' ? "available" : "missing");
        tb2_mqtt_trace_error("select_upstream_identity", ERROR_FAILURE);
        tb2_mqtt_status_attempt_failed("upstream_identity_unavailable");
        return ERROR_FAILURE;
    }

    tb2_mqtt_passthrough_session_t *created = osAllocMem(sizeof(*created));
    if (created == NULL)
    {
        tb2_mqtt_status_attempt_failed("out_of_memory");
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(created, 0, sizeof(*created));
    atomic_init(&created->box_write_error, NO_ERROR);
    atomic_init(&created->local_publish_error, NO_ERROR);
    if (!osCreateMutex(&created->io_mutex))
    {
        osFreeMem(created);
        return ERROR_OUT_OF_RESOURCES;
    }
    created->box_tls = box_tls;
    created->box_socket = box_socket;
    created->box_settings = box_settings;
    created->observer = observer;
    created->control_observer = control_observer;
    created->observer_context = observer_context;
    created->next_local_packet_id = UINT16_MAX;
    created->owner = ++next_owner;

    error_t error = tb2_mqtt_capture_open(&created->capture, get_settings());
    if (error)
    {
        tb2_mqtt_trace_error("capture_open", error);
        tb2_mqtt_status_attempt_failed("capture_open_failed");
        osDeleteMutex(&created->io_mutex);
        osFreeMem(created);
        return error;
    }
    created->capture_opened = TRUE;
    tb2_mqtt_status_start();

    error = httpClientInit(&created->upstream);
    if (error)
    {
        tb2_mqtt_trace_error("http_client_init", error);
    }
    if (!error)
    {
        created->upstream_initialized = TRUE;
        error = tb2_mqtt_connect_upstream(identity_settings, &created->upstream);
    }
    if (error)
    {
        tb2_mqtt_trace_error("passthrough_connect", error);
        tb2_mqtt_passthrough_close(created, "connect_failed", FALSE);
        return error;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(box_socket->descriptor, FIONBIO, &nonblocking) == SOCKET_ERROR)
    {
        tb2_mqtt_passthrough_close(created, "box_nonblocking_failed", FALSE);
        return ERROR_FAILURE;
    }
#endif
    created->previous_send = box_tls->socketSendCallback;
    created->previous_receive = box_tls->socketReceiveCallback;
    created->previous_handle = box_tls->socketHandle;
    error = tlsSetSocketCallbacks(box_tls, tb2_mqtt_box_send, tb2_mqtt_box_receive, created);
    if (error)
    {
        tb2_mqtt_passthrough_close(created, "box_callbacks_failed", FALSE);
        return error;
    }
    tb2_mqtt_status_connected();
    TRACE_DEBUG("TB2 MQTT upstream stage=passthrough_start connected=true session=%s\r\n",
                created->capture.session_id);
    *session = created;
    if (box_settings_out != NULL)
        *box_settings_out = box_settings;
    return NO_ERROR;
}

error_t tb2_mqtt_passthrough_forward_initial(tb2_mqtt_passthrough_session_t *session,
                                             const uint8_t *data, size_t length)
{
    if (session == NULL || data == NULL || length == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }
    return tb2_mqtt_process_stream(session, TRUE, data, length);
}

error_t tb2_mqtt_passthrough_task(tb2_mqtt_passthrough_session_t *session)
{
    if (session == NULL) return ERROR_INVALID_PARAMETER;
    if (!tb2_mqtt_passthrough_is_enabled()) return ERROR_ABORTED;
    osAcquireMutex(&session->io_mutex);
    session->box_io_ops = TB2_MQTT_BOX_IO_OPS_PER_TICK;
    session->box_io_bytes = TB2_MQTT_BOX_IO_BYTES_PER_TICK;
    osReleaseMutex(&session->io_mutex);
    error_t error = tb2_mqtt_box_write_pump(session);
    if (!error) error = tb2_mqtt_process_stream(session, TRUE, NULL, 0);
    if (!error) error = tb2_mqtt_forward_ready(session, TRUE);
    if (!error) error = tb2_mqtt_process_stream(session, FALSE, NULL, 0);
    if (!error) error = tb2_mqtt_forward_ready(session, FALSE);
    uint32_t now;
    if (!error && !mqtt_monotonic_ms(&now)) return ERROR_FAILURE;
    if (!error && session->keepalive &&
        (uint32_t)(now - session->last_box_rx) > (uint32_t)session->keepalive * 1500U)
    {
        TRACE_WARNING("TB2 MQTT mqtt_keepalive_timeout box=%s session=%" PRIu64 " decision=close\r\n",
            session->box_settings->commonName, session->owner);
        return ERROR_TIMEOUT;
    }
    return error;
}

bool_t tb2_mqtt_passthrough_is_established(const tb2_mqtt_passthrough_session_t *session)
{
    return session != NULL && session->connack_sent;
}

error_t tb2_mqtt_passthrough_reserve_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t *packet_id)
{
    if (session == NULL || packet_id == NULL)
        return ERROR_INVALID_PARAMETER;

    uint16_t reserved = 0;
    error_t error = tb2_mqtt_allocate_wire_packet_id(session, &reserved);
    if (!error)
        error = tb2_mqtt_packet_id_add(session, reserved, reserved, 1, TRUE);
    if (!error)
        *packet_id = reserved;
    return error;
}

void tb2_mqtt_passthrough_release_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id)
{
    if (session == NULL || packet_id == 0)
        return;
    tb2_mqtt_packet_id_entry_t *entry =
        tb2_mqtt_packet_id_find_wire(session, packet_id);
    if (entry != NULL && entry->local)
        tb2_mqtt_packet_id_remove(session, entry);
}

mqtt_delivery_result_t tb2_mqtt_passthrough_submit_local_batch(
    tb2_mqtt_passthrough_session_t *session,
    const tb2_mqtt_local_publish_t *publishes, size_t count)
{
    if (session == NULL || publishes == NULL || !count || count > TB2_MQTT_INFLIGHT_MAX)
        return mqtt_delivery_result(MQTT_DELIVERY_FAILED, ERROR_INVALID_PARAMETER);
    tb2_mqtt_box_write_t *head = NULL, *tail = NULL;
    size_t bytes = 0;
    error_t error = NO_ERROR;
    for (size_t i = 0; i < count && !error; i++)
    {
        const tb2_mqtt_local_publish_t *item = &publishes[i];
        if (item->packet == NULL || !item->packet_size || item->topic == NULL ||
            (item->packet[0] >> 4) != TB2_MQTT_PACKET_PUBLISH || ((item->packet[0] >> 1) & 3U) > 1U)
        { error = ERROR_INVALID_PARAMETER; break; }
        if (item->packet[0] & 2U)
        {
            tb2_mqtt_packet_id_entry_t *entry = tb2_mqtt_packet_id_find_wire(session, item->packet_id);
            bool_t valid = item->packet_id != 0 && entry != NULL && entry->local;
            if (!valid) { error = ERROR_INVALID_PARAMETER; break; }
        }
        tb2_mqtt_box_write_t *write = NULL;
        error = tb2_mqtt_box_write_create(item->packet, item->packet_size, NULL, 0,
            TB2_MQTT_PACKET_PUBLISH, item->topic, NULL, TRUE,
            item->capture_action != NULL ? item->capture_action : "local_publish",
            item->packet_id, item->packet_id, FALSE, 0, &write);
        if (error) break;
        if (item->context_bytes > TB2_MQTT_BUFFER_LIMIT - write->packet.budget_bytes ||
            bytes > TB2_MQTT_BUFFER_LIMIT - write->packet.budget_bytes - item->context_bytes)
        { tb2_mqtt_box_write_free(write); error = ERROR_OUT_OF_RESOURCES; break; }
        write->packet.budget_bytes += item->context_bytes;
        bytes += write->packet.budget_bytes;
        write->completed = item->completed;
        write->context = item->context;
        if (tail != NULL) tail->next = write;
        else head = write;
        tail = write;
    }
    osAcquireMutex(&session->io_mutex);
    error_t terminal = tb2_mqtt_passthrough_box_write_error(session);
    if (!terminal) terminal = atomic_load(&session->local_publish_error);
    bool_t busy = session->box_write_stalled ||
        (session->processing_packet && parser_session != session) ||
        count > TB2_MQTT_INFLIGHT_MAX - session->box_write_count ||
        bytes > TB2_MQTT_BUFFER_LIMIT - session->box_write_bytes;
    if (!error && !terminal && !busy)
    {
        if (session->box_write_tail != NULL) session->box_write_tail->next = head;
        else session->box_write_head = head;
        session->box_write_tail = tail;
        session->box_write_count += count;
        session->box_write_bytes += bytes;
        for (tb2_mqtt_box_write_t *write = head; write != NULL; write = write->next)
        {
            tb2_mqtt_pending_write_t *p = &write->packet;
            if (session->capture_opened &&
                tb2_mqtt_capture_packet_ex(&session->capture, "upstream_to_box", p->original,
                    p->original_length, p->wire, p->wire_length, p->packet_type, p->topic,
                    FALSE, p->filter_id, TRUE, TRUE, "box_write_pending", p->original_id,
                    p->wire_id, 0))
                atomic_store(&session->local_publish_error, ERROR_WRITE_FAILED);
        }
        osReleaseMutex(&session->io_mutex);
        return mqtt_delivery_result(MQTT_DELIVERY_QUEUED, NO_ERROR);
    }
    osReleaseMutex(&session->io_mutex);
    while (head != NULL)
    { tb2_mqtt_box_write_t *next = head->next; tb2_mqtt_box_write_free(head); head = next; }
    return terminal ? mqtt_delivery_result(MQTT_DELIVERY_FAILED, terminal) :
        error ? mqtt_delivery_result(MQTT_DELIVERY_FAILED, error) :
        mqtt_delivery_result(MQTT_DELIVERY_BUSY, ERROR_WOULD_BLOCK);
}

mqtt_delivery_result_t tb2_mqtt_passthrough_submit_local_publish(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, const char *topic, uint16_t packet_id,
    const char *capture_action, tb2_mqtt_local_write_completed_t completed,
    void *context, size_t context_bytes)
{
    const tb2_mqtt_local_publish_t item = {
        packet, packet_size, topic, packet_id, capture_action, completed, context, context_bytes
    };
    return tb2_mqtt_passthrough_submit_local_batch(session, &item, 1);
}

error_t tb2_mqtt_passthrough_write_local_publish(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, const char *topic, uint16_t packet_id,
    const char *capture_action)
{
    mqtt_delivery_result_t result = tb2_mqtt_passthrough_submit_local_publish(session,
        packet, packet_size, topic, packet_id, capture_action, NULL, NULL, 0);
    return mqtt_delivery_accepted(result) ? NO_ERROR : result.error;
}

void tb2_mqtt_passthrough_close(tb2_mqtt_passthrough_session_t *session,
                                const char *result_code, bool_t success)
{
    if (session == NULL)
    {
        return;
    }
    error_t cancel_error = tb2_mqtt_passthrough_box_write_error(session);
    tb2_mqtt_box_writes_cancel(session, cancel_error ? cancel_error : ERROR_NOT_CONNECTED);
    if (session->upstream_initialized)
    {
        httpClientDeinit(&session->upstream);
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
        tb2_mqtt_status_finish(success, result_code);
        TRACE_INFO("TB2 MQTT passthrough session=%s status=%s up=%llu down=%llu\r\n",
                   session->capture.session_id, result_code,
                   (unsigned long long)session->capture.bytes_box_to_upstream,
                   (unsigned long long)session->capture.bytes_upstream_to_box);
    }
    osFreeMem(session->box_stream.data);
    osFreeMem(session->upstream_stream.data);
    tb2_mqtt_qos2_free(&session->blocked_qos2_box);
    tb2_mqtt_qos2_free(&session->blocked_qos2_upstream);
    tb2_mqtt_local_responses_free(session);
    tb2_mqtt_packet_ids_free(session);
    if (session->previous_send != NULL && session->previous_receive != NULL)
        tlsSetSocketCallbacks(session->box_tls, session->previous_send,
            session->previous_receive, session->previous_handle);
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
    else if (mqtt_passthrough_status.active_sessions == 0 &&
             mqtt_passthrough_status.error_code[0] == '\0')
    {
        state = "ready";
    }
    else
    {
        state = mqtt_passthrough_status.state;
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
