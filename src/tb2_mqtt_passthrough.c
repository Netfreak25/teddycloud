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
#define TB2_MQTT_CAPTURE_MIB_BYTES (1024ULL * 1024ULL)
#define TB2_MQTT_MAX_REMAINING_LENGTH 268435455U
#define TB2_MQTT_PACKET_PUBLISH 3U
#define TB2_MQTT_PACKET_PUBREL 6U

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
    struct tb2_mqtt_qos2_entry *next;
} tb2_mqtt_qos2_entry_t;

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
    time_t started_at;
} tb2_mqtt_capture_t;

struct tb2_mqtt_passthrough_session
{
    TlsContext *box_tls;
    Socket *box_socket;
    HttpClientContext upstream;
    bool_t upstream_initialized;
    settings_t *box_settings;
    tb2_mqtt_publish_observer_t observer;
    void *observer_context;
    tb2_mqtt_stream_t box_stream;
    tb2_mqtt_stream_t upstream_stream;
    tb2_mqtt_qos2_entry_t *blocked_qos2_box;
    tb2_mqtt_qos2_entry_t *blocked_qos2_upstream;
    tb2_mqtt_capture_t capture;
    bool_t capture_opened;
};

static tb2_mqtt_passthrough_status_t mqtt_passthrough_status;

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
    if (subject == NULL || issuer == NULL || subject[0] == '\0' || issuer[0] == '\0')
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=certificate_identity_missing\r\n");
        return NULL;
    }
    TRACE_DEBUG("TB2 MQTT upstream stage=box_client_auth certificate_present=true"
                " subject='%s' issuer='%s' serial='%s'\r\n",
                subject, issuer, tls_context->client_cert_serial);

    char common_name[32] = "";
    size_t subject_length = osStrlen(subject);
    if (subject_length == 15 && osStrncmp(subject, "b'", 2) == 0 && subject[14] == '\'')
    {
        osMemcpy(common_name, subject + 2, 12);
        common_name[12] = '\0';
    }
    else if (subject_length == 12)
    {
        osStrncpy(common_name, subject, sizeof(common_name) - 1);
    }
    else
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=subject_format length=%"
                    PRIuSIZE "\r\n", subject_length);
        return NULL;
    }
    osStringToLower(common_name);

    bool_t trusted_issuer = osStrstr(issuer, "Toniebox Root CA") != NULL ||
                            osStrstr(issuer, "Toniebox SubCA") != NULL ||
                            osStrstr(issuer, "Boxine Factory SubCA") != NULL;
    if (!trusted_issuer)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=issuer_untrusted\r\n");
        return NULL;
    }

    settings_t *settings = get_settings_cn(common_name);
    bool_t overlay_found = settings != NULL && settings->internal.overlayNumber > 0 &&
                           settings->internal.config_used &&
                           osStrcmp(settings->commonName, common_name) == 0;
    if (!overlay_found)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=overlay_validation"
                    " overlay_found=%s config_used=%s trusted_issuer=%s\r\n",
                    overlay_found ? "true" : "false",
                    settings != NULL && settings->internal.config_used ? "true" : "false",
                    trusted_issuer ? "true" : "false");
        return NULL;
    }
    if (settings->toniebox.boxGeneration == GENERATION_UNKNOWN)
    {
        settings_set_unsigned_id("toniebox.boxGeneration", GENERATION_TB2,
                                 settings->internal.overlayNumber);
        settings_load_certs_id(settings->internal.overlayNumber);
        TRACE_DEBUG("TB2 MQTT upstream stage=map_box_identity generation=tb2 source=mqtt\r\n");
    }
    if (settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity failed reason=box_generation"
                    " actual=%u expected=%u\r\n",
                    (unsigned)settings->toniebox.boxGeneration,
                    (unsigned)GENERATION_TB2);
        return NULL;
    }
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

static error_t tb2_mqtt_capture_packet(tb2_mqtt_capture_t *capture, const char *direction,
                                       const uint8_t *data, size_t length, uint8_t packet_type,
                                       const char *topic, bool_t forwarded, const char *filter_id,
                                       bool_t generated, bool_t packet_complete)
{
    size_t encoded_length = 0;
    base64Encode(data, length, NULL, &encoded_length);
    char *encoded = osAllocMem(encoded_length + 1);
    if (encoded == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    base64Encode(data, length, encoded, &encoded_length);
    encoded[encoded_length] = '\0';

    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL)
    {
        osFreeMem(encoded);
        return ERROR_OUT_OF_MEMORY;
    }
    cJSON_AddNumberToObject(entry, "sequence", (double)++capture->sequence);
    cJSON_AddNumberToObject(entry, "timestamp_ms", (double)time(NULL) * 1000.0);
    cJSON_AddStringToObject(entry, "direction", direction);
    cJSON_AddStringToObject(entry, "data_base64", encoded);
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
    osFreeMem(encoded);

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

static void tb2_mqtt_status_tunneling(void)
{
    osAcquireMutex(&mqtt_passthrough_status.mutex);
    osStrcpy(mqtt_passthrough_status.state, "tunneling");
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
                 mqtt_passthrough_status.active_sessions > 0 ? "tunneling" : "armed");
    }
    else if (success)
    {
        mqtt_passthrough_status.last_success = time(NULL);
        mqtt_passthrough_status.error_code[0] = '\0';
        osStrcpy(mqtt_passthrough_status.state,
                 mqtt_passthrough_status.active_sessions > 0 ? "tunneling" : "armed");
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

static error_t tb2_mqtt_record_packet(tb2_mqtt_passthrough_session_t *session,
                                      bool_t box_to_upstream, const uint8_t *data,
                                      size_t length, uint8_t packet_type, const char *topic,
                                      bool_t forwarded, const char *filter_id, bool_t generated,
                                      bool_t packet_complete)
{
    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    error_t error = tb2_mqtt_capture_packet(&session->capture, direction, data, length,
                                            packet_type, topic, forwarded, filter_id,
                                            generated, packet_complete);
    if (error)
    {
        tb2_mqtt_trace_error("capture_write", error);
        return ERROR_WRITE_FAILED;
    }

    if (!forwarded)
    {
        if (box_to_upstream)
            session->capture.messages_blocked_box_to_upstream++;
        else
            session->capture.messages_blocked_upstream_to_box++;
        tb2_mqtt_status_add_message(box_to_upstream, TRUE);
        return NO_ERROR;
    }

    TlsContext *destination = box_to_upstream ? session->upstream.tlsContext : session->box_tls;
    error = tb2_mqtt_tls_write_all(destination, data, length);
    if (error)
    {
        return error;
    }
    if (box_to_upstream)
    {
        session->capture.bytes_box_to_upstream += length;
        session->capture.messages_forwarded_box_to_upstream++;
    }
    else
    {
        session->capture.bytes_upstream_to_box += length;
        session->capture.messages_forwarded_upstream_to_box++;
    }
    tb2_mqtt_status_add_bytes(box_to_upstream, length);
    tb2_mqtt_status_add_message(box_to_upstream, FALSE);
    return NO_ERROR;
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

static error_t tb2_mqtt_qos2_begin(tb2_mqtt_qos2_entry_t **list, uint16_t packet_id)
{
    tb2_mqtt_qos2_entry_t *existing = tb2_mqtt_qos2_find(*list, packet_id);
    if (existing != NULL)
    {
        existing->completed = FALSE;
        return NO_ERROR;
    }
    tb2_mqtt_qos2_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL)
        return ERROR_OUT_OF_MEMORY;
    entry->packet_id = packet_id;
    entry->completed = FALSE;
    entry->next = *list;
    *list = entry;
    return NO_ERROR;
}

static bool_t tb2_mqtt_qos2_complete(tb2_mqtt_qos2_entry_t *list, uint16_t packet_id)
{
    tb2_mqtt_qos2_entry_t *entry = tb2_mqtt_qos2_find(list, packet_id);
    if (entry == NULL)
        return FALSE;
    entry->completed = TRUE;
    return TRUE;
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

static error_t tb2_mqtt_send_generated_ack(tb2_mqtt_passthrough_session_t *session,
                                           bool_t source_box_to_upstream, uint8_t type,
                                           uint16_t packet_id)
{
    uint8_t packet[] = {(uint8_t)(type << 4), 0x02U,
                        (uint8_t)(packet_id >> 8), (uint8_t)packet_id};
    bool_t ack_box_to_upstream = !source_box_to_upstream;
    TRACE_DEBUG("TB2 MQTT proxy generated=%s direction=%s packet_id=%u\r\n",
                tb2_mqtt_packet_type_name(type),
                ack_box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                (unsigned)packet_id);
    return tb2_mqtt_record_packet(session, ack_box_to_upstream, packet, sizeof(packet),
                                  type, NULL, TRUE, NULL, TRUE, TRUE);
}

static error_t tb2_mqtt_parse_publish(const uint8_t *packet, size_t packet_size,
                                      size_t fixed_header_size, char **topic,
                                      const uint8_t **payload, size_t *payload_len,
                                      uint8_t *qos, uint16_t *packet_id)
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
    if (*qos > 0)
    {
        if (packet_size - offset < 2)
        {
            osFreeMem(*topic);
            *topic = NULL;
            return ERROR_INVALID_LENGTH;
        }
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

static error_t tb2_mqtt_process_packet(tb2_mqtt_passthrough_session_t *session,
                                       bool_t box_to_upstream, const uint8_t *packet,
                                       size_t packet_size, size_t fixed_header_size)
{
    uint8_t type = packet[0] >> 4;
    if (type == TB2_MQTT_PACKET_PUBREL)
    {
        if (packet_size - fixed_header_size < 2)
            return ERROR_INVALID_LENGTH;
        uint16_t packet_id = ((uint16_t)packet[fixed_header_size] << 8) |
                             packet[fixed_header_size + 1];
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        if (tb2_mqtt_qos2_complete(*list, packet_id))
        {
            error_t error = tb2_mqtt_record_packet(session, box_to_upstream, packet,
                                                   packet_size, type, NULL, FALSE,
                                                   "qos2_blocked_publish",
                                                   FALSE, TRUE);
            return error ? error : tb2_mqtt_send_generated_ack(session, box_to_upstream,
                                                                7U, packet_id);
        }
    }

    if (type != TB2_MQTT_PACKET_PUBLISH)
    {
        TRACE_DEBUG("TB2 MQTT proxy direction=%s packet_type=%s action=forward bytes=%" PRIuSIZE "\r\n",
                    box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                    tb2_mqtt_packet_type_name(type), packet_size);
        return tb2_mqtt_record_packet(session, box_to_upstream, packet, packet_size,
                                      type, NULL, TRUE, NULL, FALSE, TRUE);
    }

    char *topic = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t qos = 0;
    uint16_t packet_id = 0;
    error_t error = tb2_mqtt_parse_publish(packet, packet_size, fixed_header_size,
                                           &topic, &payload, &payload_len, &qos, &packet_id);
    if (error)
        return error;

    if (session->observer != NULL)
    {
        session->observer(session->observer_context, box_to_upstream, topic,
                          payload, payload_len, qos);
    }
    const char *filter_id = NULL;
    bool_t blocked = mqtt_forward_filter_should_block(session->box_settings, topic,
                                                       payload, payload_len, &filter_id);
    TRACE_DEBUG("TB2 MQTT proxy direction=%s packet_type=PUBLISH topic='%s' qos=%u"
                " packet_id=%u action=%s filter=%s payload_len=%" PRIuSIZE "\r\n",
                box_to_upstream ? "box_to_upstream" : "upstream_to_box",
                topic, (unsigned)qos, (unsigned)packet_id,
                blocked ? "block" : "forward", filter_id != NULL ? filter_id : "-",
                payload_len);

    error = tb2_mqtt_record_packet(session, box_to_upstream, packet, packet_size,
                                   type, topic, !blocked, filter_id, FALSE, TRUE);
    if (!error && blocked && qos == 1)
    {
        error = tb2_mqtt_send_generated_ack(session, box_to_upstream, 4U, packet_id);
    }
    else if (!error && blocked && qos == 2)
    {
        tb2_mqtt_qos2_entry_t **list = tb2_mqtt_qos2_list(session, box_to_upstream);
        error = tb2_mqtt_qos2_begin(list, packet_id);
        if (!error)
            error = tb2_mqtt_send_generated_ack(session, box_to_upstream, 5U, packet_id);
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
        error = tb2_mqtt_process_packet(session, box_to_upstream, stream->data,
                                        packet_size, fixed_header_size);
        if (error)
            return error;
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
    TlsContext *source = box_to_upstream ? session->box_tls : session->upstream.tlsContext;
    Socket *source_socket = box_to_upstream ? session->box_socket : session->upstream.socket;
    if (!tlsIsRxReady(source) &&
        (tcpWaitForEvents(source_socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY) == 0)
        return NO_ERROR;

    uint8_t buffer[TB2_MQTT_TUNNEL_BUFFER_SIZE];
    size_t received = 0;
    error_t error = tlsRead(source, buffer, sizeof(buffer), &received, 0);
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

bool_t tb2_mqtt_passthrough_is_armed(void)
{
    settings_t *settings = get_settings();
    return settings->mqtt_client_upstream.enabled &&
           settings->mqtt_client_upstream.passthrough_enabled;
}

error_t tb2_mqtt_passthrough_start(TlsContext *box_tls, Socket *box_socket,
                                   tb2_mqtt_passthrough_session_t **session,
                                   bool_t *handled,
                                   tb2_mqtt_publish_observer_t observer,
                                   void *observer_context,
                                   settings_t **box_settings_out)
{
    *session = NULL;
    *handled = FALSE;
    if (box_settings_out != NULL)
        *box_settings_out = NULL;
    if (!tb2_mqtt_passthrough_is_armed())
    {
        return NO_ERROR;
    }
    *handled = TRUE;
    TRACE_DEBUG("TB2 MQTT upstream stage=passthrough_start armed=true\r\n");

    settings_t *box_settings = tb2_mqtt_settings_from_certificate(box_tls);
    settings_t *identity_settings = tb2_mqtt_select_identity_settings(box_settings);
    if (box_settings == NULL || !tb2_mqtt_has_original_identity(identity_settings))
    {
        TRACE_ERROR("TB2 MQTT upstream stage=map_box_identity identity_material"
                    " settings=%s ca=%s certificate=%s key=%s\r\n",
                    box_settings != NULL ? "available" : "unavailable",
                    identity_settings != NULL && identity_settings->internal.client_tb2.ca != NULL &&
                            identity_settings->internal.client_tb2.ca[0] != '\0' ? "available" : "missing",
                    identity_settings != NULL && identity_settings->internal.client_tb2.crt != NULL &&
                            identity_settings->internal.client_tb2.crt[0] != '\0' ? "available" : "missing",
                    identity_settings != NULL && identity_settings->internal.client_tb2.key != NULL &&
                            identity_settings->internal.client_tb2.key[0] != '\0' ? "available" : "missing");
        tb2_mqtt_trace_error("map_box_identity", ERROR_FAILURE);
        tb2_mqtt_status_attempt_failed("identity_unavailable");
        return ERROR_FAILURE;
    }

    tb2_mqtt_passthrough_session_t *created = osAllocMem(sizeof(*created));
    if (created == NULL)
    {
        tb2_mqtt_status_attempt_failed("out_of_memory");
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(created, 0, sizeof(*created));
    created->box_tls = box_tls;
    created->box_socket = box_socket;
    created->box_settings = box_settings;
    created->observer = observer;
    created->observer_context = observer_context;

    error_t error = tb2_mqtt_capture_open(&created->capture, get_settings());
    if (error)
    {
        tb2_mqtt_trace_error("capture_open", error);
        tb2_mqtt_status_attempt_failed("capture_open_failed");
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

    socketSetTimeout(box_socket, TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
    tb2_mqtt_status_tunneling();
    TRACE_DEBUG("TB2 MQTT upstream stage=passthrough_start tunneling=true session=%s\r\n",
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
    if (session == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (!tb2_mqtt_passthrough_is_armed())
    {
        return ERROR_ABORTED;
    }

    error_t error = tb2_mqtt_forward_ready(session, TRUE);
    if (!error)
    {
        error = tb2_mqtt_forward_ready(session, FALSE);
    }
    return error;
}

void tb2_mqtt_passthrough_close(tb2_mqtt_passthrough_session_t *session,
                                const char *result_code, bool_t success)
{
    if (session == NULL)
    {
        return;
    }
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
    else if (!settings->mqtt_client_upstream.passthrough_enabled)
    {
        state = "standby";
    }
    else if (mqtt_passthrough_status.active_sessions == 0 &&
             mqtt_passthrough_status.error_code[0] == '\0')
    {
        state = "armed";
    }
    else
    {
        state = mqtt_passthrough_status.state;
    }

    cJSON_AddBoolToObject(json, "enabled", settings->mqtt_client_upstream.enabled);
    cJSON_AddBoolToObject(json, "passthrough_enabled",
                         settings->mqtt_client_upstream.passthrough_enabled);
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
