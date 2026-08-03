#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
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
#include "os_ext.h"
#include "os_port.h"
#include "platform.h"
#include "rand.h"
#include "settings.h"
#include "server_helpers.h"
#include "tls.h"
#include "tls_adapter.h"
#include "tb2_https_passthrough.h"

#define TB2_TUNNEL_BUFFER_SIZE 16384
#define TB2_TUNNEL_POLL_MS 20
#define TB2_TUNNEL_IO_TIMEOUT_MS 500
#define TB2_CAPTURE_MIB_BYTES (1024ULL * 1024ULL)

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
} tb2_passthrough_status_t;

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
} tb2_capture_t;

static tb2_passthrough_status_t passthrough_status;

static void tb2_set_private_permissions(const char *path, bool_t directory)
{
#ifdef _WIN32
    (void)directory;
    _chmod(path, _S_IREAD | _S_IWRITE);
#else
    chmod(path, directory ? 0700 : 0600);
#endif
}

static void tb2_format_utc(time_t timestamp, char *output, size_t output_size)
{
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    strftime(output, output_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static char *tb2_resolve_capture_root(settings_t *settings)
{
    char *resolved = osAllocMem(512);
    if (resolved == NULL)
    {
        return NULL;
    }
    resolved[0] = '\0';
    settings_resolve_dir(&resolved, settings->cloud.tb2_capture_dir, settings->internal.basedirfull);
    return resolved;
}

static settings_t *tb2_settings_from_certificate(HttpConnection *connection)
{
    if (connection == NULL || connection->tlsContext == NULL)
    {
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: TLS context unavailable\r\n");
        return NULL;
    }

    const char *subject = connection->tlsContext->client_cert_subject;
    const char *issuer = connection->tlsContext->client_cert_issuer;
    if (subject == NULL || issuer == NULL || subject[0] == '\0' || issuer[0] == '\0')
    {
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: client certificate identity unavailable\r\n");
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
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: unsupported client certificate subject format\r\n");
        return NULL;
    }
    osStringToLower(common_name);

    uint8_t overlay_id = get_overlay_id(common_name);
    bool_t tb2_issuer = osStrstr(issuer, "Toniebox Root CA") != NULL;
    settings_t *settings = overlay_id > 0 ? get_settings_id(overlay_id) : NULL;

    if (settings == NULL && tb2_issuer && get_settings()->core.allowNewBox)
    {
        settings = get_settings_cn(common_name);
    }
    if (settings == NULL || !settings->internal.config_used)
    {
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: client certificate is not mapped to an active overlay\r\n");
        return NULL;
    }

    if (settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        if (!tb2_issuer)
        {
            TRACE_DEBUG("TB2 HTTPS passthrough skipped: mapped overlay is not TB2 and issuer is not the TB2 root\r\n");
            return NULL;
        }
        if (!settings_set_unsigned_id("toniebox.boxGeneration", GENERATION_TB2,
                                      settings->internal.overlayNumber))
        {
            TRACE_DEBUG("TB2 HTTPS passthrough skipped: failed to mark mapped overlay as TB2\r\n");
            return NULL;
        }
        settings_try_load_certs_id(settings->internal.overlayNumber);
    }

    return settings;
}

static bool_t tb2_has_original_identity(settings_t *settings)
{
    return settings != NULL && settings->internal.client.ca != NULL &&
           settings->internal.client.crt != NULL && settings->internal.client.key != NULL &&
           settings->internal.client.ca[0] != '\0' && settings->internal.client.crt[0] != '\0' &&
           settings->internal.client.key[0] != '\0';
}

static error_t tb2_outbound_tls_init(HttpClientContext *context, TlsContext *tls_context)
{
    settings_t *settings = (settings_t *)context->sourceCtx;
    if (!tb2_has_original_identity(settings))
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

static error_t tb2_connect_upstream(settings_t *box_settings, HttpClientContext *upstream)
{
    settings_t *global = get_settings();
    error_t error;
    upstream->serverName = global->cloud.remote_hostname_tb2;
    upstream->sourceCtx = box_settings;
    error = httpClientSetTimeout(upstream, global->core.http_client_timeout);
    if (!error)
    {
        error = httpClientRegisterTlsInitCallback(upstream, tb2_outbound_tls_init);
    }
    if (error)
    {
        return error;
    }

    void *resolver = resolve_host(global->cloud.remote_hostname_tb2);
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
        error = httpClientConnect(upstream, &address, (uint16_t)global->cloud.remote_port_tb2);
        if (!error)
        {
            break;
        }
    }
    resolve_free(resolver);

    if (!error && upstream->socket != NULL)
    {
        socketSetTimeout(upstream->socket, TB2_TUNNEL_IO_TIMEOUT_MS);
    }
    return error;
}

static error_t tb2_capture_open(tb2_capture_t *capture, settings_t *settings)
{
    osMemset(capture, 0, sizeof(*capture));
    capture->started_at = time(NULL);

    char *root = tb2_resolve_capture_root(settings);
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
    tb2_set_private_permissions(root, TRUE);

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
        osAcquireMutex(&passthrough_status.mutex);
        uint32_t counter = ++passthrough_status.session_counter;
        osReleaseMutex(&passthrough_status.mutex);

        osSnprintf(capture->session_id, sizeof(capture->session_id), "%s-%08u", compact_time,
                   (unsigned)counter);
        osSnprintf(capture->directory, sizeof(capture->directory), "%s%c%s", root, PATH_SEPARATOR,
                   capture->session_id);
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

    tb2_set_private_permissions(capture->directory, TRUE);

    capture->traffic = fsOpenFile(capture->traffic_path,
                                  FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    if (capture->traffic == NULL)
    {
        return ERROR_OPEN_FAILED;
    }
    tb2_set_private_permissions(capture->traffic_path, FALSE);
    return NO_ERROR;
}

static error_t tb2_capture_chunk(tb2_capture_t *capture, const char *direction,
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

static error_t tb2_capture_finish(tb2_capture_t *capture, settings_t *settings,
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
    tb2_format_utc(capture->started_at, started, sizeof(started));
    tb2_format_utc(time(NULL), finished, sizeof(finished));
    cJSON_AddStringToObject(session, "session_id", capture->session_id);
    cJSON_AddStringToObject(session, "started_at", started);
    cJSON_AddStringToObject(session, "finished_at", finished);
    cJSON_AddStringToObject(session, "hostname", settings->cloud.remote_hostname_tb2);
    cJSON_AddNumberToObject(session, "port", settings->cloud.remote_port_tb2);
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

    if (!error)
    {
        error = fsWriteFile(file, json, osStrlen(json));
    }
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
    tb2_set_private_permissions(capture->session_path, FALSE);
    return error;
}

static void tb2_rotate_completed_captures(settings_t *settings)
{
    char *root = tb2_resolve_capture_root(settings);
    if (root == NULL)
    {
        return;
    }

    uint64_t limit = (uint64_t)settings->cloud.tb2_capture_max_mib * TB2_CAPTURE_MIB_BYTES;
    uint64_t total = 0;
    FsDir *dir = fsOpenDir(root);
    FsDirEntry entry;
    if (dir != NULL)
    {
        while (fsReadDir(dir, &entry) == NO_ERROR)
        {
            if ((entry.attributes & FS_FILE_ATTR_DIRECTORY) == 0 || entry.name[0] == '.')
            {
                continue;
            }
            char session_path[576];
            char traffic_path[576];
            osSnprintf(session_path, sizeof(session_path), "%s%c%s%csession.json", root,
                       PATH_SEPARATOR, entry.name, PATH_SEPARATOR);
            if (!fsFileExists(session_path))
            {
                continue;
            }
            osSnprintf(traffic_path, sizeof(traffic_path), "%s%c%s%ctraffic.jsonl", root,
                       PATH_SEPARATOR, entry.name, PATH_SEPARATOR);
            uint32_t size = 0;
            if (fsGetFileSize(session_path, &size) == NO_ERROR)
            {
                total += size;
            }
            if (fsGetFileSize(traffic_path, &size) == NO_ERROR)
            {
                total += size;
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
            char session_path[576];
            char traffic_path[576];
            osSnprintf(session_path, sizeof(session_path), "%s%c%s%csession.json", root,
                       PATH_SEPARATOR, entry.name, PATH_SEPARATOR);
            if (!fsFileExists(session_path))
            {
                continue;
            }
            time_t modified = convertDateToUnixTime(&entry.modified);
            if (oldest_name[0] == '\0' || modified < oldest_time)
            {
                uint32_t size = 0;
                oldest_time = modified;
                osMemcpy(oldest_name, entry.name, sizeof(oldest_name));
                oldest_name[sizeof(oldest_name) - 1] = '\0';
                oldest_size = 0;
                if (fsGetFileSize(session_path, &size) == NO_ERROR)
                {
                    oldest_size += size;
                }
                osSnprintf(traffic_path, sizeof(traffic_path), "%s%c%s%ctraffic.jsonl", root,
                           PATH_SEPARATOR, entry.name, PATH_SEPARATOR);
                if (fsGetFileSize(traffic_path, &size) == NO_ERROR)
                {
                    oldest_size += size;
                }
            }
        }
        fsCloseDir(dir);

        if (oldest_name[0] == '\0')
        {
            break;
        }
        char session_path[1024];
        char traffic_path[1024];
        char session_dir[800];
        osSnprintf(session_dir, sizeof(session_dir), "%s%c%s", root, PATH_SEPARATOR, oldest_name);
        osSnprintf(session_path, sizeof(session_path), "%s%csession.json", session_dir, PATH_SEPARATOR);
        osSnprintf(traffic_path, sizeof(traffic_path), "%s%ctraffic.jsonl", session_dir, PATH_SEPARATOR);
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

static void tb2_status_start(const char *state)
{
    osAcquireMutex(&passthrough_status.mutex);
    passthrough_status.active_sessions++;
    passthrough_status.last_attempt = time(NULL);
    osStrncpy(passthrough_status.state, state, sizeof(passthrough_status.state) - 1);
    passthrough_status.error_code[0] = '\0';
    osReleaseMutex(&passthrough_status.mutex);
}

static void tb2_status_set_state(const char *state)
{
    osAcquireMutex(&passthrough_status.mutex);
    osStrncpy(passthrough_status.state, state, sizeof(passthrough_status.state) - 1);
    osReleaseMutex(&passthrough_status.mutex);
}

static void tb2_status_add_bytes(bool_t box_to_upstream, size_t length)
{
    osAcquireMutex(&passthrough_status.mutex);
    if (box_to_upstream)
    {
        passthrough_status.bytes_box_to_upstream += length;
    }
    else
    {
        passthrough_status.bytes_upstream_to_box += length;
    }
    osReleaseMutex(&passthrough_status.mutex);
}

static void tb2_status_finish(bool_t success, const char *error_code)
{
    osAcquireMutex(&passthrough_status.mutex);
    if (passthrough_status.active_sessions > 0)
    {
        passthrough_status.active_sessions--;
    }
    if (success)
    {
        passthrough_status.last_success = time(NULL);
        passthrough_status.error_code[0] = '\0';
        osStrncpy(passthrough_status.state,
                  passthrough_status.active_sessions > 0 ? "tunneling" : "online",
                  sizeof(passthrough_status.state) - 1);
    }
    else
    {
        osStrncpy(passthrough_status.error_code, error_code,
                  sizeof(passthrough_status.error_code) - 1);
        if (passthrough_status.active_sessions == 0)
        {
            osStrncpy(passthrough_status.state, "error", sizeof(passthrough_status.state) - 1);
        }
    }
    osReleaseMutex(&passthrough_status.mutex);
}

static error_t tb2_tls_write_all(TlsContext *destination, const uint8_t *data, size_t length)
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

static error_t tb2_forward_ready(TlsContext *source, Socket *source_socket,
                                 TlsContext *destination, tb2_capture_t *capture,
                                 bool_t box_to_upstream, bool_t *progress)
{
    if (!tlsIsRxReady(source) &&
        (tcpWaitForEvents(source_socket, SOCKET_EVENT_RX_READY, TB2_TUNNEL_POLL_MS) &
         SOCKET_EVENT_RX_READY) == 0)
    {
        return NO_ERROR;
    }

    uint8_t buffer[TB2_TUNNEL_BUFFER_SIZE];
    size_t received = 0;
    error_t error = tlsRead(source, buffer, sizeof(buffer), &received, 0);
    if (error)
    {
        return error;
    }
    if (received == 0)
    {
        return ERROR_END_OF_STREAM;
    }

    const char *direction = box_to_upstream ? "box_to_upstream" : "upstream_to_box";
    error = tb2_capture_chunk(capture, direction, buffer, received);
    if (error)
    {
        return ERROR_WRITE_FAILED;
    }
    error = tb2_tls_write_all(destination, buffer, received);
    if (error)
    {
        return error;
    }

    if (box_to_upstream)
    {
        capture->bytes_box_to_upstream += received;
    }
    else
    {
        capture->bytes_upstream_to_box += received;
    }
    tb2_status_add_bytes(box_to_upstream, received);
    *progress = TRUE;
    return NO_ERROR;
}

error_t tb2_https_passthrough_init(void)
{
    osMemset(&passthrough_status, 0, sizeof(passthrough_status));
    if (!osCreateMutex(&passthrough_status.mutex))
    {
        return ERROR_OUT_OF_RESOURCES;
    }
    passthrough_status.initialized = TRUE;
    osStrcpy(passthrough_status.state, "disabled");
    return NO_ERROR;
}

void tb2_https_passthrough_deinit(void)
{
    if (passthrough_status.initialized)
    {
        osDeleteMutex(&passthrough_status.mutex);
        passthrough_status.initialized = FALSE;
    }
}

error_t tb2_https_passthrough_post_tls(HttpConnection *connection, bool_t *handled)
{
    *handled = FALSE;
    settings_t *global = get_settings();
    if (!global->cloud.tb2_enabled)
    {
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: forwarding is disabled\r\n");
        return NO_ERROR;
    }

    settings_t *box_settings = tb2_settings_from_certificate(connection);
    if (box_settings == NULL)
    {
        TRACE_DEBUG("TB2 HTTPS passthrough skipped: no eligible TB2 overlay resolved\r\n");
        return NO_ERROR;
    }
    *handled = TRUE;

    tb2_capture_t capture;
    error_t error = tb2_capture_open(&capture, global);
    if (error)
    {
        tb2_status_start("error");
        tb2_status_finish(FALSE, "capture_open_failed");
        return error;
    }

    tb2_status_start("connecting");
    HttpClientContext upstream;
    osMemset(&upstream, 0, sizeof(upstream));

    const char *result_code = "connect_failed";
    bool_t upstream_initialized = FALSE;
    error = httpClientInit(&upstream);
    if (!error)
    {
        upstream_initialized = TRUE;
        error = tb2_connect_upstream(box_settings, &upstream);
    }
    if (!error)
    {
        socketSetTimeout(connection->socket, TB2_TUNNEL_IO_TIMEOUT_MS);
        tb2_status_set_state("tunneling");
        result_code = "stream_failed";

        while (!error)
        {
            bool_t progress = FALSE;
            error = tb2_forward_ready(connection->tlsContext, connection->socket,
                                      upstream.tlsContext, &capture, TRUE, &progress);
            if (!error)
            {
                error = tb2_forward_ready(upstream.tlsContext, upstream.socket,
                                          connection->tlsContext, &capture, FALSE, &progress);
            }
            if (!error && !progress)
            {
                osDelayTask(1);
            }
        }
    }

    bool_t success = error == ERROR_END_OF_STREAM;
    if (success)
    {
        result_code = "completed";
    }
    else if (error == ERROR_WRITE_FAILED)
    {
        result_code = "capture_or_write_failed";
    }

    if (upstream_initialized)
    {
        httpClientDeinit(&upstream);
    }
    error_t capture_finish_error = tb2_capture_finish(&capture, global, result_code);
    if (capture_finish_error)
    {
        success = FALSE;
        error = capture_finish_error;
        result_code = "capture_finalize_failed";
    }
    tb2_rotate_completed_captures(global);
    tb2_status_finish(success, result_code);

    TRACE_INFO("TB2 HTTPS passthrough session=%s status=%s up=%llu down=%llu\r\n",
               capture.session_id, result_code,
               (unsigned long long)capture.bytes_box_to_upstream,
               (unsigned long long)capture.bytes_upstream_to_box);
    return success ? NO_ERROR : error;
}

error_t tb2_https_passthrough_write_status(HttpConnection *connection)
{
    settings_t *settings = get_settings();
    cJSON *json = cJSON_CreateObject();
    if (json == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    osAcquireMutex(&passthrough_status.mutex);
    cJSON_AddBoolToObject(json, "enabled", settings->cloud.tb2_enabled);
    cJSON_AddStringToObject(json, "state",
                           settings->cloud.tb2_enabled ? passthrough_status.state : "disabled");
    cJSON_AddStringToObject(json, "hostname", settings->cloud.remote_hostname_tb2);
    cJSON_AddNumberToObject(json, "port", settings->cloud.remote_port_tb2);
    cJSON_AddNumberToObject(json, "bytes_box_to_upstream",
                           (double)passthrough_status.bytes_box_to_upstream);
    cJSON_AddNumberToObject(json, "bytes_upstream_to_box",
                           (double)passthrough_status.bytes_upstream_to_box);
    cJSON_AddNumberToObject(json, "last_attempt", (double)passthrough_status.last_attempt);
    cJSON_AddNumberToObject(json, "last_success", (double)passthrough_status.last_success);
    cJSON_AddStringToObject(json, "error_code", passthrough_status.error_code);
    osReleaseMutex(&passthrough_status.mutex);

    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    httpPrepareHeader(connection, "application/json; charset=utf-8", osStrlen(body));
    return httpWriteResponse(connection, body, connection->response.contentLength, true);
}
