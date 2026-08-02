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
    time_t started_at;
} tb2_mqtt_capture_t;

struct tb2_mqtt_passthrough_session
{
    TlsContext *box_tls;
    Socket *box_socket;
    HttpClientContext upstream;
    bool_t upstream_initialized;
    tb2_mqtt_capture_t capture;
    bool_t capture_opened;
};

static tb2_mqtt_passthrough_status_t mqtt_passthrough_status;

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
        return NULL;
    }

    const char *subject = tls_context->client_cert_subject;
    const char *issuer = tls_context->client_cert_issuer;
    if (subject == NULL || issuer == NULL || subject[0] == '\0' || issuer[0] == '\0')
    {
        return NULL;
    }

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
        return NULL;
    }
    osStringToLower(common_name);

    uint8_t overlay_id = get_overlay_id(common_name);
    bool_t trusted_issuer = osStrstr(issuer, "Toniebox Root CA") != NULL ||
                            osStrstr(issuer, "Toniebox SubCA") != NULL ||
                            osStrstr(issuer, "Boxine Factory SubCA") != NULL;
    settings_t *settings = overlay_id > 0 ? get_settings_id(overlay_id) : NULL;
    if (settings == NULL || !settings->internal.config_used || !trusted_issuer)
    {
        return NULL;
    }
    if (settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        return NULL;
    }
    return settings;
}

static bool_t tb2_mqtt_has_original_identity(settings_t *settings)
{
    return settings != NULL && settings->internal.client.ca != NULL &&
           settings->internal.client.crt != NULL && settings->internal.client.key != NULL &&
           settings->internal.client.ca[0] != '\0' && settings->internal.client.crt[0] != '\0' &&
           settings->internal.client.key[0] != '\0';
}

static error_t tb2_mqtt_outbound_tls_init(HttpClientContext *context, TlsContext *tls_context)
{
    settings_t *settings = (settings_t *)context->sourceCtx;
    if (!tb2_mqtt_has_original_identity(settings))
    {
        return ERROR_FAILURE;
    }

    error_t error = tlsSetPrng(tls_context, rand_get_algo(), rand_get_context());
    if (!error)
    {
        error = tlsSetTrustedCaList(tls_context, settings->internal.client.ca,
                                    osStrlen(settings->internal.client.ca));
    }
    if (!error)
    {
        error = tlsAddCertificate(tls_context, settings->internal.client.crt,
                                  osStrlen(settings->internal.client.crt),
                                  settings->internal.client.key,
                                  osStrlen(settings->internal.client.key));
    }
    if (!error)
    {
        error = tlsSetServerName(tls_context, context->serverName);
    }
    if (!error)
    {
        tls_context_key_log_init(tls_context);
    }
    return error;
}

static error_t tb2_mqtt_connect_upstream(settings_t *box_settings, HttpClientContext *upstream)
{
    settings_t *global = get_settings();
    upstream->serverName = global->mqtt_client_upstream.hostname;
    upstream->sourceCtx = box_settings;

    error_t error = httpClientSetTimeout(upstream, global->core.http_client_timeout);
    if (!error)
    {
        error = httpClientRegisterTlsInitCallback(upstream, tb2_mqtt_outbound_tls_init);
    }
    if (error)
    {
        return error;
    }

    void *resolver = resolve_host(global->mqtt_client_upstream.hostname);
    if (resolver == NULL)
    {
        return ERROR_ADDRESS_NOT_FOUND;
    }

    error = ERROR_ADDRESS_NOT_FOUND;
    for (int position = 0;; position++)
    {
        IpAddr address;
        if (!resolve_get_ip(resolver, position, &address))
        {
            break;
        }
        error = httpClientConnect(upstream, &address,
                                  (uint16_t)global->mqtt_client_upstream.port);
        if (!error)
        {
            break;
        }
    }
    resolve_free(resolver);

    if (!error && upstream->socket != NULL)
    {
        socketSetTimeout(upstream->socket, TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
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

static error_t tb2_mqtt_capture_chunk(tb2_mqtt_capture_t *capture, const char *direction,
                                      const uint8_t *data, size_t length)
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
            return error;
        }
        if (written == 0)
        {
            return ERROR_WRITE_FAILED;
        }
        offset += written;
    }
    return NO_ERROR;
}

static error_t tb2_mqtt_forward_bytes(tb2_mqtt_passthrough_session_t *session,
                                      bool_t box_to_upstream, const uint8_t *data,
                                      size_t length)
{
    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    error_t error = tb2_mqtt_capture_chunk(&session->capture, direction, data, length);
    if (error)
    {
        return ERROR_WRITE_FAILED;
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
    }
    else
    {
        session->capture.bytes_upstream_to_box += length;
    }
    tb2_mqtt_status_add_bytes(box_to_upstream, length);
    return NO_ERROR;
}

static error_t tb2_mqtt_forward_ready(tb2_mqtt_passthrough_session_t *session,
                                      bool_t box_to_upstream)
{
    TlsContext *source = box_to_upstream ? session->box_tls : session->upstream.tlsContext;
    Socket *source_socket = box_to_upstream ? session->box_socket : session->upstream.socket;
    if (!tlsIsRxReady(source) &&
        (tcpWaitForEvents(source_socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY) == 0)
    {
        return NO_ERROR;
    }

    uint8_t buffer[TB2_MQTT_TUNNEL_BUFFER_SIZE];
    size_t received = 0;
    error_t error = tlsRead(source, buffer, sizeof(buffer), &received, 0);
    if (error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT)
    {
        return NO_ERROR;
    }
    if (error)
    {
        return error;
    }
    if (received == 0)
    {
        return ERROR_END_OF_STREAM;
    }
    return tb2_mqtt_forward_bytes(session, box_to_upstream, buffer, received);
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
                                   bool_t *handled)
{
    *session = NULL;
    *handled = FALSE;
    if (!tb2_mqtt_passthrough_is_armed())
    {
        return NO_ERROR;
    }
    *handled = TRUE;

    settings_t *box_settings = tb2_mqtt_settings_from_certificate(box_tls);
    if (!tb2_mqtt_has_original_identity(box_settings))
    {
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

    error_t error = tb2_mqtt_capture_open(&created->capture, get_settings());
    if (error)
    {
        tb2_mqtt_status_attempt_failed("capture_open_failed");
        osFreeMem(created);
        return error;
    }
    created->capture_opened = TRUE;
    tb2_mqtt_status_start();

    error = httpClientInit(&created->upstream);
    if (!error)
    {
        created->upstream_initialized = TRUE;
        error = tb2_mqtt_connect_upstream(box_settings, &created->upstream);
    }
    if (error)
    {
        tb2_mqtt_passthrough_close(created, "connect_failed", FALSE);
        return error;
    }

    socketSetTimeout(box_socket, TB2_MQTT_TUNNEL_IO_TIMEOUT_MS);
    tb2_mqtt_status_tunneling();
    *session = created;
    return NO_ERROR;
}

error_t tb2_mqtt_passthrough_forward_initial(tb2_mqtt_passthrough_session_t *session,
                                             const uint8_t *data, size_t length)
{
    if (session == NULL || data == NULL || length == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }
    return tb2_mqtt_forward_bytes(session, TRUE, data, length);
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
