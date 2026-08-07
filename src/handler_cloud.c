#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "settings.h"
#include "fs_ext.h"

#include "handler.h"
#include "handler_api.h"
#include "handler_cloud.h"
#include "http/http_client.h"

#include "mqtt.h"
#include "mqtt_server.h"
#include "server_helpers.h"
#include "mutex_manager.h"

#include "toniefile.h"
#include "toniesJson.h"
#include "tonie_audio_playlist_internal.h"
#include "tb2_client_identity.h"
#include "tb2_ruid.h"
#include "v3_local_content.h"
#include "v3_native_cache.h"
#include "cJSON.h"

#include <byteswap.h>

#define TAP_LIVE_READY_BYTES (2U * TONIEFILE_FRAME_SIZE)
#define TAP_LIVE_READY_WAIT_MS 3000U
#define TAP_LIVE_READY_POLL_MS 100U
#define TAP_TARGET_LOCK_COUNT 8U
#define TAP_TARGET_LOCK_WAIT_MS 30000U
#define TAP_TARGET_LOCK_POLL_MS 100U
#define TAP_GENERATOR_START_WAIT_MS 10000U
#define TAP_GENERATOR_START_POLL_MS 100U

typedef struct
{
    bool_t locked;
    char *target_path;
} tap_target_lock_t;

static tap_target_lock_t tap_target_locks[TAP_TARGET_LOCK_COUNT];

static bool_t tap_is_stable_cached_playlist(tonie_info_t *tonieInfo);
static void freshness_clear_cache_after_content_request(client_ctx_t *client_ctx, cloudapi_t api, const char *ruid);
static bool_t freshness_source_changed_contains_ruid(settings_t *settings, const char *ruid);

typedef struct
{
    cbr_ctx_t passthrough;
    v3_native_cache_meta_capture_t cache;
    error_t cache_error;
    bool_t finished;
} v3_native_meta_cbr_t;

typedef struct
{
    cbr_ctx_t passthrough;
    v3_native_cache_chapter_capture_t cache;
    bool_t cache_enabled;
    bool_t response_successful;
    bool_t downstream_failed;
    bool_t finished;
    bool_t successful;
    error_t cache_error;
    uint32_t status_code;
} v3_native_chapter_cbr_t;

static void v3_native_import_library_if_enabled(client_ctx_t *client_ctx,
                                                const char *ruid)
{
    if (client_ctx == NULL || client_ctx->settings == NULL || ruid == NULL ||
        !client_ctx->settings->cloud.cacheContentV3 ||
        !v3_native_cache_active_version(
            client_ctx->settings->internal.cachedirfull,
            client_ctx->settings->internal.overlayNumber, ruid, NULL))
    {
        return;
    }

    bool_t tonieplay = v3_native_cache_active_is_tonieplay(
        client_ctx->settings->internal.cachedirfull,
        client_ctx->settings->internal.overlayNumber, ruid);
    if ((tonieplay &&
         !client_ctx->settings->cloud.cacheTonieplayToLibraryV3) ||
        (!tonieplay && !client_ctx->settings->cloud.cacheToLibraryV3))
    {
        return;
    }

    error_t error = tonieplay
                        ? v3_native_cache_import_active_tonieplay_library(
                              client_ctx->settings->internal.cachedirfull,
                              client_ctx->settings->internal.librarydirfull,
                              client_ctx->settings->internal.overlayNumber,
                              ruid)
                        : v3_native_cache_import_active_library(
                              client_ctx->settings->internal.cachedirfull,
                              client_ctx->settings->internal.librarydirfull,
                              client_ctx->settings->internal.overlayNumber,
                              ruid);
    if (error != NO_ERROR)
    {
        TRACE_WARNING("Could not import complete TB2 V3 %s cache into library overlay=%u rUID=%s: %s\r\n",
                      tonieplay ? "Tonieplay" : "audio",
                      (unsigned)client_ctx->settings->internal.overlayNumber,
                      ruid, error2text(error));
    }
}

static void v3_native_meta_response(void *source, HttpClientContext *cloud)
{
    v3_native_meta_cbr_t *context = source;
    v3_native_cache_meta_capture_response(&context->cache, cloud->statusCode);
    if (context->passthrough.connection != NULL)
    {
        cbrCloudResponsePassthrough(&context->passthrough, cloud);
    }
}

static void v3_native_meta_header(void *source, HttpClientContext *cloud,
                                  const char *header, const char *value)
{
    v3_native_meta_cbr_t *context = source;
    if (context->passthrough.connection != NULL)
    {
        cbrCloudHeaderPassthrough(&context->passthrough, cloud, header, value);
    }
}

static void v3_native_meta_body(void *source, HttpClientContext *cloud,
                                const char *payload, size_t length, error_t error)
{
    v3_native_meta_cbr_t *context = source;
    if (length > 0)
    {
        v3_native_cache_meta_capture_append(&context->cache, payload, length);
    }
    if (error == ERROR_END_OF_STREAM && !context->finished)
    {
        context->finished = TRUE;
        context->cache_error = v3_native_cache_meta_capture_finish(&context->cache);
        if (context->cache_error != NO_ERROR)
        {
            TRACE_WARNING("TB2 V3 content-meta was received but not cached: %s\r\n",
                          error2text(context->cache_error));
        }
        else
        {
            v3_native_import_library_if_enabled(
                context->passthrough.client_ctx, context->cache.ruid);
        }
    }
    if (context->passthrough.connection != NULL)
    {
        cbrCloudBodyPassthrough(&context->passthrough, cloud, payload, length, error);
    }
}

static void v3_native_meta_disconnect(void *source, HttpClientContext *cloud)
{
    v3_native_meta_cbr_t *context = source;
    if (context->passthrough.connection != NULL)
    {
        cbrCloudServerDiscoPassthrough(&context->passthrough, cloud);
    }
}

static void v3_native_chapter_response(void *source, HttpClientContext *cloud)
{
    v3_native_chapter_cbr_t *context = source;
    context->status_code = cloud->statusCode;
    context->response_successful = cloud->statusCode == 200 ||
                                   cloud->statusCode == 206;
    if (cloud->statusCode != 200)
    {
        context->cache.failed = TRUE;
    }
    if (context->passthrough.connection != NULL)
    {
        cbrCloudResponsePassthrough(&context->passthrough, cloud);
    }
}

static void v3_native_chapter_header(void *source, HttpClientContext *cloud,
                                     const char *header, const char *value)
{
    v3_native_chapter_cbr_t *context = source;
    if (context->cache_enabled && header != NULL && value != NULL &&
        !osStrcasecmp(header, "Content-Type"))
    {
        v3_native_cache_object_content_type(&context->cache, value);
    }
    if (context->passthrough.connection != NULL)
    {
        cbrCloudHeaderPassthrough(&context->passthrough, cloud, header, value);
    }
}

static void v3_native_chapter_body(void *source, HttpClientContext *cloud,
                                   const char *payload, size_t length, error_t error)
{
    v3_native_chapter_cbr_t *context = source;
    (void)cloud;
    if (context->cache_enabled && length > 0)
    {
        v3_native_cache_chapter_append(&context->cache, payload, length);
    }
    if (context->passthrough.connection != NULL)
    {
        error_t send_error = httpSend(context->passthrough.connection, payload,
                                      length, HTTP_FLAG_DELAY);
        if (send_error != NO_ERROR)
        {
            context->downstream_failed = TRUE;
            TRACE_ERROR("Could not forward TB2 V3 chapter %s to box: %s\r\n",
                        context->cache.name, error2text(send_error));
        }
    }
    if (error == ERROR_END_OF_STREAM && !context->finished)
    {
        context->finished = TRUE;
        context->successful = context->response_successful &&
                              !context->downstream_failed;
        if (context->cache_enabled && !context->cache.failed)
        {
            context->cache_error = v3_native_cache_chapter_finish(&context->cache);
            if (context->cache_error != NO_ERROR)
            {
                TRACE_WARNING("TB2 V3 chapter %s was received but not cached: %s\r\n",
                              context->cache.name,
                              error2text(context->cache_error));
            }
            else
            {
                v3_native_import_library_if_enabled(
                    context->passthrough.client_ctx, context->cache.ruid);
            }
        }
        else if (context->cache_enabled && context->cache.failed)
        {
            context->cache_error = context->response_successful
                                       ? ERROR_WRITE_FAILED
                                       : ERROR_INVALID_RESPONSE;
        }
        else if (!context->response_successful)
        {
            context->cache_error = ERROR_INVALID_RESPONSE;
            TRACE_WARNING("TB2 V3 chapter %s did not complete cleanly: %s\r\n",
                          context->cache.name,
                          error2text(ERROR_INVALID_RESPONSE));
        }
    }
    context->passthrough.status = PROX_STATUS_BODY;
}

static void v3_native_chapter_disconnect(void *source, HttpClientContext *cloud)
{
    v3_native_chapter_cbr_t *context = source;
    if (context->passthrough.connection != NULL)
    {
        cbrCloudServerDiscoPassthrough(&context->passthrough, cloud);
    }
}

/*
 * Cyclone reads stream_max_size from the connection client context. Keep the
 * TAP live-size override local to this request without changing vendored code.
 */
static error_t tap_send_response_stream_with_size(HttpConnection *connection, const char_t *uri, uint32_t stream_max_size)
{
    client_ctx_t saved_client_ctx = connection->private.client_ctx;
    client_ctx_t local_client_ctx = saved_client_ctx;
    settings_t local_settings = *local_client_ctx.settings;

    local_settings.encode.stream_max_size = stream_max_size;
    local_client_ctx.settings = &local_settings;
    connection->private.client_ctx = local_client_ctx;

    error_t error = httpSendResponseStream(connection, uri, true);

    connection->private.client_ctx = saved_client_ctx;
    return error;
}

static uint32_t stream_ctx_next_generation(stream_ctx_t *stream_ctx)
{
    stream_ctx->generation++;
    if (stream_ctx->generation == 0)
    {
        stream_ctx->generation++;
    }

    return stream_ctx->generation;
}

/*
 * Serialize live generation per final TAP target. Different TAP targets can
 * still run in parallel, but two boxes cannot write/read the same .taf.tmp.
 */
static error_t tap_target_lock(const char *target_path)
{
    if (target_path == NULL)
    {
        return ERROR_FAILURE;
    }

    uint32_t waited_ms = 0;
    while (waited_ms < TAP_TARGET_LOCK_WAIT_MS)
    {
        tap_target_lock_t *free_lock = NULL;
        bool_t target_locked = FALSE;

        mutex_lock(MUTEX_TAP_TARGETS);
        for (size_t i = 0; i < TAP_TARGET_LOCK_COUNT; i++)
        {
            tap_target_lock_t *lock = &tap_target_locks[i];

            if (lock->locked && lock->target_path != NULL && osStrcmp(lock->target_path, target_path) == 0)
            {
                target_locked = TRUE;
                break;
            }

            if (!lock->locked && free_lock == NULL)
            {
                free_lock = lock;
            }
        }

        if (!target_locked && free_lock != NULL)
        {
            size_t target_path_len = osStrlen(target_path) + 1;
            char *target_path_copy = osAllocMem(target_path_len);
            if (target_path_copy == NULL)
            {
                mutex_unlock(MUTEX_TAP_TARGETS);
                return ERROR_OUT_OF_MEMORY;
            }

            osMemcpy(target_path_copy, target_path, target_path_len);
            free_lock->locked = TRUE;
            free_lock->target_path = target_path_copy;
            mutex_unlock(MUTEX_TAP_TARGETS);
            return NO_ERROR;
        }

        mutex_unlock(MUTEX_TAP_TARGETS);
        if (waited_ms == 0 || (waited_ms % 5000U) == 0)
        {
            TRACE_VERBOSE("Waiting for TAP target lock %s, waited=%" PRIu32 "ms\r\n", target_path, waited_ms);
        }
        osDelayTask(TAP_TARGET_LOCK_POLL_MS);
        waited_ms += TAP_TARGET_LOCK_POLL_MS;
    }

    TRACE_WARNING("Timeout while waiting for TAP target lock %s after %" PRIu32 "ms\r\n", target_path, waited_ms);
    return ERROR_TIMEOUT;
}

static void tap_target_unlock(const char *target_path)
{
    if (target_path == NULL)
    {
        return;
    }

    mutex_lock(MUTEX_TAP_TARGETS);
    for (size_t i = 0; i < TAP_TARGET_LOCK_COUNT; i++)
    {
        tap_target_lock_t *lock = &tap_target_locks[i];

        if (lock->locked && lock->target_path != NULL && osStrcmp(lock->target_path, target_path) == 0)
        {
            lock->locked = FALSE;
            osFreeMem(lock->target_path);
            lock->target_path = NULL;
            break;
        }
    }
    mutex_unlock(MUTEX_TAP_TARGETS);
}

void convertTokenBytesToString(uint8_t *token, char *msg, bool_t logFullAuth)
{
    char buffer[4];

    msg[0] = '\0';
    for (int i = 0; i < TONIE_AUTH_TOKEN_LENGTH; i++)
    {
        if (i > 3 && !logFullAuth)
        {
            osStrcat(msg, "...");
            break;
        }
        osSnprintf(buffer, sizeof(buffer), "%02X", token[i]);
        osStrcat(msg, buffer);
    }
}

error_t handleCloudTime(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    TRACE_INFO(" >> respond with current time\r\n");

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudTime", current_time, client_ctx);

    char response[32];

    if (!client_ctx->settings->cloud.enabled || !client_ctx->settings->cloud.enableV1Time)
    {
        osSprintf(response, "%" PRIuTIME, time(NULL));
    }
    else
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V1_TIME, &ctx, client_ctx);
        if (!cloud_request_get(NULL, 0, uri, queryString, NULL, &cbr))
        {
            return NO_ERROR;
        }
        else
        {
            osSprintf(response, "%" PRIuTIME, time(NULL));
        }
    }

    httpPrepareHeader(connection, "text/plain; charset=utf-8", osStrlen(response));
    return httpWriteResponseString(connection, response, false);
}

error_t handleCloudOTA(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    error_t ret = NO_ERROR;
    char *query = strdup(queryString);
    char *localUri = strdup(uri);
    char *savelocalUri = localUri;
    char *filename = strtok_r(&localUri[8], "?", &savelocalUri);
    char *cv = strpbrk(query, "cv=");
    char *timestampTxt = cv ? strtok_r(&cv[3], "&", &cv) : NULL;

    cloudapi_ota_t fileId = (cloudapi_ota_t)atoi(filename);
    (void)fileId;
    time_t timestamp = timestampTxt ? atoi(timestampTxt) : 0;

    char date_buffer[32] = "";
    struct tm tm_info;
    if (localtime_r(&timestamp, &tm_info) != 0)
    {
        strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
    }

    TRACE_INFO(" >> OTA-Request for %d with timestamp %" PRIuTIME " (%s)\r\n", fileId, timestamp, date_buffer);

    settings_internal_toniebox_firmware_t *toniebox_fw = &client_ctx->settings->internal.toniebox_firmware;

    switch (fileId)
    {
    case OTA_FIRMWARE_PD:
        toniebox_fw->otaVersionPd = timestamp;
        break;
    case OTA_FIRMWARE_EU:
        toniebox_fw->otaVersionEu = timestamp;
        break;
    case OTA_SERVICEPACK_CC3200:
        toniebox_fw->otaVersionServicePack = timestamp;
        break;
    case OTA_HTML_CONFIG:
        toniebox_fw->otaVersionHtml = timestamp;
        break;
    case OTA_SFX_BIN:
        toniebox_fw->otaVersionSfx = timestamp;
        break;
    }

    char *folder;
    switch (client_ctx->settings->internal.toniebox_firmware.boxIC)
    {
    case BOX_CC3200:
        folder = custom_asprintf("cc3200%c", PATH_SEPARATOR);
        break;
    case BOX_CC3235:
        folder = custom_asprintf("cc3235%c", PATH_SEPARATOR);
        break;
    case BOX_ESP32:
        folder = custom_asprintf("esp32%c", PATH_SEPARATOR);
        break;
    case BOX_TB2:
        folder = custom_asprintf("tb2%c", PATH_SEPARATOR);
        break;
    default:
        folder = strdup("");
        break;
    }
    char *local_dir = custom_asprintf("%s%cota%c%s%" PRIu8 "%c", client_ctx->settings->internal.firmwaredirfull, PATH_SEPARATOR, PATH_SEPARATOR, folder, fileId, PATH_SEPARATOR);
    osFreeMem(folder);

    time_t biggest_timestamp = 1;
    char *biggest_filename = strdup("");
    FsDir *dir = fsOpenDir(local_dir);
    if (dir)
    {
        while (true)
        {
            FsDirEntry entry;
            if (fsReadDir(dir, &entry) != NO_ERROR)
            {
                fsCloseDir(dir);
                break;
            }
            if (entry.attributes & FS_FILE_ATTR_DIRECTORY)
            {
                continue;
            }
            // Examples for valid filenames
            // 1701777214-esp32-toniebox-eu-v5.230.0-app.ota
            // 1691743093-esp32-toniebox-eu-v5.229.0-app.ota
            // 1669853893_index.html
            // 1640950635_toniebox-eu-cc32XX_v4.0.0.tc.signed.hashed.bin
            // 0034471936_servicepack.hashed.bin

            if (osStrstr(entry.name, ".tmp") != NULL)
            {
                continue;
            }
            char *tmpname = strdup(entry.name);
            char *biggest_timestamp_txt = strtok(tmpname, "-_");
            time_t file_timestamp = (time_t)atoi(biggest_timestamp_txt);
            if (file_timestamp > biggest_timestamp)
            {
                biggest_timestamp = file_timestamp;
                osFreeMem(biggest_filename);
                biggest_filename = strdup(entry.name);
            }
            osFreeMem(tmpname);
        }
    }

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudOtaTime", current_time, client_ctx);
    char *queryStringNew = NULL;

    if (client_ctx->settings->cloud.cacheOta) // && timestamp < biggest_timestamp)
    {
        // TODO replace uri timestamp with biggest_timestamp
        queryStringNew = custom_asprintf("cv=%" PRIuTIME, biggest_timestamp);
        TRACE_INFO(" >> Replaced OTA query %s with new OTA query %s\r\n", queryString, queryStringNew);
    }
    if (queryStringNew == NULL)
    {
        queryStringNew = strdup(queryString);
    }
    if (client_ctx->settings->cloud.enabled && client_ctx->settings->cloud.enableV1Ota)
    {
        ota_ctx_t ota_ctx;
        cbr_ctx_t ctx;
        req_cbr_t cbr;
        if (client_ctx->settings->cloud.cacheOta)
        {
            cbr = getCloudOtaCbr(NULL, uri, queryStringNew, V1_OTA, &ctx, client_ctx);
        }
        else
        {
            cbr = getCloudCbr(connection, uri, queryStringNew, V1_OTA, &ctx, client_ctx);
        }
        ota_ctx.fileId = fileId;
        ctx.customData = &ota_ctx;
        cloud_request_get(NULL, 0, uri, queryStringNew, NULL, &cbr);

        if (!client_ctx->settings->cloud.cacheOta)
        {
            osFreeMem(queryStringNew);
            osFreeMem(biggest_filename);
            osFreeMem(local_dir);
            osFreeMem(query);
            osFreeMem(localUri);
            return ret;
        }
    }

    char *local_file = custom_asprintf("%s%s", local_dir, biggest_filename);
    bool new_ota = false;
    if (biggest_timestamp > 0 && fsFileExists(local_file) && timestamp < biggest_timestamp)
    {
        if (client_ctx->settings->cloud.localOta)
        {
            TRACE_INFO(" >> Found OTA %" PRIu8 " with timestamp %" PRIuTIME " (%s)\r\n", fileId, biggest_timestamp, biggest_filename);
            new_ota = true;
        }
        else
        {
            TRACE_INFO(" >> Found OTA %" PRIu8 " with timestamp %" PRIuTIME " (%s) but local OTA disabled\r\n", fileId, biggest_timestamp, biggest_filename);
        }
    }
    else
    {
        TRACE_INFO(" >> No OTA (newer) found for %" PRIu8 "\r\n", fileId);
        if (timestamp == 1)
        {
            TRACE_WARNING(" >> Box tried to enforce firmware delivery, but nothing here to serve!\r\n");
        }
    }
    if (new_ota)
    {
        // TODO add Content-Disposition: attachment;filename=
        ret = httpSendResponseStreamUnsafe(connection, uri, local_file, false);
    }
    else
    {
        httpPrepareHeader(connection, NULL, 0);
        connection->response.statusCode = 304; // No new firmware
        ret = httpWriteResponse(connection, NULL, 0, false);
    }

    osFreeMem(queryStringNew);
    osFreeMem(local_file);
    osFreeMem(biggest_filename);
    osFreeMem(local_dir);
    osFreeMem(query);
    osFreeMem(localUri);
    return ret;
}

bool checkCustomTonie(char *ruid, uint8_t *token, settings_t *settings)
{
    if (settings->cloud.markCustomTagByPass)
    {
        bool tokenIsZero = TRUE;
        for (uint8_t i = 0; i < 32; i++)
        {
            if (token[i] != 0)
            {
                tokenIsZero = FALSE;
                break;
            }
        }
        if (tokenIsZero)
        {
            TRACE_INFO("Found possible custom tonie by password\r\n");
            return true;
        }
    }
    if (settings->cloud.markCustomTagByUid)
    {
        // Ignore TB2 special files 00000af0 (00000001)
        if (!(ruid[0] == '0' && ruid[1] == '0' && ruid[2] == '0' && ruid[3] == '0' && ruid[4] == '0' && ruid[5] == 'a' && ruid[6] == 'f' && ruid[7] == '0'))
        {
            if (ruid[15] != '0' || ruid[14] != 'e' || ruid[13] != '4' || ruid[12] != '0' || ruid[11] != '3' || ruid[10] != '0')
            {
                TRACE_INFO("Found possible custom tonie by uid\r\n");
                return true;
            }
        }
    }
    return false;
}

static bool_t tonie_cloud_access_allowed(const tonie_info_t *tonieInfo)
{
    return tonieInfo != NULL && (!tonieInfo->json.nocloud || tonieInfo->json.cloud_override);
}

void markCustomTonie(tonie_info_t *tonieInfo)
{
    tonieInfo->json.nocloud = true;
    tonieInfo->json._updated = true;
    TRACE_INFO("Marked custom tonie %s\r\n", tonieInfo->contentPath);
}

void markLiveTonie(tonie_info_t *tonieInfo)
{
    tonieInfo->json.live = true;
    tonieInfo->json._updated = true;
    TRACE_INFO("Marked custom tonie %s\r\n", tonieInfo->contentPath);
}

void dumpRuidAuth(contentJson_t *content_json, char *ruid, uint8_t *authentication)
{
    if (!content_json->cloud_override && osStrlen(content_json->cloud_ruid) == 0)
    {
        osFreeMem(content_json->cloud_auth);
        content_json->cloud_auth_len = TONIE_AUTH_TOKEN_LENGTH;
        content_json->cloud_auth = osAllocMem(content_json->cloud_auth_len);
        osMemcpy(content_json->cloud_auth, authentication, content_json->cloud_auth_len);

        osFreeMem(content_json->cloud_ruid);
        content_json->cloud_ruid = strdup(ruid);
        content_json->_updated = true;
        TRACE_INFO("Dumped rUID %s and auth into content.json\r\n", content_json->cloud_ruid);
    }
}

error_t handleCloudLog(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    uint8_t data[BODY_BUFFER_SIZE + 1];
    size_t size = 0;

    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %" PRIuSIZE " bigger than buffer size %i bytes\r\n", connection->request.byteCount, BODY_BUFFER_SIZE);
    }
    else
    {
        error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
        if (error != NO_ERROR)
        {
            TRACE_ERROR("httpReceive failed!\r\n");
            return error;
        }
        data[size] = '\0';
        TRACE_INFO(" >> client log data (%" PRIuSIZE " bytes):\n%s\n", size, data);
    }

    if (client_ctx->settings->cloud.enabled && client_ctx->settings->cloud.enableV1Log)
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V1_LOG, &ctx, client_ctx);
        cloud_request_post(NULL, 0, uri, queryString, data, size, NULL, &cbr);
    }
    return NO_ERROR;
}

error_t handleCloudClaim(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    error_t ret = NO_ERROR;
    char ruid[17];
    uint8_t *token = connection->private.authentication_token;

#define RUID_URI_CLAIM_BEGIN 10
    osStrncpy(ruid, &uri[RUID_URI_CLAIM_BEGIN], sizeof(ruid));
    ruid[16] = 0;

    if (osStrlen(ruid) != 16)
    {
        TRACE_WARNING(" >>  invalid URI\r\n");
        return ERROR_NOT_FOUND;
    }
    char msg[TONIE_AUTH_TOKEN_LENGTH * 2 + 1] = {0};
    convertTokenBytesToString(token, msg, client_ctx->settings->log.logFullAuth);
    TRACE_INFO(" >> client claim requested rUID %s, auth %s\r\n", ruid, msg);

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudClaimTime", current_time, client_ctx);
    setLastRuid(ruid, client_ctx->settingsNoOverlay);

    tonie_info_t *tonieInfo;
    tonieInfo = getTonieInfoFromRuid(ruid, true, client_ctx->settings);

    /* allow to override HTTP status code if needed */
    bool served = false;
    httpPrepareHeader(connection, NULL, 0);
    connection->response.statusCode = 200;

    if (client_ctx->settings->cloud.dumpRuidAuthContentJson && connection->request.auth.found)
    {
        dumpRuidAuth(&tonieInfo->json, ruid, token);
    }
    if (tonieInfo->json.hide || !tonieInfo->json.claimed)
    {
        tonieInfo->json.hide = false;
        tonieInfo->json.claimed = true;
        tonieInfo->json._updated = true;
    }
    saveTonieInfo(tonieInfo, true);

    if (tonie_cloud_access_allowed(tonieInfo))
    {
        if (checkCustomTonie(ruid, token, client_ctx->settings) && !tonieInfo->json.cloud_override && connection->request.auth.found)
        {
            TRACE_INFO(" >> custom tonie detected, nothing forwarded\r\n");
            markCustomTonie(tonieInfo);
        }
        else if (client_ctx->settings->cloud.enabled && client_ctx->settings->cloud.enableV1Claim)
        {
            if (tonieInfo->json.cloud_override)
            {
                token = tonieInfo->json.cloud_auth;
                convertTokenBytesToString(token, msg, client_ctx->settings->log.logFullAuth);
                osMemcpy((char_t *)&uri[RUID_URI_CLAIM_BEGIN], tonieInfo->json.cloud_ruid, osStrlen(tonieInfo->json.cloud_ruid));
                TRACE_INFO("Serve cloud claim from alternative rUID %s, auth %s\r\n", tonieInfo->json.cloud_ruid, msg);
            }
            cbr_ctx_t ctx;
            req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V1_CLAIM, &ctx, client_ctx);
            cloud_request_get(NULL, 0, uri, queryString, token, &cbr);
            served = true;
        }
        else
        {
            TRACE_INFO(" >> cloud claim disabled\r\n");
        }
    }
    else
    {
        TRACE_INFO(" >> nocloud content, nothing forwarded\r\n");
    }

    freeTonieInfo(tonieInfo);

    if (!served)
    {
        ret = httpWriteResponse(connection, NULL, 0, false);
    }

    return ret;
}

tonie_info_t *getTonieInfoForRequest(HttpConnection *connection, const char_t *uri, int ruid_uri_begin, const char_t *queryString, client_ctx_t *client_ctx, bool_t noPassword, char *ruid, bool_t *tonie_marked, error_t *error)
{
    uint8_t *token = connection->private.authentication_token;

    char queryValue[16];
    if (queryGet(queryString, "skip_header", queryValue, sizeof(queryValue)))
    {
        if (queryValue[0] == 't')
        {
            connection->private.client_ctx.skip_taf_header = true;
        }
    }

    osStrncpy(ruid, &uri[ruid_uri_begin], 16);
    ruid[16] = 0;
    if (osStrlen(ruid) != 16)
    {
        TRACE_WARNING(" >>  invalid URI\r\n");
        *error = ERROR_NOT_FOUND;
        return NULL;
    }

    if (connection->request.Range.start != 0)
    {
        TRACE_INFO(" >> client requested partial download\r\n");
    }

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudContentTime", current_time, client_ctx);
    char msg[TONIE_AUTH_TOKEN_LENGTH * 2 + 1] = {0};
    convertTokenBytesToString(token, msg, client_ctx->settings->log.logFullAuth);
    TRACE_INFO(" >> client requested content for rUID %s, auth %s\r\n", ruid, msg);
    if (!noPassword)
    {
        setLastRuid(ruid, client_ctx->settingsNoOverlay);
    }

    tonie_info_t *tonieInfo;
    tonieInfo = getTonieInfoFromRuid(ruid, true, client_ctx->settings);

    *tonie_marked = false;
    if (client_ctx->settings->cloud.enabled && client_ctx->settings->cloud.enableV2Content && !tonieInfo->json.nocloud)
    {
        uint64_t uid = strtoull(ruid, NULL, 16);
        uid = bswap_64(uid);

        size_t freshnessCacheLen = 0;
        uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", client_ctx->settings->internal.overlayNumber, &freshnessCacheLen);
        TRACE_DEBUG(" >> Checking freshnessCache with %" PRIuSIZE " entries\r\n", freshnessCacheLen);
        for (size_t i = 0; i < freshnessCacheLen; i++)
        {
            char cruid[17];
            osSprintf(cruid, "%016" PRIX64, bswap_64(freshnessCache[i]));
            TRACE_DEBUG(" >> freshnessCache[%" PRIuSIZE "] = %" PRIu64 " =? %" PRIu64 "\r\n", i, freshnessCache[i], uid);
            TRACE_DEBUG(" >> freshnessCache[%" PRIuSIZE "] = %s =? %s\r\n", i, cruid, ruid);
            if (freshnessCache[i] == uid)
            {
                if (tap_is_stable_cached_playlist(tonieInfo))
                {
                    TRACE_VERBOSE(" >> rUID %s found in freshnessCache but ignored for stable cached TAP\r\n", ruid);
                }
                else
                {
                    *tonie_marked = true;
                    TRACE_INFO(" >> rUID %s found in freshnessCache, refresh content\r\n", ruid);
                }
                break;
            }
        }
    }

    if (!tonieInfo->json.nocloud && !noPassword && checkCustomTonie(ruid, token, client_ctx->settings) && !tonieInfo->json.cloud_override && connection->request.auth.found)
    {
        TRACE_INFO(" >> custom tonie detected, nothing forwarded\r\n");
        markCustomTonie(tonieInfo);
    }

    settings_t *settings = client_ctx->settings;

    if (client_ctx->settings->cloud.dumpRuidAuthContentJson && connection->request.auth.found)
    {
        dumpRuidAuth(&tonieInfo->json, ruid, token);
    }
    if (tonieInfo->json.hide)
    {
        tonieInfo->json.hide = false;
        tonieInfo->json._updated = true;
    }

    bool setLive = false;
    const char *assignUnknown = settings_get_string("internal.assign_unknown");
    const char *assignFile = NULL;

    if (osStrlen(assignUnknown) > 0)
    {
        if (!tonieInfo->exists)
        {
            assignFile = assignUnknown;
            TRACE_INFO(" >> this is a unknown tonie, assigning '%s'\r\n", assignFile);
        }

        if (settings->core.flex_enabled)
        {
            char uid[17];
            for (int pos = 0; pos < 16; pos += 2)
            {
                osStrncpy(&uid[pos], &ruid[14 - pos], 2);
            }
            uid[16] = 0;
            if (!osStrcasecmp(uid, settings->core.flex_uid))
            {
                assignFile = assignUnknown;
                setLive = true;
                TRACE_INFO(" >> this is the defined flex tonie, assigning '%s'\r\n", assignFile);
            }
        }
    }

    if (assignFile)
    {
        do
        {
            if (!fsFileExists(assignFile))
            {
                TRACE_ERROR("Path to assign not available: %s\r\n", assignFile);
                break;
            }

            /* check assignFile for validity */
            tonie_info_t *tonieInfoAssign = getTonieInfo(assignFile, true, client_ctx->settings);
            if (!tonieInfoAssign->valid)
            {
                freeTonieInfo(tonieInfoAssign);
                TRACE_ERROR("TAF header invalid: %s\r\n", assignFile);
                break;
            }

            char *dir = strdup(tonieInfo->contentPath);
            dir[osStrlen(dir) - 8] = '\0';
            fsCreateDir(dir);
            osFreeMem(dir);

            *error = fsCopyFile(assignFile, tonieInfo->contentPath, true);
            if (*error != NO_ERROR)
            {
                freeTonieInfo(tonieInfoAssign);
                TRACE_ERROR("Could not copy %s to %s, error=%s\r\n", assignFile, tonieInfo->contentPath, error2text(*error));
                break;
            }

            freeTonieInfo(tonieInfoAssign);

            /* reopen the TAF with new content */
            char *oldFile = strdup(tonieInfo->contentPath);
            freeTonieInfo(tonieInfo);

            tonieInfo = getTonieInfo(oldFile, true, client_ctx->settings);
            free(oldFile);

            if (!tonieInfo->valid)
            {
                TRACE_ERROR("TAF headerinvalid, delete it again: %s\r\n", tonieInfo->contentPath);
                fsDeleteFile(tonieInfo->contentPath);
                break;
            }

            TRACE_INFO("Assigned to %s\r\n", assignFile);

            if (setLive)
            {
                markLiveTonie(tonieInfo);
            }

        } while (0);

        settings_set_string("internal.assign_unknown", "");
        *error = NO_ERROR;
    }

    saveTonieInfo(tonieInfo, true);
    return tonieInfo;
}

static error_t tap_send_response_stream_unsafe_with_size(
    HttpConnection *connection,
    const char_t *uri,
    const char_t *absolute_path,
    uint32_t stream_max_size)
{
    client_ctx_t saved_client_ctx = connection->private.client_ctx;
    client_ctx_t local_client_ctx = saved_client_ctx;
    settings_t local_settings = *local_client_ctx.settings;

    local_settings.encode.stream_max_size = stream_max_size;
    local_client_ctx.settings = &local_settings;
    connection->private.client_ctx = local_client_ctx;
    error_t error = httpSendResponseStreamUnsafe(connection, uri,
                                                 absolute_path, true);
    connection->private.client_ctx = saved_client_ctx;
    return error;
}

typedef struct
{
    const char *const *chapter_paths;
    size_t chapter_count;
    uint32_t audio_id;
    const char *tmp_taf;
    toniefile_live_header_t live_header;
    uint32_t predicted_size;
    size_t current_source;
    error_t error;
    bool_t started;
    bool_t active;
    bool_t done;
} native_collection_taf_task_t;

static void native_collection_taf_task(void *param)
{
    native_collection_taf_task_t *task = param;
    task->started = TRUE;
    task->error = tap_remux_native_collection(
        task->chapter_paths, task->chapter_count, task->audio_id,
        task->tmp_taf, &task->current_source, &task->active,
        &task->live_header, &task->predicted_size);
    task->active = FALSE;
    task->done = TRUE;
    osDeleteTask((OsTaskId)OS_SELF_TASK_ID);
}

static bool_t native_collection_taf_is_current(const char *path,
                                               uint32_t audio_id,
                                               settings_t *settings)
{
    if (path == NULL || !fsFileExists(path))
    {
        return FALSE;
    }
    tonie_info_t *info = getTonieInfoV2(path, false,
                                        settings->core.tap_taf_validation,
                                        settings);
    bool_t current = info != NULL && info->valid && info->tafHeader != NULL &&
                     info->tafHeader->audio_id == audio_id;
    if (info != NULL)
    {
        freeTonieInfo(info);
    }
    return current;
}

static error_t serve_native_collection_tb1(
    HttpConnection *connection,
    const char_t *uri,
    client_ctx_t *client_ctx,
    tonie_info_t *tonieInfo,
    const char *ruid,
    cloudapi_t api)
{
    v3_native_library_collection_t collection;
    error_t error = v3_native_library_collection_load(
        client_ctx->settings->internal.librarydirfull,
        tonieInfo->json.source, FALSE, &collection);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Native collection source for rUID %s is unavailable: %s\r\n",
                    ruid, error2text(error));
        return error;
    }

    char *cache_dir = custom_asprintf("%s%ctb1-native-library",
                                      client_ctx->settings->internal.cachedirfull,
                                      PATH_SEPARATOR);
    char *final_taf = cache_dir != NULL
                          ? custom_asprintf("%s%c%s.taf", cache_dir,
                                            PATH_SEPARATOR,
                                            collection.content_hash)
                          : NULL;
    char *tmp_taf = final_taf != NULL
                        ? custom_asprintf("%s.tmp", final_taf)
                        : NULL;
    if (cache_dir == NULL || final_taf == NULL || tmp_taf == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }
    error = fsCreateDirEx(cache_dir, true);
    if (error != NO_ERROR && !fsDirExists(cache_dir))
    {
        goto cleanup;
    }

    error = tap_target_lock(final_taf);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Native collection TAF lock unavailable for %s: %s\r\n",
                    final_taf, error2text(error));
        goto cleanup;
    }
    bool_t target_locked = TRUE;

    if (native_collection_taf_is_current(final_taf, collection.audio_id,
                                         client_ctx->settings))
    {
        TRACE_INFO("Serve cached TB1 native collection hash=%s audioId=%08" PRIX32
                   " path=%s\r\n",
                   collection.content_hash, collection.audio_id, final_taf);
        tap_target_unlock(final_taf);
        target_locked = FALSE;
        connection->response.keepAlive = true;
        error = httpSendResponseStreamUnsafe(connection, uri, final_taf, false);
        if (error == NO_ERROR)
        {
            freshness_clear_cache_after_content_request(client_ctx, api, ruid);
        }
        goto unlock_cleanup;
    }

    v3_native_library_collection_free(&collection);
    error = v3_native_library_collection_load(
        client_ctx->settings->internal.librarydirfull,
        tonieInfo->json.source, TRUE, &collection);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Native collection hash verification failed for rUID %s: %s\r\n",
                    ruid, error2text(error));
        goto unlock_cleanup;
    }

    const char **chapter_paths = osAllocMem(
        sizeof(*chapter_paths) * collection.chapter_count);
    if (chapter_paths == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto unlock_cleanup;
    }
    for (size_t i = 0; i < collection.chapter_count; i++)
    {
        chapter_paths[i] = collection.chapters[i].path;
    }

    native_collection_taf_task_t task;
    osMemset(&task, 0, sizeof(task));
    task.chapter_paths = chapter_paths;
    task.chapter_count = collection.chapter_count;
    task.audio_id = collection.audio_id;
    task.tmp_taf = tmp_taf;
    error = tap_remux_native_collection(
        task.chapter_paths, task.chapter_count, task.audio_id,
        NULL, NULL, NULL, &task.live_header, &task.predicted_size);
    if (error != NO_ERROR || task.predicted_size <= TONIEFILE_FRAME_SIZE)
    {
        TRACE_ERROR("Could not predict TB1 native collection TAF %s: %s\r\n",
                    final_taf, error2text(error));
        osFreeMem(chapter_paths);
        goto unlock_cleanup;
    }

    if (fsFileExists(tmp_taf))
    {
        fsDeleteFile(tmp_taf);
    }
    OsTaskParameters task_params;
    osMemset(&task_params, 0, sizeof(task_params));
    task_params.stackSize = 10U * 1024U;
    task_params.priority = 0;
    OsTaskId task_id = osCreateTask("native-library-taf",
                                   &native_collection_taf_task,
                                   &task, &task_params);
    if (task_id == (OsTaskId)OS_INVALID_TASK_ID)
    {
        error = ERROR_FAILURE;
        osFreeMem(chapter_paths);
        goto unlock_cleanup;
    }

    uint32_t waited_ms = 0;
    while (!task.started && !task.done &&
           waited_ms < TAP_GENERATOR_START_WAIT_MS)
    {
        osDelayTask(TAP_GENERATOR_START_POLL_MS);
        waited_ms += TAP_GENERATOR_START_POLL_MS;
    }
    uint32_t tmp_size = 0;
    waited_ms = 0;
    while (!task.done && waited_ms < TAP_LIVE_READY_WAIT_MS)
    {
        if (fsGetFileSize(tmp_taf, &tmp_size) == NO_ERROR &&
            tmp_size >= TAP_LIVE_READY_BYTES)
        {
            break;
        }
        osDelayTask(TAP_LIVE_READY_POLL_MS);
        waited_ms += TAP_LIVE_READY_POLL_MS;
    }

    error_t response_error = task.error;
    if (response_error == NO_ERROR &&
        (task.active || task.done) && fsFileExists(tmp_taf))
    {
        TRACE_INFO("Stream TB1 native collection hash=%s audioId=%08" PRIX32
                   " chapters=%" PRIuSIZE " bytes=%" PRIu32 "\r\n",
                   collection.content_hash, collection.audio_id,
                   collection.chapter_count, task.predicted_size);
        connection->response.keepAlive = true;
        response_error = tap_send_response_stream_unsafe_with_size(
            connection, uri, tmp_taf, task.predicted_size);
    }

    while (!task.done)
    {
        osDelayTask(100);
    }
    error = task.error;
    if (error == NO_ERROR)
    {
        error = tap_publish_taf_replace_safe(tmp_taf, final_taf);
        if (error == NO_ERROR)
        {
            TRACE_INFO("Published TB1 native collection hash=%s audioId=%08" PRIX32
                       " path=%s\r\n",
                       collection.content_hash, collection.audio_id, final_taf);
        }
    }
    if (fsFileExists(tmp_taf))
    {
        fsDeleteFile(tmp_taf);
    }
    osFreeMem(chapter_paths);
    if (response_error != NO_ERROR)
    {
        error = response_error;
    }
    else if (error == NO_ERROR)
    {
        freshness_clear_cache_after_content_request(client_ctx, api, ruid);
    }

unlock_cleanup:
    if (target_locked)
    {
        tap_target_unlock(final_taf);
    }
cleanup:
    v3_native_library_collection_free(&collection);
    osFreeMem(tmp_taf);
    osFreeMem(final_taf);
    osFreeMem(cache_dir);
    return error;
}

error_t handleCloudContentExt(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx, bool_t noPassword, int ruid_begin, cloudapi_t api)
{
    char ruid[17];
    error_t error = NO_ERROR;
    bool_t tonie_marked = false;

    tonie_info_t *tonieInfo = getTonieInfoForRequest(connection, uri, ruid_begin, queryString, client_ctx, noPassword, ruid, &tonie_marked, &error);

    if (tonieInfo == NULL)
    {
        return error;
    }

    uint8_t *token = connection->private.authentication_token;

    const bool_t cloud_mode_enabled = api == V3_CHAPTER
                                          ? client_ctx->settings->cloud.tb2_v3_enabled
                                          : client_ctx->settings->cloud.enabled;
    bool can_use_cloud = cloud_mode_enabled &&
                         (api != V2_CONTENT || client_ctx->settings->cloud.enableV2Content) &&
                         tonie_cloud_access_allowed(tonieInfo);
    if (tonieInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION)
    {
        error = serve_native_collection_tb1(connection, uri, client_ctx,
                                            tonieInfo, ruid, api);
        freeTonieInfo(tonieInfo);
        return error;
    }
    if (tonieInfo->json._source_type == CT_SOURCE_TONIEPLAY_COLLECTION)
    {
        TRACE_WARNING("Rejecting TB2 Tonieplay collection on TB1 content path rUID=%s\r\n",
                      ruid);
        freeTonieInfo(tonieInfo);
        return ERROR_ACCESS_DENIED;
    }
    if (tonieInfo->json._source_type == CT_SOURCE_STREAM)
    {
        char *streamFileRel = &tonieInfo->json._streamFile[osStrlen(client_ctx->settings->internal.datadirfull)];
        TRACE_INFO("Serve streaming content from %s\r\n", tonieInfo->json._source_resolved);
        connection->response.keepAlive = true;

        ffmpeg_stream_ctx_t ffmpeg_ctx;
        ffmpeg_ctx.append = (connection->request.Range.start != 0);
        ffmpeg_ctx.sweep = client_ctx->settings->encode.ffmpeg_sweep_startup_buffer;
        ffmpeg_ctx.source = tonieInfo->json._source_resolved;
        ffmpeg_ctx.skip_seconds = tonieInfo->json.skip_seconds;
        ffmpeg_ctx.targetFile = tonieInfo->json._streamFile;

        stream_ctx_t *stream_ctx = &client_ctx->state->box.stream_ctx;
        stream_ctx_next_generation(stream_ctx);
        stream_ctx->active = false;
        stream_ctx->quit = false;
        stream_ctx->error = NO_ERROR;
        stream_ctx->current_source = 0;
        stream_ctx->stop_on_playback_stop = true;
        stream_ctx->ctx = &ffmpeg_ctx;
        stream_ctx->taskParams.priority = 0;
        stream_ctx->taskParams.stackSize = 10 * 1024;
        stream_ctx->taskId = osCreateTask(streamFileRel, &ffmpeg_stream_task, stream_ctx, &stream_ctx->taskParams);

        while (!stream_ctx->active && stream_ctx->error == NO_ERROR)
        {
            osDelayTask(100);
        }
        if (stream_ctx->error == NO_ERROR)
        {
            if (client_ctx->settings->encode.ffmpeg_sweep_startup_buffer)
            {
                osDelayTask(client_ctx->settings->encode.ffmpeg_sweep_delay_ms);
            }

            uint32_t delay = client_ctx->settings->encode.ffmpeg_stream_buffer_ms;
            TRACE_INFO("Serve streaming content from %s, delay %" PRIu32 "ms\r\n", tonieInfo->json.source, delay);
            ffmpeg_ctx.sweep = false;

            osDelayTask(delay);
            error_t response_error = httpSendResponseStream(connection, streamFileRel, true);
            if (response_error)
            {
                TRACE_ERROR(" >> file %s not available or not send, error=%s...\r\n", tonieInfo->contentPath, error2text(response_error));
            }
        }
        stream_ctx->active = false;
        while (!stream_ctx->quit)
        {
            osDelayTask(100);
        }
    }
    else if (tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM)
    {
        TRACE_INFO("Serve streaming TAP %s from %s\r\n", tonieInfo->contentPath, tonieInfo->json._source_resolved);
        char *streamFileRel = &tonieInfo->contentPath[osStrlen(client_ctx->settings->internal.datadirfull)];
        connection->response.keepAlive = true;

        tap_generate_param_t tap_param;
        tap_param.tap = &tonieInfo->json._tap;
        bool_t dynamic_tap_playlist = tap_param.tap->shuffle != TAP_SHUFFLE_NONE;
        bool_t force_tap_regeneration = tap_param.tap->audio_id == 0 || dynamic_tap_playlist;
        tap_param.force = force_tap_regeneration;
        tap_param.preserve_on_client_disconnect = true;
        tap_param.current_source = 0;
        tap_param.error = NO_ERROR;
        tap_param.generator_active = false;
        tap_param.generator_done = false;
        tap_param.runtime_indices = NULL;
        tap_param.runtime_files_count = 0;
        osMemset(&tap_param.live_header, 0, sizeof(tap_param.live_header));

        error_t tap_lock_error = tap_target_lock(tap_param.tap->_filepath_resolved);
        bool_t tap_target_locked = (tap_lock_error == NO_ERROR);
        if (tap_lock_error != NO_ERROR)
        {
            if (!force_tap_regeneration && tap_final_cache_is_current(tap_param.tap))
            {
                TRACE_INFO("Serve finalized TAP TAF directly from %s after TAP lock timeout\r\n", tap_param.tap->_filepath_resolved);
                char *finalFileRel = &tap_param.tap->_filepath_resolved[osStrlen(client_ctx->settings->internal.datadirfull)];
                error_t response_error = httpSendResponseStream(connection, finalFileRel, false);
                if (response_error == NO_ERROR)
                {
                    freshness_clear_cache_after_content_request(client_ctx, api, ruid);
                }
                freeTonieInfo(tonieInfo);
                return response_error;
            }

            TRACE_ERROR(" >> TAP target lock unavailable for %s, error=%s\r\n", tap_param.tap->_filepath_resolved, error2text(tap_lock_error));
            freeTonieInfo(tonieInfo);
            return tap_lock_error;
        }

        if (!force_tap_regeneration && tap_final_cache_is_current(tap_param.tap))
        {
            TRACE_INFO("Serve finalized TAP TAF directly from %s\r\n", tap_param.tap->_filepath_resolved);
            char *finalFileRel = &tap_param.tap->_filepath_resolved[osStrlen(client_ctx->settings->internal.datadirfull)];
            tap_target_unlock(tap_param.tap->_filepath_resolved);
            tap_target_locked = FALSE;
            error_t response_error = httpSendResponseStream(connection, finalFileRel, false);
            if (response_error)
            {
                if (response_error == ERROR_WRITE_FAILED)
                {
                    TRACE_WARNING(" >> client disconnected while sending finalized TAP file %s, error=%s\r\n", tap_param.tap->_filepath_resolved, error2text(response_error));
                }
                else
                {
                    TRACE_ERROR(" >> finalized TAP file %s not available or not sent, error=%s...\r\n", tap_param.tap->_filepath_resolved, error2text(response_error));
                }
            }
            freeTonieInfo(tonieInfo);
            if (response_error == NO_ERROR)
            {
                freshness_clear_cache_after_content_request(client_ctx, api, ruid);
            }
            return response_error;
        }

        error_t runtime_error = tap_prepare_runtime_indices(tap_param.tap, &tap_param.runtime_indices, &tap_param.runtime_files_count);
        if (runtime_error != NO_ERROR)
        {
            tap_target_unlock(tap_param.tap->_filepath_resolved);
            freeTonieInfo(tonieInfo);
            return runtime_error;
        }

        uint32_t predicted_taf_size = 0;
        if (tap_predict_taf_live_header(tap_param.tap, tap_param.runtime_indices, tap_param.runtime_files_count, &tap_param.live_header, &predicted_taf_size) != NO_ERROR)
        {
            predicted_taf_size = 0;
            osMemset(&tap_param.live_header, 0, sizeof(tap_param.live_header));
        }

        bool_t tap_live_prediction_valid = predicted_taf_size > TONIEFILE_FRAME_SIZE &&
                                           tap_param.live_header.payload_size > 0 &&
                                           tap_param.live_header.has_sha1_hash &&
                                           tap_param.live_header.has_ogg_state;
        TRACE_VERBOSE("TAP live prediction: valid=%u predicted_taf_size=%" PRIu32 " payload_size=%" PRIu32 " track_pages=%" PRIuSIZE "\r\n",
                      tap_live_prediction_valid, predicted_taf_size, tap_param.live_header.payload_size, tap_param.live_header.track_page_nums_count);

        stream_ctx_t *stream_ctx = &client_ctx->state->box.stream_ctx;
        uint32_t stream_generation = stream_ctx_next_generation(stream_ctx);
        tap_param.generation = stream_generation;
        tap_param.generator_started = false;
        stream_ctx->active = false;
        stream_ctx->quit = false;
        stream_ctx->error = NO_ERROR;
        stream_ctx->current_source = 0;
        stream_ctx->stop_on_playback_stop = false;
        stream_ctx->ctx = &tap_param;
        tap_param.stream_ctx = stream_ctx;
        stream_ctx->taskParams.stackSize = 10 * 1024;
        stream_ctx->taskParams.priority = 0;
        stream_ctx->taskId = osCreateTask(streamFileRel, &tap_generate_task, &tap_param, &stream_ctx->taskParams);
        if (stream_ctx->taskId == (OsTaskId)OS_INVALID_TASK_ID)
        {
            TRACE_ERROR("Could not start TAP generator task for %s\r\n", tonieInfo->contentPath);
            tap_param.error = ERROR_FAILURE;
            tap_param.generator_done = true;
        }

        uint32_t tap_generator_start_waited_ms = 0;
        while (!tap_param.generator_started && !tap_param.generator_done && tap_param.error == NO_ERROR && tap_generator_start_waited_ms < TAP_GENERATOR_START_WAIT_MS)
        {
            osDelayTask(TAP_GENERATOR_START_POLL_MS);
            tap_generator_start_waited_ms += TAP_GENERATOR_START_POLL_MS;
        }
        if (!tap_param.generator_started && !tap_param.generator_done && tap_param.error == NO_ERROR)
        {
            TRACE_WARNING("TAP generator task has not entered for %s after %" PRIu32 "ms; keeping handler context until task completion\r\n", tonieInfo->contentPath, tap_generator_start_waited_ms);
        }

        bool_t client_disconnected = false;
        if (tap_param.error == NO_ERROR)
        {
            uint32_t tap_live_waited_ms = 0;
            uint32_t tap_live_tmp_size = 0;

            while (tap_live_waited_ms < TAP_LIVE_READY_WAIT_MS)
            {
                error_t size_error = fsGetFileSize(tonieInfo->contentPath, &tap_live_tmp_size);

                if (size_error == NO_ERROR && tap_live_tmp_size >= TAP_LIVE_READY_BYTES)
                {
                    break;
                }

                if (tap_param.error != NO_ERROR || tap_param.generator_done)
                {
                    break;
                }

                osDelayTask(TAP_LIVE_READY_POLL_MS);
                tap_live_waited_ms += TAP_LIVE_READY_POLL_MS;
            }

            if (stream_ctx->generation == stream_generation)
            {
                stream_ctx->active = tap_param.generator_active;
                stream_ctx->error = tap_param.error;
                stream_ctx->quit = tap_param.generator_done;
            }

            error_t response_error;
            if (tap_live_prediction_valid)
            {
                response_error = tap_send_response_stream_with_size(connection, streamFileRel, predicted_taf_size);
            }
            else
            {
                response_error = httpSendResponseStream(connection, streamFileRel, true);
            }
            TRACE_VERBOSE("TAP live response finished: error=%s predicted_taf_size=%" PRIu32 " active=%u quit=%u current=%u\r\n",
                          error2text(response_error), predicted_taf_size, tap_param.generator_active, tap_param.generator_done, stream_ctx->generation == stream_generation);

            if (response_error)
            {
                if (tap_param.error == NO_ERROR && !tap_param.generator_done)
                {
                    client_disconnected = true;
                    TRACE_WARNING(" >> TAP stream client disconnected while sending %s, generation still active; preserving background generation, error=%s\r\n", tonieInfo->contentPath, error2text(response_error));
                }
                else if (response_error == ERROR_WRITE_FAILED)
                {
                    TRACE_WARNING(" >> TAP stream client disconnected while sending %s, generation already finished, error=%s\r\n", tonieInfo->contentPath, error2text(response_error));
                }
                else
                {
                    TRACE_ERROR(" >> file %s not available or not sent, error=%s...\r\n", tonieInfo->contentPath, error2text(response_error));
                }
            }
        }
        else
        {
            TRACE_ERROR(" >> TAP stream not available, error=%s...\r\n", error2text(tap_param.error));
        }

        if (!client_disconnected)
        {
            tap_param.generator_active = false;
        }

        while (!tap_param.generator_done)
        {
            osDelayTask(100);
        }

        if (tap_param.error == NO_ERROR)
        {
            error_t publish_error = tap_publish_taf_replace_safe(tonieInfo->contentPath, tap_param.tap->_filepath_resolved);
            if (publish_error != NO_ERROR)
            {
                TRACE_ERROR("Could not publish generated TAP %s from %s, error=%s\r\n", tap_param.tap->_filepath_resolved, tonieInfo->contentPath, error2text(publish_error));
                tap_param.error = publish_error;
            }
        }
        if (fsFileExists(tonieInfo->contentPath))
        {
            error_t delete_error = fsDeleteFile(tonieInfo->contentPath);
            if (delete_error != NO_ERROR && delete_error != ERROR_FILE_NOT_FOUND)
            {
                TRACE_WARNING("Could not delete temporary TAP file %s, error=%s\r\n", tonieInfo->contentPath, error2text(delete_error));
            }
        }

        if (stream_ctx->generation == stream_generation)
        {
            stream_ctx->current_source = tap_param.current_source;
            stream_ctx->error = tap_param.error;
            stream_ctx->active = tap_param.generator_active;
            stream_ctx->quit = tap_param.generator_done;
        }

        if (tap_target_locked)
        {
            tap_target_unlock(tap_param.tap->_filepath_resolved);
        }
        tap_free_runtime_indices(tap_param.runtime_indices);
    }
    else if (tonieInfo->exists && tonieInfo->valid && (!tonie_marked || !can_use_cloud))
    {
        TRACE_INFO("Serve local content from %s\r\n", tonieInfo->contentPath);
        connection->response.keepAlive = true;

        if (tonieInfo->json._source_type == CT_SOURCE_TAF_INCOMPLETE)
        {
            TRACE_INFO("Found incomplete TAF, streaming...\r\n");
        }

        size_t dataPathLen = osStrlen(client_ctx->settings->internal.datadirfull);
        if (osStrncmp(tonieInfo->contentPath, client_ctx->settings->internal.datadirfull, dataPathLen) == 0)
        {
            error = httpSendResponseStream(connection, &tonieInfo->contentPath[dataPathLen], (tonieInfo->json._source_type == CT_SOURCE_TAF_INCOMPLETE));
            if (error)
            {
                if (error == ERROR_WRITE_FAILED)
                {
                    TRACE_WARNING(" >> client disconnected while sending file %s, error=%s\r\n", tonieInfo->contentPath, error2text(error));
                }
                else
                {
                    TRACE_ERROR(" >> file %s not available or not sent, error=%s...\r\n", tonieInfo->contentPath, error2text(error));
                }
            }
        }
        else
        {
            TRACE_ERROR(" >> path %s is not within the data dir %s\r\n", tonieInfo->contentPath, client_ctx->settings->internal.datadirfull);
        }
    }
    else
    {
        if (!can_use_cloud)
        {
            if (tonieInfo->json.nocloud && !tonieInfo->json.cloud_override)
            {
                TRACE_INFO("Content marked as no cloud and no content locally available\r\n");
            }
            else
            {
                TRACE_INFO("No local content available and cloud access disabled\r\n");
            }
            httpPrepareHeader(connection, NULL, 0);
            connection->response.statusCode = 404;
            error = httpWriteResponse(connection, NULL, 0, false);
        }
        else
        {
            TRACE_INFO("Serve cloud content from %s\r\n", uri);

            if (tonieInfo->json.source != NULL)
            {
                TRACE_INFO(" >> Removing source %s for download\r\n", tonieInfo->json.source);
                tonieInfo->json.source[0] = '\0';
                tonieInfo->json._updated = true;
                freeTonieInfo(tonieInfo);
                tonieInfo = getTonieInfoFromRuid(ruid, true, client_ctx->settings);
            }
            if (tonieInfo->json.cloud_override)
            {
                token = tonieInfo->json.cloud_auth;
                char msg[TONIE_AUTH_TOKEN_LENGTH * 2 + 1] = {0};
                convertTokenBytesToString(token, msg, client_ctx->settings->log.logFullAuth);
                osMemcpy((char_t *)&uri[ruid_begin], tonieInfo->json.cloud_ruid, osStrlen(tonieInfo->json.cloud_ruid));
                TRACE_INFO("Serve cloud from alternative rUID %s, auth %s\r\n", tonieInfo->json.cloud_ruid, msg);
            }

            connection->response.keepAlive = true;
            cbr_ctx_t ctx;
            req_cbr_t cbr = getCloudCbr(connection, uri, queryString, api, &ctx, client_ctx);
            ctx.tonieInfo = tonieInfo;
            if (api == V3_CHAPTER)
            {
                cloud_request_tb2_get(NULL, 0, uri, queryString, token, &cbr);
            }
            else
            {
                cloud_request_get(NULL, 0, uri, queryString, token, &cbr);
            }
            error = NO_ERROR;
        }
    }
    if (error == NO_ERROR && connection->response.statusCode != 404)
    {
        freshness_clear_cache_after_content_request(client_ctx, api, ruid);
    }
    freeTonieInfo(tonieInfo);
    return error;
}

error_t handleCloudContent(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx, bool_t noPassword)
{
    return handleCloudContentExt(connection, uri, queryString, client_ctx, noPassword, 12, V2_CONTENT);
}


error_t handleCloudContentV1(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    return handleCloudContent(connection, uri, queryString, client_ctx, TRUE);
}

error_t handleCloudContentV2(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    if (connection->request.auth.found && connection->request.auth.mode == HTTP_AUTH_MODE_DIGEST)
    {
        return handleCloudContent(connection, uri, queryString, client_ctx, FALSE);
    }
    else
    {
        TRACE_WARNING("Missing auth for content v2: %s\r\n", uri);
    }
    return NO_ERROR;
}

void checkAudioIdForCustom(bool_t *isCustom, char date_buffer[32], time_t audioId);
void checkAudioIdForCustom(bool_t *isCustom, char date_buffer[32], time_t audioId)
{
    struct tm tm_info;
    time_t unix_time = audioId;

    *isCustom = false;
    if (unix_time < 0x0e000000)
    {
        osSprintf(date_buffer, "special");
    }
    else
    {
        /* custom tonies from teddyBench have the audio id reduced by a constant */
        if (unix_time < TEDDY_BENCH_AUDIO_ID_DEDUCT)
        {
            unix_time += TEDDY_BENCH_AUDIO_ID_DEDUCT;
            *isCustom = true;
        }
        if (localtime_r(&unix_time, &tm_info) == 0)
        {
            osSprintf(date_buffer, "(localtime failed)");
        }
        else
        {
            strftime(date_buffer, 32, "%Y-%m-%d %H:%M:%S", &tm_info);
        }
    }
}

static bool_t tap_should_force_dynamic_freshness(tonie_info_t *tonieInfo)
{
    if (tonieInfo == NULL || tonieInfo->json._source_type != CT_SOURCE_TAP_STREAM)
    {
        return false;
    }

    tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
    if (!tap->_valid)
    {
        return false;
    }

    return tap->audio_id == 0 || tap->shuffle != TAP_SHUFFLE_NONE;
}

static bool_t tap_is_stable_cached_playlist(tonie_info_t *tonieInfo)
{
    if (tonieInfo == NULL || tonieInfo->json._source_type != CT_SOURCE_TAP_CACHED)
    {
        return false;
    }

    tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
    if (!tap->_valid || tap->audio_id == 0 || tap->shuffle != TAP_SHUFFLE_NONE || tap->_filepath_resolved == NULL)
    {
        return false;
    }

    if (tonieInfo->tafHeader == NULL || tonieInfo->tafHeader->audio_id != tap->audio_id)
    {
        return false;
    }

    return fsFileExists(tap->_filepath_resolved) && toniefile_is_valid(tap->_filepath_resolved);
}

#if (TRACE_LEVEL >= TRACE_LEVEL_VERBOSE)
static const char *content_source_type_name(ct_source_t source_type)
{
    switch (source_type)
    {
        case CT_SOURCE_NONE: return "none";
        case CT_SOURCE_TAF: return "taf";
        case CT_SOURCE_TAF_INCOMPLETE: return "taf_incomplete";
        case CT_SOURCE_TAP_STREAM: return "tap_stream";
        case CT_SOURCE_TAP_CACHED: return "tap_cached";
        case CT_SOURCE_STREAM: return "stream";
        case CT_SOURCE_NATIVE_COLLECTION: return "native_collection";
        case CT_SOURCE_TONIEPLAY_COLLECTION: return "tonieplay_collection";
        default: return "unknown";
    }
}
#endif

typedef struct
{
    uint32_t boxAudioId;
    uint32_t serverAudioId;
    uint32_t effectiveServerAudioIdRaw;
    uint32_t naturalServerAudioIdRaw;
    bool_t custom_box;
    bool_t custom_server;
    bool_t serverAudioIdAvailable;
    bool_t forced_server_audio_id;
    bool_t stable_cached_tap;
    bool_t tap_dynamic_freshness;
    bool_t tap_final_needs_rebuild;
    bool_t updated_freshness;
    bool_t stream_freshness;
    bool_t isFlex;
    bool_t should_mark_freshness;
    char date_buffer_box[32];
    char date_buffer_server[32];
} freshness_decision_t;

static bool_t freshness_is_hex_string(const char *value, size_t len)
{
    if (value == NULL || osStrlen(value) != len)
    {
        return FALSE;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t freshness_ruid_to_uid(const char *ruid, uint64_t *uid)
{
    if (!freshness_is_hex_string(ruid, 16) || uid == NULL)
    {
        return FALSE;
    }

    *uid = bswap_64(strtoull(ruid, NULL, 16));
    return TRUE;
}

static void freshness_uid_to_ruid(uint64_t uid, char ruid[17])
{
    osSnprintf(ruid, 17, "%016" PRIX64, bswap_64(uid));
}

static bool_t freshness_uid_array_contains(const uint64_t *items, size_t len, uint64_t uid)
{
    if (items == NULL)
    {
        return FALSE;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (items[i] == uid)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static bool_t freshness_settings_array_contains(settings_t *settings, const char *key, uint64_t uid)
{
    if (settings == NULL || key == NULL)
    {
        return FALSE;
    }

    size_t len = 0;
    uint64_t *items = settings_get_u64_array_id(key, settings->internal.overlayNumber, &len);
    return freshness_uid_array_contains(items, len, uid);
}

static bool_t freshness_settings_array_add_uid(settings_t *settings, const char *key, uint64_t uid, bool_t *added)
{
    if (added != NULL)
    {
        *added = FALSE;
    }
    if (settings == NULL || key == NULL)
    {
        return FALSE;
    }

    size_t len = 0;
    uint64_t *items = settings_get_u64_array_id(key, settings->internal.overlayNumber, &len);
    if (freshness_uid_array_contains(items, len, uid))
    {
        return TRUE;
    }

    uint64_t *updated = osAllocMem(sizeof(uint64_t) * (len + 1));
    if (updated == NULL)
    {
        TRACE_ERROR("Could not allocate V3 freshness cache update\r\n");
        return FALSE;
    }

    if (len > 0 && items != NULL)
    {
        osMemcpy(updated, items, sizeof(uint64_t) * len);
    }
    updated[len] = uid;

    bool_t ok = settings_set_u64_array_id(key, updated, len + 1, settings->internal.overlayNumber);
    osFreeMem(updated);
    if (!ok)
    {
        return FALSE;
    }

    if (added != NULL)
    {
        *added = TRUE;
    }
    return TRUE;
}

static bool_t freshness_settings_array_remove_uid(settings_t *settings, const char *key, uint64_t uid)
{
    if (settings == NULL || key == NULL)
    {
        return FALSE;
    }

    size_t len = 0;
    uint64_t *items = settings_get_u64_array_id(key, settings->internal.overlayNumber, &len);
    if (!freshness_uid_array_contains(items, len, uid))
    {
        return FALSE;
    }

    if (len <= 1)
    {
        return settings_set_u64_array_id(key, NULL, 0, settings->internal.overlayNumber);
    }

    uint64_t *updated = osAllocMem(sizeof(uint64_t) * (len - 1));
    if (updated == NULL)
    {
        TRACE_ERROR("Could not allocate V3 freshness cache removal\r\n");
        return FALSE;
    }

    size_t target = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (items[i] != uid)
        {
            updated[target++] = items[i];
        }
    }

    bool_t ok = settings_set_u64_array_id(key, updated, target, settings->internal.overlayNumber);
    osFreeMem(updated);
    return ok;
}

static bool_t freshness_response_add_uid(TonieFreshnessCheckResponse *freshResp, size_t capacity, uint64_t uid)
{
    if (freshResp == NULL)
    {
        return FALSE;
    }

    for (size_t i = 0; i < freshResp->n_tonie_marked; i++)
    {
        if (freshResp->tonie_marked[i] == uid)
        {
            return TRUE;
        }
    }

    if (freshResp->n_tonie_marked >= capacity)
    {
        TRACE_WARNING("Could not add UID %016" PRIX64 " to freshnessCheck response, no free slot\r\n", uid);
        return FALSE;
    }

    freshResp->tonie_marked[freshResp->n_tonie_marked++] = uid;
    return TRUE;
}

static bool_t freshness_cache_add_uid(settings_t *settings, uint64_t uid, bool_t *added)
{
    bool_t ok = freshness_settings_array_add_uid(settings, "internal.freshnessCache", uid, added);
    if (ok)
    {
        settings_set_bool_id("internal.freshnessCacheChanged", true, settings->internal.overlayNumber);
    }
    return ok;
}

static bool_t freshness_cache_source_changed_contains(settings_t *settings, uint64_t uid)
{
    return freshness_settings_array_contains(settings, "internal.freshnessCacheSourceChangedUids", uid);
}

static bool_t freshness_cache_add_source_changed_uid(settings_t *settings, uint64_t uid)
{
    return freshness_settings_array_add_uid(settings, "internal.freshnessCacheSourceChangedUids", uid, NULL);
}

static bool_t freshness_cache_remove_source_changed_uid(settings_t *settings, uint64_t uid)
{
    return freshness_settings_array_remove_uid(settings, "internal.freshnessCacheSourceChangedUids", uid);
}

void freshness_cache_sync_source_changed_uids(settings_t *settings)
{
    if (settings == NULL)
    {
        return;
    }

    size_t freshnessCacheLen = 0;
    size_t sourceChangedLen = 0;
    uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    uint64_t *sourceChanged = settings_get_u64_array_id("internal.freshnessCacheSourceChangedUids", settings->internal.overlayNumber, &sourceChangedLen);
    if (sourceChangedLen == 0)
    {
        return;
    }

    if (freshnessCacheLen == 0 || freshnessCache == NULL)
    {
        settings_set_u64_array_id("internal.freshnessCacheSourceChangedUids", NULL, 0, settings->internal.overlayNumber);
        return;
    }

    uint64_t *updated = osAllocMem(sizeof(uint64_t) * sourceChangedLen);
    if (updated == NULL)
    {
        TRACE_ERROR("Could not allocate V3 freshness source-change cache sync\r\n");
        return;
    }

    size_t target = 0;
    for (size_t i = 0; i < sourceChangedLen; i++)
    {
        if (freshness_uid_array_contains(freshnessCache, freshnessCacheLen, sourceChanged[i]))
        {
            updated[target++] = sourceChanged[i];
        }
    }

    settings_set_u64_array_id("internal.freshnessCacheSourceChangedUids",
                              target > 0 ? updated : NULL,
                              target,
                              settings->internal.overlayNumber);
    osFreeMem(updated);
}

static bool_t freshness_cache_remove_uid(settings_t *settings, uint64_t uid)
{
    if (settings == NULL)
    {
        return FALSE;
    }

    if (!freshness_settings_array_remove_uid(settings, "internal.freshnessCache", uid))
    {
        return FALSE;
    }
    freshness_cache_remove_source_changed_uid(settings, uid);

    size_t freshnessCacheLen = 0;
    settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    if (freshnessCacheLen == 0)
    {
        settings_set_bool_id("internal.freshnessCacheChanged", false, settings->internal.overlayNumber);
    }
    return TRUE;
}

static void freshness_clear_cache_after_v3_request(settings_t *settings, const char *ruid)
{
    uint64_t uid = 0;
    if (settings == NULL || !freshness_ruid_to_uid(ruid, &uid))
    {
        return;
    }

    if (freshness_cache_remove_uid(settings, uid))
    {
        TRACE_INFO("Cleared V3 freshness cache entry for rUID %s\r\n", ruid);
    }
}

static void freshness_clear_cache_after_content_request(client_ctx_t *client_ctx, cloudapi_t api, const char *ruid)
{
    if (client_ctx == NULL || client_ctx->settings == NULL)
    {
        return;
    }
    if (api != V3_CONTENT_META && api != V3_CHAPTER)
    {
        return;
    }
    if (freshness_source_changed_contains_ruid(client_ctx->settings, ruid))
    {
        TRACE_DEBUG("Keeping V3 freshness source-change entry for rUID %s until matching playback confirmation\r\n", ruid);
        return;
    }

    freshness_clear_cache_after_v3_request(client_ctx->settings, ruid);
}

static void freshness_store_v3_inventory(settings_t *settings, TonieFreshnessCheckRequest *freshReq)
{
    if (settings == NULL || freshReq == NULL || freshReq->n_tonie_infos == 0)
    {
        if (settings != NULL)
        {
            settings_set_u64_array_id("internal.v3FreshnessInventoryUids", NULL, 0, settings->internal.overlayNumber);
            settings_set_u64_array_id("internal.v3FreshnessInventoryAudioIds", NULL, 0, settings->internal.overlayNumber);
            TRACE_INFO("Stored V3 freshness inventory with 0 entries\r\n");
        }
        return;
    }

    uint64_t *uids = osAllocMem(sizeof(uint64_t) * freshReq->n_tonie_infos);
    uint64_t *audioIds = osAllocMem(sizeof(uint64_t) * freshReq->n_tonie_infos);
    if (uids == NULL || audioIds == NULL)
    {
        osFreeMem(uids);
        osFreeMem(audioIds);
        settings_set_u64_array_id("internal.v3FreshnessInventoryUids", NULL, 0, settings->internal.overlayNumber);
        settings_set_u64_array_id("internal.v3FreshnessInventoryAudioIds", NULL, 0, settings->internal.overlayNumber);
        TRACE_ERROR("Could not store V3 freshness inventory\r\n");
        return;
    }

    for (size_t i = 0; i < freshReq->n_tonie_infos; i++)
    {
        uids[i] = freshReq->tonie_infos[i]->uid;
        audioIds[i] = freshReq->tonie_infos[i]->audio_id;
    }

    bool_t storedUids = settings_set_u64_array_id("internal.v3FreshnessInventoryUids", uids, freshReq->n_tonie_infos, settings->internal.overlayNumber);
    bool_t storedAudioIds = settings_set_u64_array_id("internal.v3FreshnessInventoryAudioIds", audioIds, freshReq->n_tonie_infos, settings->internal.overlayNumber);
    osFreeMem(uids);
    osFreeMem(audioIds);

    if (!storedUids || !storedAudioIds)
    {
        settings_set_u64_array_id("internal.v3FreshnessInventoryUids", NULL, 0, settings->internal.overlayNumber);
        settings_set_u64_array_id("internal.v3FreshnessInventoryAudioIds", NULL, 0, settings->internal.overlayNumber);
        TRACE_ERROR("Could not store V3 freshness inventory\r\n");
        return;
    }

    TRACE_INFO("Stored V3 freshness inventory with %" PRIuSIZE " entries\r\n", freshReq->n_tonie_infos);
}

static bool_t freshness_inventory_find_uid(settings_t *settings, uint64_t uid, uint32_t *audio_id)
{
    if (settings == NULL || audio_id == NULL)
    {
        return FALSE;
    }

    size_t uidLen = 0;
    size_t audioIdLen = 0;
    uint64_t *uids = settings_get_u64_array_id("internal.v3FreshnessInventoryUids", settings->internal.overlayNumber, &uidLen);
    uint64_t *audioIds = settings_get_u64_array_id("internal.v3FreshnessInventoryAudioIds", settings->internal.overlayNumber, &audioIdLen);
    if (uids == NULL || audioIds == NULL || uidLen == 0 || uidLen != audioIdLen)
    {
        return FALSE;
    }

    for (size_t i = 0; i < uidLen; i++)
    {
        if (uids[i] == uid)
        {
            *audio_id = (uint32_t)audioIds[i];
            return TRUE;
        }
    }
    return FALSE;
}

static uint32_t freshness_audio_id_compare_value(uint32_t audio_id, bool_t *isCustom, char date_buffer[32])
{
    bool_t localCustom = FALSE;
    char localDateBuffer[32];
    char *targetDateBuffer = date_buffer != NULL ? date_buffer : localDateBuffer;

    checkAudioIdForCustom(&localCustom, targetDateBuffer, audio_id);
    if (isCustom != NULL)
    {
        *isCustom = localCustom;
    }
    if (localCustom)
    {
        return audio_id + TEDDY_BENCH_AUDIO_ID_DEDUCT;
    }
    return audio_id;
}

static bool_t freshness_get_natural_server_audio_id(tonie_info_t *tonieInfo, uint32_t *audio_id, bool_t *tap_has_audio_id)
{
    if (tap_has_audio_id != NULL)
    {
        *tap_has_audio_id = FALSE;
    }
    if (audio_id == NULL || tonieInfo == NULL)
    {
        return FALSE;
    }

    if (tonieInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION)
    {
        return v3_native_library_source_audio_id(tonieInfo->json.source,
                                                  audio_id);
    }
    if (tonieInfo->json._source_type == CT_SOURCE_TONIEPLAY_COLLECTION &&
        tonieInfo->json._version != 0)
    {
        *audio_id = tonieInfo->json._version;
        return TRUE;
    }

    bool_t tap_freshness = (tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM || tonieInfo->json._source_type == CT_SOURCE_TAP_CACHED) &&
                           tonieInfo->json._tap._valid;
    tonie_audio_playlist_t *tap = tap_freshness ? &tonieInfo->json._tap : NULL;
    if (tap_freshness && tap->audio_id != 0)
    {
        *audio_id = (uint32_t)tap->audio_id;
        if (tap_has_audio_id != NULL)
        {
            *tap_has_audio_id = TRUE;
        }
        return TRUE;
    }

    if (tonieInfo->valid && tonieInfo->tafHeader != NULL)
    {
        *audio_id = tonieInfo->tafHeader->audio_id;
        return TRUE;
    }

    return FALSE;
}

static void freshness_forced_version_clear(settings_t *settings)
{
    if (settings == NULL)
    {
        return;
    }
    settings_set_u64_array_id("internal.v3ForcedVersionUids", NULL, 0, settings->internal.overlayNumber);
    settings_set_u64_array_id("internal.v3ForcedVersions", NULL, 0, settings->internal.overlayNumber);
    settings_set_u64_array_id("internal.v3ForcedVersionBaseAudioIds", NULL, 0, settings->internal.overlayNumber);
}

static bool_t freshness_forced_version_remove(settings_t *settings, uint64_t uid)
{
    if (settings == NULL)
    {
        return FALSE;
    }

    size_t uidLen = 0;
    size_t versionLen = 0;
    size_t baseLen = 0;
    uint64_t *uids = settings_get_u64_array_id("internal.v3ForcedVersionUids", settings->internal.overlayNumber, &uidLen);
    uint64_t *versions = settings_get_u64_array_id("internal.v3ForcedVersions", settings->internal.overlayNumber, &versionLen);
    uint64_t *baseAudioIds = settings_get_u64_array_id("internal.v3ForcedVersionBaseAudioIds", settings->internal.overlayNumber, &baseLen);
    if (uidLen == 0)
    {
        return FALSE;
    }
    if (uids == NULL || versions == NULL || baseAudioIds == NULL || uidLen != versionLen || uidLen != baseLen)
    {
        freshness_forced_version_clear(settings);
        return FALSE;
    }

    size_t removeIndex = uidLen;
    for (size_t i = 0; i < uidLen; i++)
    {
        if (uids[i] == uid)
        {
            removeIndex = i;
            break;
        }
    }
    if (removeIndex == uidLen)
    {
        return FALSE;
    }

    if (uidLen <= 1)
    {
        freshness_forced_version_clear(settings);
        return TRUE;
    }

    uint64_t *updatedUids = osAllocMem(sizeof(uint64_t) * (uidLen - 1));
    uint64_t *updatedVersions = osAllocMem(sizeof(uint64_t) * (uidLen - 1));
    uint64_t *updatedBases = osAllocMem(sizeof(uint64_t) * (uidLen - 1));
    if (updatedUids == NULL || updatedVersions == NULL || updatedBases == NULL)
    {
        osFreeMem(updatedUids);
        osFreeMem(updatedVersions);
        osFreeMem(updatedBases);
        TRACE_ERROR("Could not allocate V3 forced-version removal\r\n");
        return FALSE;
    }

    size_t target = 0;
    for (size_t i = 0; i < uidLen; i++)
    {
        if (i == removeIndex)
        {
            continue;
        }
        updatedUids[target] = uids[i];
        updatedVersions[target] = versions[i];
        updatedBases[target] = baseAudioIds[i];
        target++;
    }

    bool_t ok = settings_set_u64_array_id("internal.v3ForcedVersionUids", updatedUids, target, settings->internal.overlayNumber) &&
                settings_set_u64_array_id("internal.v3ForcedVersions", updatedVersions, target, settings->internal.overlayNumber) &&
                settings_set_u64_array_id("internal.v3ForcedVersionBaseAudioIds", updatedBases, target, settings->internal.overlayNumber);
    osFreeMem(updatedUids);
    osFreeMem(updatedVersions);
    osFreeMem(updatedBases);
    if (!ok)
    {
        freshness_forced_version_clear(settings);
    }
    return ok;
}

static bool_t freshness_forced_version_get(settings_t *settings, uint64_t uid,
                                           uint32_t *forcedAudioId, uint32_t *baseAudioId)
{
    if (settings == NULL || forcedAudioId == NULL)
    {
        return FALSE;
    }

    size_t uidLen = 0;
    size_t versionLen = 0;
    size_t baseLen = 0;
    uint64_t *uids = settings_get_u64_array_id("internal.v3ForcedVersionUids", settings->internal.overlayNumber, &uidLen);
    uint64_t *versions = settings_get_u64_array_id("internal.v3ForcedVersions", settings->internal.overlayNumber, &versionLen);
    uint64_t *baseAudioIds = settings_get_u64_array_id("internal.v3ForcedVersionBaseAudioIds", settings->internal.overlayNumber, &baseLen);
    if (uidLen == 0)
    {
        return FALSE;
    }
    if (uids == NULL || versions == NULL || baseAudioIds == NULL || uidLen != versionLen || uidLen != baseLen)
    {
        freshness_forced_version_clear(settings);
        return FALSE;
    }

    for (size_t i = 0; i < uidLen; i++)
    {
        if (uids[i] != uid)
        {
            continue;
        }

        *forcedAudioId = (uint32_t)versions[i];
        if (baseAudioId != NULL)
        {
            *baseAudioId = (uint32_t)baseAudioIds[i];
        }
        return TRUE;
    }
    return FALSE;
}

static bool_t freshness_forced_version_find(settings_t *settings, uint64_t uid,
                                            uint32_t naturalServerAudioId,
                                            uint32_t *forcedAudioId)
{
    uint32_t baseAudioId = 0;
    if (!freshness_forced_version_get(settings, uid, forcedAudioId, &baseAudioId))
    {
        return FALSE;
    }

    if (freshness_audio_id_compare_value(baseAudioId, NULL, NULL) !=
        freshness_audio_id_compare_value(naturalServerAudioId, NULL, NULL))
    {
        freshness_forced_version_remove(settings, uid);
        return FALSE;
    }
    return TRUE;
}

static bool_t freshness_forced_version_set(settings_t *settings, uint64_t uid, uint32_t forcedAudioId, uint32_t baseAudioId)
{
    if (settings == NULL)
    {
        return FALSE;
    }

    size_t uidLen = 0;
    size_t versionLen = 0;
    size_t baseLen = 0;
    uint64_t *uids = settings_get_u64_array_id("internal.v3ForcedVersionUids", settings->internal.overlayNumber, &uidLen);
    uint64_t *versions = settings_get_u64_array_id("internal.v3ForcedVersions", settings->internal.overlayNumber, &versionLen);
    uint64_t *baseAudioIds = settings_get_u64_array_id("internal.v3ForcedVersionBaseAudioIds", settings->internal.overlayNumber, &baseLen);
    if (uidLen != versionLen || uidLen != baseLen ||
        (uidLen > 0 && (uids == NULL || versions == NULL || baseAudioIds == NULL)))
    {
        freshness_forced_version_clear(settings);
        uidLen = 0;
        uids = NULL;
        versions = NULL;
        baseAudioIds = NULL;
    }

    size_t targetLen = uidLen;
    size_t updateIndex = uidLen;
    for (size_t i = 0; i < uidLen; i++)
    {
        if (uids[i] == uid)
        {
            updateIndex = i;
            break;
        }
    }
    if (updateIndex == uidLen)
    {
        targetLen++;
    }

    uint64_t *updatedUids = osAllocMem(sizeof(uint64_t) * targetLen);
    uint64_t *updatedVersions = osAllocMem(sizeof(uint64_t) * targetLen);
    uint64_t *updatedBases = osAllocMem(sizeof(uint64_t) * targetLen);
    if (updatedUids == NULL || updatedVersions == NULL || updatedBases == NULL)
    {
        osFreeMem(updatedUids);
        osFreeMem(updatedVersions);
        osFreeMem(updatedBases);
        TRACE_ERROR("Could not allocate V3 forced-version update\r\n");
        return FALSE;
    }

    for (size_t i = 0; i < uidLen; i++)
    {
        updatedUids[i] = uids[i];
        updatedVersions[i] = versions[i];
        updatedBases[i] = baseAudioIds[i];
    }
    updatedUids[updateIndex] = uid;
    updatedVersions[updateIndex] = forcedAudioId;
    updatedBases[updateIndex] = baseAudioId;

    bool_t ok = settings_set_u64_array_id("internal.v3ForcedVersionUids", updatedUids, targetLen, settings->internal.overlayNumber) &&
                settings_set_u64_array_id("internal.v3ForcedVersions", updatedVersions, targetLen, settings->internal.overlayNumber) &&
                settings_set_u64_array_id("internal.v3ForcedVersionBaseAudioIds", updatedBases, targetLen, settings->internal.overlayNumber);
    osFreeMem(updatedUids);
    osFreeMem(updatedVersions);
    osFreeMem(updatedBases);
    if (!ok)
    {
        freshness_forced_version_clear(settings);
    }
    return ok;
}

static bool_t freshness_new_forced_version(uint32_t naturalServerAudioId,
                                           bool_t boxAudioIdKnown,
                                           uint32_t boxAudioId,
                                           bool_t previousVersionKnown,
                                           uint32_t previousVersion,
                                           uint32_t *forcedAudioId)
{
    if (forcedAudioId == NULL)
    {
        return FALSE;
    }

    uint32_t candidate = (uint32_t)time(NULL);
    uint32_t naturalCompare = freshness_audio_id_compare_value(naturalServerAudioId, NULL, NULL);
    uint32_t minimum = naturalCompare > naturalServerAudioId ? naturalCompare : naturalServerAudioId;
    if (boxAudioIdKnown)
    {
        uint32_t boxCompare = freshness_audio_id_compare_value(boxAudioId, NULL, NULL);
        if (boxCompare > minimum)
        {
            minimum = boxCompare;
        }
        if (boxAudioId > minimum)
        {
            minimum = boxAudioId;
        }
    }
    if (previousVersionKnown && previousVersion > minimum)
    {
        minimum = previousVersion;
    }

    if (candidate <= minimum)
    {
        if (minimum == UINT32_MAX)
        {
            TRACE_ERROR("Could not allocate a newer V3 content version after %" PRIu32 "\r\n", minimum);
            return FALSE;
        }
        candidate = minimum + 1;
    }

    *forcedAudioId = candidate;
    return TRUE;
}

static bool_t freshness_get_effective_server_audio_id(settings_t *settings, tonie_info_t *tonieInfo, uint64_t uid, uint32_t *audio_id, bool_t *tap_has_audio_id, bool_t *forced)
{
    uint32_t naturalAudioId = 0;
    if (forced != NULL)
    {
        *forced = FALSE;
    }
    if (!freshness_get_natural_server_audio_id(tonieInfo, &naturalAudioId, tap_has_audio_id))
    {
        return FALSE;
    }

    uint32_t forcedAudioId = 0;
    if (freshness_forced_version_find(settings, uid, naturalAudioId, &forcedAudioId))
    {
        *audio_id = forcedAudioId;
        if (forced != NULL)
        {
            *forced = TRUE;
        }
        return TRUE;
    }

    *audio_id = naturalAudioId;
    return TRUE;
}

static bool_t freshness_set_forced_version_for_source_change(settings_t *settings,
                                                             tonie_info_t *tonieInfo,
                                                             uint64_t uid,
                                                             bool_t boxAudioIdKnown,
                                                             uint32_t boxAudioId)
{
    uint32_t naturalAudioId = 0;
    if (!freshness_get_natural_server_audio_id(tonieInfo, &naturalAudioId, NULL))
    {
        return FALSE;
    }

    uint32_t previousForcedAudioId = 0;
    bool_t previousVersionKnown = freshness_forced_version_get(settings, uid,
                                                               &previousForcedAudioId,
                                                               NULL);
    uint32_t newForcedAudioId = 0;
    if (!freshness_new_forced_version(naturalAudioId,
                                      boxAudioIdKnown,
                                      boxAudioId,
                                      previousVersionKnown,
                                      previousForcedAudioId,
                                      &newForcedAudioId))
    {
        return FALSE;
    }

    return freshness_forced_version_set(settings, uid, newForcedAudioId, naturalAudioId);
}

static bool_t freshness_source_changed_contains_ruid(settings_t *settings, const char *ruid)
{
    uint64_t uid = 0;
    return freshness_ruid_to_uid(ruid, &uid) && freshness_cache_source_changed_contains(settings, uid);
}

static const char *freshness_v3_resume_behavior(settings_t *settings, contentJson_t *content_json, const char *ruid)
{
    if ((content_json != NULL && content_json->live) || freshness_source_changed_contains_ruid(settings, ruid))
    {
        return "alwaysReset";
    }
    return "resume";
}

static uint32_t freshness_v3_content_meta_version(settings_t *settings, tonie_info_t *tonieInfo, const char *ruid, uint32_t fallbackAudioId)
{
    uint64_t uid = 0;
    uint32_t effectiveAudioId = fallbackAudioId;
    if (!freshness_ruid_to_uid(ruid, &uid))
    {
        return fallbackAudioId;
    }

    bool_t forced = FALSE;
    if (freshness_get_effective_server_audio_id(settings, tonieInfo, uid,
                                                &effectiveAudioId, NULL, &forced))
    {
        if (!forced && freshness_cache_source_changed_contains(settings, uid))
        {
            uint32_t boxAudioId = 0;
            bool_t boxAudioIdKnown = freshness_inventory_find_uid(settings, uid,
                                                                   &boxAudioId);
            if (!freshness_set_forced_version_for_source_change(settings,
                                                                tonieInfo,
                                                                uid,
                                                                boxAudioIdKnown,
                                                                boxAudioId) ||
                !freshness_get_effective_server_audio_id(settings, tonieInfo, uid,
                                                         &effectiveAudioId, NULL, NULL))
            {
                char canonicalRuid[17];
                freshness_uid_to_ruid(uid, canonicalRuid);
                TRACE_ERROR("Could not allocate the pending V3 content version for rUID %s\r\n",
                            canonicalRuid);
                return 0;
            }
        }
        return effectiveAudioId;
    }
    return fallbackAudioId;
}

bool_t freshness_confirm_v3_content_version(settings_t *settings, const char *ruid,
                                            uint64_t content_version)
{
    uint64_t uid = 0;
    if (settings == NULL || settings->toniebox.boxGeneration != GENERATION_TB2 ||
        !freshness_ruid_to_uid(ruid, &uid) ||
        !freshness_cache_source_changed_contains(settings, uid))
    {
        return FALSE;
    }

    tonie_info_t *tonieInfo = getTonieInfoFromUid(uid, false, settings);
    uint32_t expectedVersion = freshness_v3_content_meta_version(settings,
                                                                  tonieInfo,
                                                                  ruid,
                                                                  0);
    bool_t expectedVersionAvailable = expectedVersion != 0;
    if (tonieInfo != NULL)
    {
        freeTonieInfo(tonieInfo);
    }

    char canonicalRuid[17];
    freshness_uid_to_ruid(uid, canonicalRuid);
    if (!expectedVersionAvailable)
    {
        TRACE_DEBUG("Ignoring V3 playback confirmation for rUID %s: received=%" PRIu64 ", expected version unavailable\r\n",
                    canonicalRuid, content_version);
        return FALSE;
    }
    if (content_version != expectedVersion)
    {
        TRACE_DEBUG("Ignoring stale V3 playback confirmation for rUID %s: received=%" PRIu64 ", expected=%" PRIu32 "\r\n",
                    canonicalRuid, content_version, expectedVersion);
        return FALSE;
    }

    if (!freshness_cache_remove_uid(settings, uid))
    {
        TRACE_WARNING("Could not clear confirmed V3 freshness entry for rUID %s\r\n",
                      canonicalRuid);
        return FALSE;
    }

    TRACE_INFO("Confirmed V3 content version for rUID %s via MQTT playback: %" PRIu32 "\r\n",
               canonicalRuid, expectedVersion);
    return TRUE;
}

static void freshness_evaluate_tonie(settings_t *settings, tonie_info_t *tonieInfo, uint64_t uid, uint32_t boxAudioIdRaw, bool_t force_stale, bool_t log_details, freshness_decision_t *decision)
{
    osMemset(decision, 0, sizeof(*decision));
    decision->boxAudioId = freshness_audio_id_compare_value(boxAudioIdRaw, &decision->custom_box, decision->date_buffer_box);

    if (tonieInfo == NULL || settings == NULL)
    {
        decision->should_mark_freshness = force_stale;
        return;
    }

    bool_t tap_freshness = (tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM || tonieInfo->json._source_type == CT_SOURCE_TAP_CACHED) &&
                           tonieInfo->json._tap._valid;
    tonie_audio_playlist_t *tap = tap_freshness ? &tonieInfo->json._tap : NULL;
    bool_t tap_has_audio_id = FALSE;
    uint32_t naturalServerAudioId = 0;

    if (freshness_get_natural_server_audio_id(tonieInfo, &naturalServerAudioId, &tap_has_audio_id))
    {
        uint32_t effectiveServerAudioId = naturalServerAudioId;
        uint32_t forcedServerAudioId = 0;
        decision->naturalServerAudioIdRaw = naturalServerAudioId;
        if (freshness_forced_version_find(settings, uid, naturalServerAudioId, &forcedServerAudioId))
        {
            effectiveServerAudioId = forcedServerAudioId;
            decision->forced_server_audio_id = TRUE;
        }

        decision->effectiveServerAudioIdRaw = effectiveServerAudioId;
        decision->serverAudioId = freshness_audio_id_compare_value(effectiveServerAudioId, &decision->custom_server, decision->date_buffer_server);
        decision->serverAudioIdAvailable = TRUE;

        const bool_t use_tb1_audio_id_policy =
            settings->toniebox.boxGeneration != GENERATION_TB2;
        if (!use_tb1_audio_id_policy)
        {
            tonieInfo->updated = boxAudioIdRaw != effectiveServerAudioId;
        }
        else if (tap_has_audio_id)
        {
            tonieInfo->updated = decision->boxAudioId != decision->serverAudioId;
        }
        else
        {
            tonieInfo->updated = decision->boxAudioId < decision->serverAudioId;
            tonieInfo->updated = tonieInfo->updated ||
                                 (use_tb1_audio_id_policy &&
                                  settings->cloud.updateOnLowerAudioId &&
                                  decision->boxAudioId > decision->serverAudioId);
        }
        if (use_tb1_audio_id_policy && settings->cloud.prioCustomContent &&
            !settings->cloud.updateOnLowerAudioId)
        {
            if (decision->custom_box && !decision->custom_server)
            {
                tonieInfo->updated = false;
            }
            if (!decision->custom_box && decision->custom_server)
            {
                tonieInfo->updated = true;
            }
        }
    }

    if (!tonieInfo->valid)
    {
        content_json_update_model(&tonieInfo->json, boxAudioIdRaw, NULL);
    }

    char uidText[17];
    osSprintf(uidText, "%016" PRIX64, uid);
    if (settings->core.flex_enabled && !osStrcasecmp(settings->core.flex_uid, uidText))
    {
        decision->isFlex = TRUE;
    }

    decision->stable_cached_tap = tap_is_stable_cached_playlist(tonieInfo);
    decision->tap_dynamic_freshness = tap_should_force_dynamic_freshness(tonieInfo);
    decision->tap_final_needs_rebuild = tap_has_audio_id &&
                                        tap->shuffle == TAP_SHUFFLE_NONE &&
                                        (!tonieInfo->valid ||
                                         tonieInfo->tafHeader == NULL ||
                                         tonieInfo->tafHeader->audio_id != tap->audio_id);
    decision->updated_freshness = tap_freshness ?
                                  (tonieInfo->updated || decision->tap_final_needs_rebuild) :
                                  (tonieInfo->updated && !decision->stable_cached_tap);
    decision->stream_freshness = tonieInfo->json._source_type == CT_SOURCE_STREAM;
    decision->should_mark_freshness = force_stale ||
                                      tonieInfo->json.live ||
                                      decision->updated_freshness ||
                                      decision->stream_freshness ||
                                      decision->tap_dynamic_freshness ||
                                      decision->isFlex;

    if (!log_details)
    {
        return;
    }

    (void)decision->custom_box;
    (void)decision->custom_server;
    TRACE_INFO("  uid: %016" PRIX64 ", nocloud: %d, live: %d, updated: %d, audioid: %08X (%s%s)",
               uid,
               tonieInfo->json.nocloud,
               tonieInfo->json.live || decision->isFlex || decision->stream_freshness,
               tonieInfo->updated,
               decision->boxAudioId,
               decision->date_buffer_box,
               decision->custom_box ? ", custom" : "");

    if (decision->serverAudioIdAvailable)
    {
        TRACE_INFO_RESUME(", audioid-tc: %08X (%s%s)",
                          decision->serverAudioId,
                          decision->date_buffer_server,
                          decision->forced_server_audio_id ? ", forced" : (decision->custom_server ? ", custom" : ""));
    }
    TRACE_INFO_RESUME("\r\n");

#if (TRACE_LEVEL >= TRACE_LEVEL_VERBOSE)
    if ((tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM) || (tonieInfo->json._source_type == CT_SOURCE_TAP_CACHED))
    {
        tonie_audio_playlist_t *tapInfo = &tonieInfo->json._tap;
        bool_t final_exists = tapInfo->_filepath_resolved != NULL && fsFileExists(tapInfo->_filepath_resolved);
        bool_t final_valid = final_exists && toniefile_is_valid(tapInfo->_filepath_resolved);
        TRACE_VERBOSE("  tap freshness: source=%s tap_audioid=%08X shuffle=%u tap_cached=%u final_exists=%u final_valid=%u stable_cached=%u boxAudioId=%08X serverAudioId=%08X\r\n",
                      content_source_type_name(tonieInfo->json._source_type),
                      (uint32_t)tapInfo->audio_id,
                      (unsigned int)tapInfo->shuffle,
                      tapInfo->_cached,
                      final_exists,
                      final_valid,
                      decision->stable_cached_tap,
                      decision->boxAudioId,
                      decision->serverAudioId);
    }
#endif
    if (decision->stable_cached_tap && tonieInfo->updated && !decision->updated_freshness)
    {
        TRACE_VERBOSE("  stable cached TAP freshness protected: updated=%u ignored\r\n", tonieInfo->updated);
    }
    if (decision->should_mark_freshness)
    {
        TRACE_VERBOSE("  freshness mark reason: live=%u updated=%u stream=%u dynamic_tap=%u flex=%u cache=%u stable_cached_tap=%u\r\n",
                      tonieInfo->json.live,
                      decision->updated_freshness,
                      decision->stream_freshness,
                      decision->tap_dynamic_freshness,
                      decision->isFlex,
                      force_stale,
                      decision->stable_cached_tap);
    }
}

void process_freshness_check(client_ctx_t *client_ctx, TonieFreshnessCheckRequest *freshReq, TonieFreshnessCheckResponse *freshResp, TonieFreshnessCheckRequest *freshReqCloud, size_t *freshnessCacheLenOut, bool_t allow_cloud_override)
{
    settings_t *settings = client_ctx->settings;
    TRACE_INFO("Found %" PRIuSIZE " tonies:\n", freshReq->n_tonie_infos);
    freshResp->n_tonie_marked = 0;

    size_t freshnessCacheLen = 0;
    uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", settings->internal.overlayNumber, &freshnessCacheLen);
    if (freshnessCacheLenOut) *freshnessCacheLenOut = freshnessCacheLen;

    size_t freshRespCapacity = freshReq->n_tonie_infos + freshnessCacheLen;
    freshResp->tonie_marked = malloc(sizeof(uint64_t) * freshRespCapacity);
    if (freshRespCapacity > 0 && freshResp->tonie_marked == NULL)
    {
        TRACE_ERROR("Could not allocate freshnessCheck response\r\n");
        freshReqCloud->n_tonie_infos = 0;
        freshReqCloud->tonie_infos = NULL;
        return;
    }

    uint64_t *boxCorrectedUids = NULL;
    size_t boxCorrectedLen = 0;
    if (freshReq->n_tonie_infos > 0)
    {
        boxCorrectedUids = malloc(sizeof(uint64_t) * freshReq->n_tonie_infos);
        if (boxCorrectedUids == NULL)
        {
            TRACE_ERROR("Could not allocate V3 freshness inventory correction list\r\n");
        }
    }

    for (size_t i = 0; i < freshnessCacheLen; i++)
    {
        bool_t requested = FALSE;
        for (size_t j = 0; j < freshReq->n_tonie_infos; j++)
        {
            if (freshReq->tonie_infos[j]->uid == freshnessCache[i])
            {
                requested = TRUE;
                break;
            }
        }
        if (!requested)
        {
            freshness_response_add_uid(freshResp, freshRespCapacity, freshnessCache[i]);
        }
    }

    freshReqCloud->n_tonie_infos = 0;
    freshReqCloud->tonie_infos = malloc(sizeof(TonieFCInfo *) * freshReq->n_tonie_infos);
    if (freshReq->n_tonie_infos > 0 && freshReqCloud->tonie_infos == NULL)
    {
        TRACE_ERROR("Could not allocate freshnessCheck cloud request\r\n");
        return;
    }

    for (size_t i = 0; i < freshReq->n_tonie_infos; i++)
    {
        uint64_t uid = freshReq->tonie_infos[i]->uid;
        bool_t cache_stale = freshness_uid_array_contains(freshnessCache, freshnessCacheLen, uid);
        bool_t source_changed_stale = cache_stale && freshness_cache_source_changed_contains(settings, uid);
        tonie_info_t *tonieInfo = getTonieInfoFromUid(uid, false, settings);
        freshness_decision_t decision;

        freshness_evaluate_tonie(settings, tonieInfo, uid, freshReq->tonie_infos[i]->audio_id, FALSE, TRUE, &decision);

        if (cache_stale && !source_changed_stale && !decision.should_mark_freshness)
        {
            char cruid[17];
            freshness_uid_to_ruid(uid, cruid);
            TRACE_INFO("Accepted V3 freshness inventory for rUID %s, clearing pending freshness update\r\n", cruid);
            if (boxCorrectedUids != NULL)
            {
                boxCorrectedUids[boxCorrectedLen++] = uid;
            }
            cache_stale = FALSE;
        }
        else if (source_changed_stale)
        {
            decision.should_mark_freshness = TRUE;
        }

        bool_t forward_to_cloud = tonieInfo != NULL &&
                                  (!tonieInfo->json.nocloud ||
                                   (allow_cloud_override && tonieInfo->json.cloud_override));
        if (forward_to_cloud)
        {
            freshReqCloud->tonie_infos[freshReqCloud->n_tonie_infos] = freshReq->tonie_infos[i];
            if (decision.serverAudioIdAvailable)
            {
                freshReqCloud->tonie_infos[freshReqCloud->n_tonie_infos]->audio_id = decision.serverAudioId;
            }
            freshReqCloud->n_tonie_infos++;
        }

        if (decision.should_mark_freshness)
        {
            freshness_response_add_uid(freshResp, freshRespCapacity, uid);
        }
        freeTonieInfo(tonieInfo);
    }

    for (size_t i = 0; i < boxCorrectedLen; i++)
    {
        freshness_cache_remove_uid(settings, boxCorrectedUids[i]);
    }
    free(boxCorrectedUids);
}

static bool_t freshness_mark_content_mapping_changed_for_overlay(settings_t *settings,
                                                                 uint64_t uid,
                                                                 bool_t source_changed,
                                                                 bool_t require_inventory)
{
    if (settings == NULL || !settings->internal.config_used ||
        settings->internal.overlayNumber == 0 ||
        settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        return FALSE;
    }

    uint32_t boxAudioId = 0;
    bool_t inventory_available = freshness_inventory_find_uid(settings, uid, &boxAudioId);
    if (!inventory_available && (require_inventory || !source_changed))
    {
        return FALSE;
    }

    tonie_info_t *tonieInfo = (inventory_available || source_changed)
                                  ? getTonieInfoFromUid(uid, false, settings)
                                  : NULL;
    bool_t forced_version_set = FALSE;
    bool_t should_mark_freshness = source_changed;
    if (source_changed)
    {
        forced_version_set = freshness_set_forced_version_for_source_change(
            settings, tonieInfo, uid, inventory_available, boxAudioId);
    }
    if (inventory_available)
    {
        freshness_decision_t decision;
        freshness_evaluate_tonie(settings, tonieInfo, uid, boxAudioId, FALSE, FALSE,
                                 &decision);
        should_mark_freshness = should_mark_freshness || decision.should_mark_freshness;
    }

    bool_t queued = FALSE;
    if (should_mark_freshness && freshness_cache_add_uid(settings, uid, NULL))
    {
        if (source_changed)
        {
            freshness_cache_add_source_changed_uid(settings, uid);
        }

        char cruid[17];
        freshness_uid_to_ruid(uid, cruid);
        TRACE_INFO("Marked rUID %s for V3 freshness update on overlay %s (%s, inventory=%s)\r\n",
                   cruid,
                   settings->commonName,
                   forced_version_set ? "source changed, forced version" :
                       (source_changed ? "source changed" : "freshness comparison"),
                   inventory_available ? "present" : "absent");
        mqtt_server_publish_fresh_tonie_for_overlay(settings->internal.overlayNumber, uid);
        queued = TRUE;
    }

    if (tonieInfo != NULL)
    {
        freeTonieInfo(tonieInfo);
    }
    return queued;
}

void freshness_mark_content_mapping_changed(settings_t *target_settings,
                                            const char *ruid,
                                            bool_t source_changed)
{
    uint64_t uid = 0;
    if (!freshness_ruid_to_uid(ruid, &uid))
    {
        TRACE_WARNING("Could not process V3 freshness update for invalid rUID %s\r\n", ruid ? ruid : "(null)");
        return;
    }

    if (target_settings != NULL && target_settings->internal.overlayNumber > 0)
    {
        freshness_mark_content_mapping_changed_for_overlay(target_settings, uid,
                                                            source_changed, FALSE);
        return;
    }

    for (uint8_t overlay_id = 1; overlay_id < MAX_OVERLAYS; overlay_id++)
    {
        freshness_mark_content_mapping_changed_for_overlay(get_settings_id(overlay_id), uid,
                                                            source_changed, TRUE);
    }
}

error_t handleCloudFreshnessCheck(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    uint8_t data[BODY_BUFFER_SIZE];
    size_t size;

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudFreshnessCheckTime", current_time, client_ctx);

    settings_t *settings = client_ctx->settings;

    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %" PRIuSIZE " bigger than buffer size %i bytes\r\n", connection->request.byteCount, BODY_BUFFER_SIZE);
    }
    else
    {
        error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
        if (error != NO_ERROR)
        {
            TRACE_ERROR("httpReceive failed!\r\n");
            return error;
        }
        TRACE_INFO("Content (%" PRIuSIZE " of %" PRIuSIZE ")\n", size, connection->request.byteCount);
        TonieFreshnessCheckRequest *freshReq = tonie_freshness_check_request__unpack(NULL, size, (const uint8_t *)data);
        if (freshReq == NULL)
        {
            TRACE_ERROR("Unpacking freshness request failed!\r\n");
        }
        else
        {
            TonieFreshnessCheckResponse freshResp = TONIE_FRESHNESS_CHECK_RESPONSE__INIT;
            TonieFreshnessCheckRequest freshReqCloud = TONIE_FRESHNESS_CHECK_REQUEST__INIT;
            size_t freshnessCacheLen = 0;

            process_freshness_check(client_ctx, freshReq, &freshResp, &freshReqCloud, &freshnessCacheLen, FALSE);

            if (settings->cloud.enabled && settings->cloud.enableV1FreshnessCheck)
            {
                size_t dataLen = tonie_freshness_check_request__get_packed_size(&freshReqCloud);
                tonie_freshness_check_request__pack(&freshReqCloud, (uint8_t *)data);

                cbr_ctx_t ctx;
                req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V1_FRESHNESS_CHECK, &ctx, client_ctx);
                ctx.customData = (void *)&freshResp;
                ctx.customDataLen = freshReq->n_tonie_infos + freshnessCacheLen; // Allocated slots
                if (!cloud_request_post(NULL, 0, "/v1/freshness-check", queryString, data, dataLen, NULL, &cbr))
                {
                    tonie_freshness_check_request__free_unpacked(freshReq, NULL);
                    osFreeMem(freshReqCloud.tonie_infos);
                    osFreeMem(freshResp.tonie_marked);
                    return NO_ERROR;
                }
            }

            TRACE_INFO("Setting freshnessCache with %" PRIuSIZE " entries\r\n", freshResp.n_tonie_marked);
            settings_set_u64_array_id("internal.freshnessCache", freshResp.tonie_marked, freshResp.n_tonie_marked, client_ctx->settings->internal.overlayNumber);
            freshness_cache_sync_source_changed_uids(client_ctx->settings);
            settings_set_bool_id("internal.freshnessCacheChanged", freshResp.n_tonie_marked > 0, client_ctx->settings->internal.overlayNumber);
            mqtt_server_publish_fresh_tonies_for_overlay(client_ctx->settings->internal.overlayNumber);

            tonie_freshness_check_request__free_unpacked(freshReq, NULL);
            setTonieboxSettings(&freshResp, client_ctx->settings);

            size_t dataLen = tonie_freshness_check_response__get_packed_size(&freshResp);
            tonie_freshness_check_response__pack(&freshResp, (uint8_t *)data);
            osFreeMem(freshReqCloud.tonie_infos);
            osFreeMem(freshResp.tonie_marked);
            TRACE_INFO("Freshness check response: size=%" PRIuSIZE ", content=%s\n", dataLen, data);

            httpPrepareHeader(connection, "application/octet-stream; charset=utf-8", dataLen);
            return httpWriteResponse(connection, data, dataLen, false);
            // tonie_freshness_check_response__free_unpacked(&freshResp, NULL);
        }
        return NO_ERROR;
    }
    return NO_ERROR;
}

error_t handleCloudFreshnessCheckV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    uint8_t data[BODY_BUFFER_SIZE];
    size_t size;

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudFreshnessCheckTime", current_time, client_ctx);

    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %" PRIuSIZE " bigger than buffer size %i bytes\r\n", connection->request.byteCount, BODY_BUFFER_SIZE);
        return ERROR_FAILURE;
    }

    error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("httpReceive failed!\r\n");
        return error;
    }

    // null terminate body if it fits
    if (size < BODY_BUFFER_SIZE) {
        data[size] = '\0';
    }

    TRACE_INFO("V3 Freshness check request (%" PRIuSIZE " of %" PRIuSIZE "): %s\n", size, connection->request.byteCount, data);

    // Parse JSON and prepare structures for process_freshness_check
    cJSON *inputJson = cJSON_ParseWithLengthOpts((const char*)data, size, 0, 0);
    if (!inputJson) {
        TRACE_ERROR("Parsing V3 Freshness JSON failed\n");
        return ERROR_FAILURE;
    }

    cJSON *contentObj = cJSON_GetObjectItem(inputJson, "content");
    int count = 0;
    cJSON *item = NULL;
    bool_t validContent = cJSON_IsObject(contentObj);
    if (!validContent) {
        TRACE_WARNING("V3 Freshness JSON missing 'content' object\n");
    } else {
        count = cJSON_GetArraySize(contentObj);
        item = contentObj->child;
    }
    
    TonieFreshnessCheckRequest freshReq = TONIE_FRESHNESS_CHECK_REQUEST__INIT;
    freshReq.n_tonie_infos = count;
    TonieFCInfo *fcInfos = malloc(sizeof(TonieFCInfo) * count);
    freshReq.tonie_infos = malloc(sizeof(TonieFCInfo *) * count);
    if (count > 0 && (fcInfos == NULL || freshReq.tonie_infos == NULL))
    {
        free(fcInfos);
        free(freshReq.tonie_infos);
        cJSON_Delete(inputJson);
        return ERROR_OUT_OF_MEMORY;
    }
    
    int i = 0;
    while (item && i < count) {
        tonie_fcinfo__init(&fcInfos[i]);
        
        char ruidStr[17];
        osStrncpy(ruidStr, item->string, 16);
        ruidStr[16] = '\0';
        
        char uidStr[17];
        for (int j = 0; j < 8; j++) {
            uidStr[j*2] = ruidStr[14 - j*2];
            uidStr[j*2 + 1] = ruidStr[15 - j*2];
        }
        uidStr[16] = '\0';
        
        fcInfos[i].uid = strtoull(uidStr, NULL, 16);
        
        if (cJSON_IsNumber(item)) {
            fcInfos[i].audio_id = (uint32_t)item->valuedouble;
        }
        freshReq.tonie_infos[i] = &fcInfos[i];
        item = item->next;
        i++;
    }
    freshReq.n_tonie_infos = i;
    freshness_store_v3_inventory(client_ctx->settings, validContent ? &freshReq : NULL);
    
    TonieFreshnessCheckResponse freshResp = TONIE_FRESHNESS_CHECK_RESPONSE__INIT;
    TonieFreshnessCheckRequest freshReqCloud = TONIE_FRESHNESS_CHECK_REQUEST__INIT;
    size_t freshnessCacheLen = 0;

    process_freshness_check(client_ctx, &freshReq, &freshResp, &freshReqCloud, &freshnessCacheLen, TRUE);
    
    if (client_ctx->settings->cloud.tb2_v3_enabled && client_ctx->settings->cloud.enableV3FreshnessCheck)
    {
        cJSON *cloudReqJson = cJSON_CreateObject();
        cJSON *cloudContentObj = cJSON_CreateObject();
        cJSON_AddItemToObject(cloudReqJson, "content", cloudContentObj);
        
        for (size_t k = 0; k < freshReqCloud.n_tonie_infos; k++) {
            char ruidStr[17];
            char uidStr[17];
            osSprintf(uidStr, "%016" PRIX64, freshReqCloud.tonie_infos[k]->uid);
            for (int j = 0; j < 8; j++) {
                ruidStr[j*2] = uidStr[14 - j*2];
                ruidStr[j*2 + 1] = uidStr[15 - j*2];
            }
            ruidStr[16] = '\0';
            
            cJSON_AddNumberToObject(cloudContentObj, ruidStr, freshReqCloud.tonie_infos[k]->audio_id);
        }
        
        char *cloud_req_str = cJSON_PrintUnformatted(cloudReqJson);
        size_t cloud_req_len = osStrlen(cloud_req_str);
        
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V3_FRESHNESS_CHECK, &ctx, client_ctx);
        ctx.customData = (void *)&freshResp;
        ctx.customDataLen = freshReq.n_tonie_infos + freshnessCacheLen;
        
        if (!cloud_request_tb2_post(client_ctx->settings->cloud.remote_hostname_tb2, 0, uri, queryString, (const uint8_t *)cloud_req_str, cloud_req_len, NULL, &cbr))
        {
            free(cloud_req_str);
            cJSON_Delete(cloudReqJson);
            free(fcInfos);
            free(freshReq.tonie_infos);
            if (freshReqCloud.tonie_infos) free(freshReqCloud.tonie_infos);
            if (freshResp.tonie_marked) free(freshResp.tonie_marked);
            cJSON_Delete(inputJson);
            return NO_ERROR;
        }
        
        free(cloud_req_str);
        cJSON_Delete(cloudReqJson);
        free(fcInfos);
        free(freshReq.tonie_infos);
        if (freshReqCloud.tonie_infos) free(freshReqCloud.tonie_infos);
        if (freshResp.tonie_marked) free(freshResp.tonie_marked);
        cJSON_Delete(inputJson);
        return NO_ERROR;
    }

    TRACE_INFO("Setting freshnessCache with %" PRIuSIZE " entries\r\n", freshResp.n_tonie_marked);
    settings_set_u64_array_id("internal.freshnessCache", freshResp.tonie_marked, freshResp.n_tonie_marked, client_ctx->settings->internal.overlayNumber);
    freshness_cache_sync_source_changed_uids(client_ctx->settings);
    settings_set_bool_id("internal.freshnessCacheChanged", freshResp.n_tonie_marked > 0, client_ctx->settings->internal.overlayNumber);
    mqtt_server_publish_fresh_tonies_for_overlay(client_ctx->settings->internal.overlayNumber);

    // No settings for TB2 in freshnessCheck
    // setTonieboxSettings(&freshResp, client_ctx->settings); 

    // Now create json response
    cJSON *respJson = cJSON_CreateObject();
    cJSON *itemsArray = cJSON_CreateArray();
    cJSON_AddItemToObject(respJson, "items", itemsArray);
    
    for (size_t j = 0; j < freshResp.n_tonie_marked; j++) {
        char ruidStr[17];
        char uidStr[17];
        osSprintf(uidStr, "%016" PRIX64, freshResp.tonie_marked[j]);
        for (int m = 0; m < 8; m++) {
            ruidStr[m*2] = uidStr[14 - m*2];
            ruidStr[m*2 + 1] = uidStr[15 - m*2];
        }
        ruidStr[16] = '\0';

        cJSON_AddItemToArray(itemsArray, cJSON_CreateString(ruidStr));
    }
    
    char *response_json = cJSON_PrintUnformatted(respJson);
    size_t dataLen = osStrlen(response_json);
    
    TRACE_INFO("V3 Freshness check response: size=%" PRIuSIZE ", content=%s\n", dataLen, response_json);
    
    httpPrepareHeader(connection, "application/json; charset=utf-8", dataLen);
    error = httpWriteResponse(connection, (uint8_t *)response_json, dataLen, false);
    
    free(response_json);
    cJSON_Delete(respJson);
    
    free(fcInfos);
    free(freshReq.tonie_infos);
    if (freshReqCloud.tonie_infos) free(freshReqCloud.tonie_infos);
    if (freshResp.tonie_marked) free(freshResp.tonie_marked);
    
    cJSON_Delete(inputJson);
    return error;
}

error_t handleCloudCheckOtaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    uint8_t data[BODY_BUFFER_SIZE];
    size_t size;

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudCheckOtaTime", current_time, client_ctx);

    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %" PRIuSIZE " bigger than buffer size %i bytes\r\n", connection->request.byteCount, BODY_BUFFER_SIZE);
        return ERROR_FAILURE;
    }

    error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("httpReceive failed!\r\n");
        return error;
    }

    // null terminate body if it fits
    if (size < BODY_BUFFER_SIZE) {
        data[size] = '\0';
    }

    TRACE_INFO("V3 Check OTA request (%" PRIuSIZE " of %" PRIuSIZE "): %s\n", size, connection->request.byteCount, data);

    if (client_ctx->settings->cloud.tb2_v3_enabled && client_ctx->settings->cloud.enableV3Ota)
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V3_CHECK_OTA, &ctx, client_ctx);
        if (!cloud_request_tb2_post(client_ctx->settings->cloud.remote_hostname_tb2, 0, uri, queryString, data, size, NULL, &cbr))
        {
            return NO_ERROR;
        }
    }

    // Skip passthrough for now, reply with empty object
    const char *response_json = "{}";
    size_t dataLen = osStrlen(response_json);

    TRACE_INFO("V3 Check OTA response: size=%" PRIuSIZE ", content=%s\n", dataLen, response_json);

    httpPrepareHeader(connection, "application/json; charset=utf-8", dataLen);
    return httpWriteResponse(connection, (uint8_t *)response_json, dataLen, false);
}

error_t handleCloudSetupStatusV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    uint8_t data[BODY_BUFFER_SIZE];
    size_t size;

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudSetupStatusTime", current_time, client_ctx);

    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %" PRIuSIZE " bigger than buffer size %i bytes\r\n", connection->request.byteCount, BODY_BUFFER_SIZE);
        return ERROR_FAILURE;
    }

    error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("httpReceive failed!\r\n");
        return error;
    }

    if (size < BODY_BUFFER_SIZE) {
        data[size] = '\0';
    }

    TRACE_INFO("V3 Setup Status request (%" PRIuSIZE " of %" PRIuSIZE "): %s\n", size, connection->request.byteCount, data);

    if (client_ctx->settings->cloud.tb2_v3_enabled && client_ctx->settings->cloud.enableV3SetupStatus)
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V3_SETUP_STATUS, &ctx, client_ctx);
        if (!cloud_request_tb2_post(client_ctx->settings->cloud.remote_hostname_tb2, 0, uri, queryString, data, size, NULL, &cbr))
        {
            return NO_ERROR;
        }
    }

    const char *response_json = "";
    size_t dataLen = osStrlen(response_json);

    TRACE_INFO("V3 Setup Status response: size=%" PRIuSIZE ", content=%s\n", dataLen, response_json);

    httpPrepareHeader(connection, "application/json; charset=utf-8", dataLen);
    return httpWriteResponse(connection, (uint8_t *)response_json, dataLen, false);
}

#define V3_CHAPTER_URI_PREFIX "/v3/chapter/"
#define V3_CHAPTER_URI_PREFIX_LENGTH 12U
#define V3_LOCAL_CHAPTER_PREFIX "teddycloud_"
#define V3_LOCAL_CHAPTER_PREFIX_LENGTH 11U
#define V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH 20U
#define V3_LOCAL_CHAPTER_LEGACY_NAME_LENGTH 30U
#define V3_LOCAL_CHAPTER_HASHED_NAME_LENGTH 56U

typedef enum
{
    V3_LOCAL_CHAPTER_INVALID = 0,
    V3_LOCAL_CHAPTER_LEGACY,
    V3_LOCAL_CHAPTER_HASHED,
} v3_local_chapter_format_t;

typedef struct
{
    v3_local_chapter_format_t format;
    size_t chapter_index;
    size_t ruid_uri_offset;
    char hash_prefix[V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH + 1];
    char ruid[V3_LOCAL_CONTENT_RUID_SIZE];
} v3_local_chapter_request_t;

static bool_t v3_local_is_hex_string(const char *value, size_t length, bool_t lowercase_only)
{
    if (value == NULL)
    {
        return FALSE;
    }

    for (size_t i = 0; i < length; i++)
    {
        unsigned char current = (unsigned char)value[i];
        if (!isxdigit(current) || (lowercase_only && current >= 'A' && current <= 'F'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t v3_local_copy_canonical_ruid(const char *value, char ruid[V3_LOCAL_CONTENT_RUID_SIZE])
{
    if (!v3_local_is_hex_string(value, V3_LOCAL_CONTENT_RUID_HEX_LENGTH, FALSE))
    {
        return FALSE;
    }

    for (size_t i = 0; i < V3_LOCAL_CONTENT_RUID_HEX_LENGTH; i++)
    {
        ruid[i] = (char)toupper((unsigned char)value[i]);
    }
    ruid[V3_LOCAL_CONTENT_RUID_HEX_LENGTH] = '\0';
    return TRUE;
}

static bool_t v3_local_parse_chapter_request(const char *uri, v3_local_chapter_request_t *request)
{
    if (uri == NULL || request == NULL ||
        osStrncmp(uri, V3_CHAPTER_URI_PREFIX, V3_CHAPTER_URI_PREFIX_LENGTH) != 0)
    {
        return FALSE;
    }

    osMemset(request, 0, sizeof(*request));
    const char *name = uri + V3_CHAPTER_URI_PREFIX_LENGTH;
    if (osStrncmp(name, V3_LOCAL_CHAPTER_PREFIX, V3_LOCAL_CHAPTER_PREFIX_LENGTH) != 0)
    {
        return FALSE;
    }

    size_t name_length = osStrlen(name);
    if (name_length == V3_LOCAL_CHAPTER_HASHED_NAME_LENGTH)
    {
        const size_t hash_offset = V3_LOCAL_CHAPTER_PREFIX_LENGTH;
        const size_t index_offset = hash_offset + V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH + 1;
        const size_t ruid_offset = index_offset + 3;
        const size_t suffix_offset = ruid_offset + V3_LOCAL_CONTENT_RUID_HEX_LENGTH;

        if (name[index_offset - 1] != '_' || name[index_offset + 2] != '_' ||
            osStrcmp(name + suffix_offset, ".opus") != 0 ||
            !v3_local_is_hex_string(name + hash_offset, V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH, TRUE) ||
            !isdigit((unsigned char)name[index_offset]) ||
            !isdigit((unsigned char)name[index_offset + 1]) ||
            !v3_local_copy_canonical_ruid(name + ruid_offset, request->ruid))
        {
            return FALSE;
        }

        request->format = V3_LOCAL_CHAPTER_HASHED;
        request->chapter_index = (size_t)(name[index_offset] - '0') * 10U +
                                 (size_t)(name[index_offset + 1] - '0');
        request->ruid_uri_offset = V3_CHAPTER_URI_PREFIX_LENGTH + ruid_offset;
        osMemcpy(request->hash_prefix, name + hash_offset, V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH);
        request->hash_prefix[V3_LOCAL_CHAPTER_HASH_PREFIX_LENGTH] = '\0';
        return TRUE;
    }

    if (name_length == V3_LOCAL_CHAPTER_LEGACY_NAME_LENGTH)
    {
        const size_t index_offset = V3_LOCAL_CHAPTER_PREFIX_LENGTH;
        const size_t ruid_offset = index_offset + 3;
        if (!isdigit((unsigned char)name[index_offset]) ||
            !isdigit((unsigned char)name[index_offset + 1]) ||
            name[index_offset + 2] != '_' ||
            !v3_local_copy_canonical_ruid(name + ruid_offset, request->ruid))
        {
            return FALSE;
        }

        request->format = V3_LOCAL_CHAPTER_LEGACY;
        request->chapter_index = (size_t)(name[index_offset] - '0') * 10U +
                                 (size_t)(name[index_offset + 1] - '0');
        request->ruid_uri_offset = V3_CHAPTER_URI_PREFIX_LENGTH + ruid_offset;
        return TRUE;
    }

    return FALSE;
}

static bool_t v3_local_hashed_uid_add(settings_t *settings, const char *ruid)
{
    uint64_t uid = 0;
    return freshness_ruid_to_uid(ruid, &uid) &&
           freshness_settings_array_add_uid(settings, "internal.v3HashedChapterUids", uid, NULL);
}

static error_t v3_local_write_empty_status(HttpConnection *connection, uint16_t status_code)
{
    httpPrepareHeader(connection, NULL, 0);
    connection->response.statusCode = status_code;
    return httpWriteResponse(connection, NULL, 0, false);
}

static error_t v3_local_reject_legacy_range(HttpConnection *connection, uint32_t file_size)
{
    httpPrepareHeader(connection, NULL, 0);
    if (connection->response.contentRange == NULL)
    {
        connection->response.contentRange = osAllocMem(64);
        if (connection->response.contentRange == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
    }
    osSnprintf((char *)connection->response.contentRange,
               64,
               "bytes */%" PRIu32,
               file_size);
    connection->response.statusCode = 416;
    connection->response.contentLength = 0;
    return httpWriteResponse(connection, NULL, 0, false);
}

static bool_t v3_hex_digest_parse(
    const char hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE],
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    if (hex == NULL || osStrlen(hex) != SHA256_DIGEST_SIZE * 2U)
    {
        return FALSE;
    }
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        unsigned char high = (unsigned char)hex[i * 2U];
        unsigned char low = (unsigned char)hex[i * 2U + 1U];
        if (!isxdigit(high) || !isxdigit(low))
        {
            return FALSE;
        }
        high = (unsigned char)(isdigit(high) ? high - '0'
                                              : tolower(high) - 'a' + 10);
        low = (unsigned char)(isdigit(low) ? low - '0'
                                            : tolower(low) - 'a' + 10);
        digest[i] = (uint8_t)((high << 4) | low);
    }
    return TRUE;
}

static error_t v3_native_collection_generation_load(
    settings_t *settings,
    const char *source,
    const char *ruid,
    uint32_t effective_version,
    v3_local_content_generation_t *generation)
{
    if (settings == NULL || source == NULL || generation == NULL ||
        effective_version == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    char canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (!tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    v3_native_library_collection_t collection;
    error_t error = v3_native_library_collection_load(
        settings->internal.librarydirfull, source, FALSE, &collection);
    if (error != NO_ERROR)
    {
        return error;
    }

    osMemset(generation, 0, sizeof(*generation));
    generation->chapters = osAllocMem(
        sizeof(*generation->chapters) * collection.chapter_count);
    if (generation->chapters == NULL)
    {
        v3_native_library_collection_free(&collection);
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(generation->chapters, 0,
             sizeof(*generation->chapters) * collection.chapter_count);
    generation->chapter_count = collection.chapter_count;
    generation->overlay_id = settings->internal.overlayNumber;
    generation->effective_version = effective_version;
    osStrcpy(generation->ruid, canonical_ruid);

    for (size_t i = 0; error == NO_ERROR && i < collection.chapter_count; i++)
    {
        const v3_native_library_collection_chapter_t *stored =
            &collection.chapters[i];
        v3_local_content_chapter_t *chapter = &generation->chapters[i];
        int name_length = osSnprintf(
            chapter->name, sizeof(chapter->name),
            "teddycloud_%.20s_%02" PRIuSIZE "_%s.opus",
            stored->sha256, stored->index, canonical_ruid);
        if (name_length <= 0 || (size_t)name_length >= sizeof(chapter->name) ||
            !v3_hex_digest_parse(stored->sha256, chapter->sha256))
        {
            error = ERROR_INVALID_FILE;
            break;
        }
        chapter->index = stored->index;
        chapter->file_size = stored->file_size;
        osStrcpy(chapter->sha256_hex, stored->sha256);
        chapter->path = strdup(stored->path);
        if (chapter->path == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }

    v3_native_library_collection_free(&collection);
    if (error != NO_ERROR)
    {
        v3_local_content_generation_free(generation);
    }
    return error;
}

static bool_t v3_source_is_tap(const tonie_info_t *tonieInfo)
{
    return tonieInfo != NULL && tonieInfo->json._tap._valid &&
           (tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM ||
            tonieInfo->json._source_type == CT_SOURCE_TAP_CACHED);
}

static char *v3_tap_display_name(const char *preferred, const char *path,
                                 const char *fallback)
{
    const char *begin = preferred != NULL ? preferred : "";
    while (*begin != '\0' && isspace((unsigned char)*begin))
    {
        begin++;
    }
    const char *end = begin + osStrlen(begin);
    while (end > begin && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    if (end == begin)
    {
        begin = path != NULL ? path : "";
        const char *slash = strrchr(begin, '/');
        const char *backslash = strrchr(begin, '\\');
        if (slash != NULL || backslash != NULL)
        {
            const char *last = slash == NULL ? backslash
                                             : (backslash == NULL || slash > backslash ? slash : backslash);
            begin = last + 1;
        }
        end = begin + osStrlen(begin);
        const char *dot = strrchr(begin, '.');
        if (dot != NULL && dot > begin)
        {
            end = dot;
        }
    }
    if (end == begin)
    {
        begin = fallback;
        end = begin + osStrlen(begin);
    }

    size_t length = (size_t)(end - begin);
    if (length > V3_LOCAL_CONTENT_TITLE_MAX)
    {
        length = V3_LOCAL_CONTENT_TITLE_MAX;
    }
    char *value = osAllocMem(length + 1);
    if (value != NULL)
    {
        osMemcpy(value, begin, length);
        value[length] = '\0';
    }
    return value;
}

static bool_t v3_tap_attach_metadata(v3_local_content_generation_t *generation,
                                     const tonie_info_t *tonieInfo,
                                     const tonie_info_t *snapshotInfo,
                                     const size_t *runtime_indices,
                                     size_t runtime_files_count,
                                     uint32_t selection_generation)
{
    if (generation == NULL || tonieInfo == NULL || snapshotInfo == NULL ||
        runtime_indices == NULL || !v3_source_is_tap(tonieInfo) ||
        generation->chapter_count != runtime_files_count)
    {
        return FALSE;
    }

    const tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
    generation->source_kind = V3_LOCAL_CONTENT_SOURCE_TAP;
    generation->source_path = strdup(tonieInfo->json.source != NULL
                                         ? tonieInfo->json.source
                                         : "");
    generation->title = v3_tap_display_name(tap->name,
                                            tonieInfo->json.source,
                                            "Playlist");
    generation->tap_audio_id = (uint32_t)tap->audio_id;
    generation->shuffle_mode = tap->shuffle;
    generation->selection_generation = selection_generation;
    if (generation->source_path == NULL || generation->title == NULL)
    {
        return FALSE;
    }

    const track_positions_t *positions = &snapshotInfo->additional.track_positions;
    bool_t durations_available = positions->pos != NULL &&
                                 positions->count == generation->chapter_count &&
                                 positions->total_seconds > 0;
    uint32_t chapter_start = 0;
    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        size_t source_index = runtime_indices[i];
        if (source_index >= tap->filesCount)
        {
            return FALSE;
        }
        char fallback[32];
        osSnprintf(fallback, sizeof(fallback), "Chapter %" PRIuSIZE, i + 1);
        generation->chapters[i].source_index = source_index;
        generation->chapters[i].title = v3_tap_display_name(
            tap->files[source_index].name,
            tap->files[source_index].filepath,
            fallback);
        if (generation->chapters[i].title == NULL)
        {
            return FALSE;
        }
        if (durations_available)
        {
            uint32_t chapter_end = i + 1 < generation->chapter_count
                                       ? positions->pos[i + 1]
                                       : positions->total_seconds;
            if (chapter_end < chapter_start)
            {
                return FALSE;
            }
            generation->chapters[i].duration_seconds = chapter_end - chapter_start;
            chapter_start = chapter_end;
        }
    }
    return TRUE;
}

static error_t v3_tap_prepare_generation(settings_t *settings,
                                         tonie_info_t *tonieInfo,
                                         const char *ruid,
                                         size_t *runtime_indices,
                                         size_t runtime_files_count,
                                         uint32_t selection_generation,
                                         v3_local_content_generation_t *generation)
{
    tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
    if (!v3_source_is_tap(tonieInfo) || tap->_filepath_resolved == NULL)
    {
        return ERROR_INVALID_FILE;
    }

    error_t error = tap_target_lock(tap->_filepath_resolved);
    if (error != NO_ERROR)
    {
        return error;
    }

    const char *final_taf_path = tap->_filepath_resolved;
    if (tonieInfo->json._source_type != CT_SOURCE_TAP_CACHED ||
        !tap_final_cache_is_current(tap))
    {
        error = tap_materialize_final_snapshot_selected(tap,
                                                        runtime_indices,
                                                        runtime_files_count,
                                                        &final_taf_path);
    }

    tonie_info_t *snapshotInfo = NULL;
    if (error == NO_ERROR)
    {
        snapshotInfo = getTonieInfoV2(final_taf_path, false,
                                      settings->core.tap_taf_validation,
                                      settings);
        if (snapshotInfo == NULL || !snapshotInfo->exists || !snapshotInfo->valid ||
            snapshotInfo->tafHeader == NULL)
        {
            error = ERROR_INVALID_FILE;
        }
    }
    if (error == NO_ERROR)
    {
        error = v3_local_content_prepare_tap(final_taf_path,
                                             snapshotInfo->tafHeader,
                                             settings->internal.cachedirfull,
                                             ruid,
                                             generation);
    }
    if (error == NO_ERROR &&
        !v3_tap_attach_metadata(generation, tonieInfo, snapshotInfo,
                                runtime_indices, runtime_files_count,
                                selection_generation))
    {
        error = ERROR_INVALID_FILE;
    }
    if (snapshotInfo != NULL)
    {
        freeTonieInfo(snapshotInfo);
    }
    tap_target_unlock(tap->_filepath_resolved);
    return error;
}

static error_t v3_local_prepare_generation_from_source(settings_t *settings,
                                                       tonie_info_t *tonieInfo,
                                                       const char *ruid,
                                                       v3_local_content_generation_t *generation)
{
    if (settings == NULL || tonieInfo == NULL || generation == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM)
    {
        tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
        if (!tap->_valid || tap->_filepath_resolved == NULL)
        {
            return ERROR_INVALID_FILE;
        }

        error_t error = tap_target_lock(tap->_filepath_resolved);
        if (error != NO_ERROR)
        {
            return error;
        }

        const char *final_taf_path = tap->_filepath_resolved;
        if (!tap_final_cache_is_current(tap))
        {
            error = tap_materialize_final_snapshot(tap, &final_taf_path);
        }

        tonie_info_t *snapshotInfo = NULL;
        if (error == NO_ERROR)
        {
            snapshotInfo = getTonieInfoV2(final_taf_path,
                                          false,
                                          settings->core.tap_taf_validation,
                                          settings);
            if (snapshotInfo == NULL || !snapshotInfo->exists || !snapshotInfo->valid ||
                snapshotInfo->tafHeader == NULL)
            {
                error = ERROR_INVALID_FILE;
            }
        }
        if (error == NO_ERROR)
        {
            error = v3_local_content_prepare(final_taf_path,
                                             snapshotInfo->tafHeader,
                                             settings->internal.cachedirfull,
                                             ruid,
                                             generation);
        }
        if (snapshotInfo != NULL)
        {
            freeTonieInfo(snapshotInfo);
        }
        tap_target_unlock(tap->_filepath_resolved);
        return error;
    }

    if (tonieInfo->json._source_type == CT_SOURCE_TAF_INCOMPLETE ||
        !tonieInfo->exists || !tonieInfo->valid || tonieInfo->tafHeader == NULL)
    {
        return ERROR_INVALID_FILE;
    }

    return v3_local_content_prepare(tonieInfo->contentPath,
                                    tonieInfo->tafHeader,
                                    settings->internal.cachedirfull,
                                    ruid,
                                    generation);
}

static error_t v3_local_load_or_prepare_generation(settings_t *settings,
                                                   tonie_info_t *tonieInfo,
                                                   const char *ruid,
                                                   uint32_t effective_version,
                                                   bool_t persist,
                                                   v3_local_content_generation_t *generation)
{
    if (persist)
    {
        error_t load_error = v3_local_content_generation_load(settings->internal.cachedirfull,
                                                              settings->internal.overlayNumber,
                                                              ruid,
                                                              effective_version,
                                                              generation);
        if (load_error == NO_ERROR)
        {
            TRACE_DEBUG("Loaded V3 local generation overlay=%u rUID=%s version=%" PRIu32 " chapters=%" PRIuSIZE "\r\n",
                        (unsigned)settings->internal.overlayNumber,
                        generation->ruid,
                        effective_version,
                        generation->chapter_count);
            return NO_ERROR;
        }
        TRACE_DEBUG("Preparing V3 local generation overlay=%u rUID=%s version=%" PRIu32 ": %s\r\n",
                    (unsigned)settings->internal.overlayNumber,
                    ruid,
                    effective_version,
                    error2text(load_error));
    }

    error_t error = v3_local_prepare_generation_from_source(settings,
                                                            tonieInfo,
                                                            ruid,
                                                            generation);
    if (error == NO_ERROR && persist)
    {
        error = v3_local_content_generation_save(settings->internal.cachedirfull,
                                                 settings->internal.overlayNumber,
                                                 effective_version,
                                                 generation);
    }
    return error;
}

static uint32_t v3_tap_next_version(const v3_local_tap_state_t *state,
                                    uint32_t tap_audio_id,
                                    uint32_t static_version,
                                    bool_t box_version_known,
                                    uint32_t box_version)
{
    uint32_t maximum = tap_audio_id > static_version ? tap_audio_id : static_version;
    if (state != NULL && state->valid)
    {
        if (state->prepared_version > maximum) maximum = state->prepared_version;
        if (state->playing_version > maximum) maximum = state->playing_version;
        if (state->previous_version > maximum) maximum = state->previous_version;
    }
    if (box_version_known && box_version > maximum)
    {
        maximum = box_version;
    }
    if (maximum < UINT32_MAX)
    {
        return maximum + 1U;
    }

    uint32_t candidate = 1;
    while (candidate == tap_audio_id || candidate == static_version ||
           (box_version_known && candidate == box_version) ||
           (state != NULL && state->valid &&
            (candidate == state->prepared_version ||
             candidate == state->playing_version ||
             candidate == state->previous_version)))
    {
        candidate++;
    }
    return candidate;
}

static bool_t v3_tap_generation_matches(const v3_local_content_generation_t *generation,
                                        const tonie_info_t *tonieInfo,
                                        const v3_local_tap_state_t *state)
{
    const char *source = tonieInfo != NULL && tonieInfo->json.source != NULL
                             ? tonieInfo->json.source
                             : "";
    return generation != NULL && state != NULL && state->valid &&
           generation->source_kind == V3_LOCAL_CONTENT_SOURCE_TAP &&
           generation->tap_audio_id == state->tap_audio_id &&
           generation->shuffle_mode == state->shuffle_mode &&
           generation->selection_generation == state->selection_generation &&
           generation->source_path != NULL &&
           osStrcmp(generation->source_path, source) == 0;
}

static error_t v3_tap_load_or_prepare_generation(settings_t *settings,
                                                 tonie_info_t *tonieInfo,
                                                 const char *ruid,
                                                 uint32_t static_version,
                                                 v3_local_content_generation_t *generation,
                                                 uint32_t *effective_version)
{
    if (settings == NULL || !v3_source_is_tap(tonieInfo) ||
        generation == NULL || effective_version == NULL || static_version == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    tonie_audio_playlist_t *tap = &tonieInfo->json._tap;
    v3_local_tap_state_t state;
    bool_t state_valid = v3_local_tap_state_load(
                             settings->internal.cachedirfull,
                             settings->internal.overlayNumber,
                             ruid, &state) == NO_ERROR && state.valid;
    bool_t revision_changed = !state_valid ||
                              state.tap_audio_id != (uint32_t)tap->audio_id ||
                              state.shuffle_mode != tap->shuffle;
    bool_t source_change_pending = freshness_source_changed_contains_ruid(settings, ruid);
    if (state_valid && !revision_changed && !state.regeneration_pending &&
        (!source_change_pending || state.prepared_version == static_version))
    {
        error_t load_error = v3_local_content_generation_load(
            settings->internal.cachedirfull,
            settings->internal.overlayNumber,
            ruid, state.prepared_version, generation);
        if (load_error == NO_ERROR &&
            v3_tap_generation_matches(generation, tonieInfo, &state))
        {
            *effective_version = state.prepared_version;
            return NO_ERROR;
        }
        v3_local_content_generation_free(generation);
        revision_changed = TRUE;
    }
    else if (state_valid && source_change_pending &&
             state.prepared_version != static_version)
    {
        revision_changed = TRUE;
    }

    uint64_t uid = 0;
    uint32_t box_version = 0;
    bool_t uid_valid = tb2_ruid_to_uid(ruid, &uid);
    bool_t box_version_known = uid_valid &&
                               freshness_inventory_find_uid(settings, uid,
                                                            &box_version);
    uint32_t desired_version = static_version;
    if (state_valid && !source_change_pending &&
        (state.regeneration_pending || revision_changed) &&
        (desired_version == state.prepared_version ||
         desired_version == state.playing_version ||
         desired_version == state.previous_version))
    {
        desired_version = v3_tap_next_version(&state,
                                              (uint32_t)tap->audio_id,
                                              static_version,
                                              box_version_known,
                                              box_version);
    }
    if (desired_version == 0)
    {
        return ERROR_FAILURE;
    }

    size_t *runtime_indices = NULL;
    size_t runtime_files_count = 0;
    error_t error = tap_prepare_runtime_indices(tap, &runtime_indices,
                                                &runtime_files_count);
    uint32_t selection_generation = state_valid
                                        ? (state.selection_generation == UINT32_MAX
                                               ? 1U
                                               : state.selection_generation + 1U)
                                        : 1U;
    if (error == NO_ERROR)
    {
        error = v3_tap_prepare_generation(settings, tonieInfo, ruid,
                                          runtime_indices,
                                          runtime_files_count,
                                          selection_generation,
                                          generation);
    }
    tap_free_runtime_indices(runtime_indices);
    if (error == NO_ERROR)
    {
        error = v3_local_content_generation_save(
            settings->internal.cachedirfull,
            settings->internal.overlayNumber,
            desired_version, generation);
    }
    if (error != NO_ERROR)
    {
        v3_local_content_generation_free(generation);
        return error;
    }

    v3_local_tap_state_t updated = state_valid ? state : (v3_local_tap_state_t){0};
    updated.valid = TRUE;
    updated.tap_audio_id = (uint32_t)tap->audio_id;
    updated.shuffle_mode = tap->shuffle;
    updated.selection_generation = selection_generation;
    updated.previous_version = state_valid ? state.prepared_version : 0;
    updated.prepared_version = desired_version;
    updated.regeneration_pending = FALSE;
    error = v3_local_tap_state_save(settings->internal.cachedirfull,
                                    settings->internal.overlayNumber,
                                    ruid, &updated);
    if (error != NO_ERROR)
    {
        v3_local_content_generation_free(generation);
        return error;
    }

    *effective_version = desired_version;
    TRACE_INFO("Prepared TB2 TAP generation overlay=%u rUID=%s version=%" PRIu32
               " selection=%" PRIu32 " shuffle=%u chapters=%" PRIuSIZE "\r\n",
               (unsigned)settings->internal.overlayNumber, ruid,
               desired_version, selection_generation,
               (unsigned)tap->shuffle, generation->chapter_count);
    return NO_ERROR;
}

void v3_tap_playback_observe(settings_t *settings,
                             const char *previous_ruid,
                             const char *current_ruid,
                             bool_t content_version_valid,
                             uint32_t content_version)
{
    if (settings == NULL || settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        return;
    }

    char previous[TB2_RUID_SIZE];
    char current[TB2_RUID_SIZE];
    bool_t previous_valid = tb2_ruid_canonicalize(previous_ruid, previous);
    bool_t current_valid = tb2_ruid_canonicalize(current_ruid, current);
    bool_t cycle_ended = previous_valid &&
                         (!current_valid || osStrcasecmp(previous, current) != 0);
    if (cycle_ended)
    {
        tonie_info_t *previous_info = getTonieInfoFromRuid(previous, false, settings);
        v3_local_tap_state_t state;
        if (v3_source_is_tap(previous_info) &&
            v3_local_tap_state_load(settings->internal.cachedirfull,
                                    settings->internal.overlayNumber,
                                    previous, &state) == NO_ERROR &&
            state.valid && state.shuffle_mode != TAP_SHUFFLE_NONE &&
            !state.regeneration_pending)
        {
            state.regeneration_pending = TRUE;
            if (v3_local_tap_state_save(settings->internal.cachedirfull,
                                        settings->internal.overlayNumber,
                                        previous, &state) == NO_ERROR)
            {
                uint64_t uid = 0;
                bool_t queued = tb2_ruid_to_uid(previous, &uid) &&
                                mqtt_server_publish_fresh_tonie_for_overlay(
                                    settings->internal.overlayNumber, uid);
                TRACE_INFO("TB2 TAP playback cycle ended overlay=%u rUID=%s; next selection pending queued=%u\r\n",
                           (unsigned)settings->internal.overlayNumber,
                           previous, queued ? 1U : 0U);
            }
            else
            {
                TRACE_WARNING("Could not persist pending TB2 TAP selection overlay=%u rUID=%s\r\n",
                              (unsigned)settings->internal.overlayNumber,
                              previous);
            }
        }
        if (previous_info != NULL)
        {
            freeTonieInfo(previous_info);
        }
    }

    if (!current_valid || !content_version_valid || content_version == 0)
    {
        return;
    }

    tonie_info_t *current_info = getTonieInfoFromRuid(current, false, settings);
    v3_local_tap_state_t state;
    if (v3_source_is_tap(current_info) &&
        v3_local_tap_state_load(settings->internal.cachedirfull,
                                settings->internal.overlayNumber,
                                current, &state) == NO_ERROR &&
        state.valid &&
        (content_version == state.prepared_version ||
         content_version == state.previous_version ||
         content_version == state.playing_version) &&
        state.playing_version != content_version)
    {
        state.playing_version = content_version;
        if (v3_local_tap_state_save(settings->internal.cachedirfull,
                                    settings->internal.overlayNumber,
                                    current, &state) == NO_ERROR)
        {
            TRACE_INFO("Observed TB2 TAP playback overlay=%u rUID=%s version=%" PRIu32 "\r\n",
                       (unsigned)settings->internal.overlayNumber,
                       current, content_version);
        }
        else
        {
            TRACE_WARNING("Could not persist TB2 TAP playback state overlay=%u rUID=%s version=%" PRIu32 "\r\n",
                          (unsigned)settings->internal.overlayNumber,
                          current, content_version);
        }
    }
    if (current_info != NULL)
    {
        freeTonieInfo(current_info);
    }
}

static error_t sendLocalContentMetaV3(HttpConnection *connection,
                                      settings_t *settings,
                                      const char *ruid,
                                      uint32_t version,
                                      const char *resumeBehavior,
                                      const char *tonieSalesId,
                                      const v3_local_content_generation_t *generation)
{
    cJSON *respJson = cJSON_CreateObject();
    cJSON *contentArray = NULL;
    if (respJson == NULL ||
        cJSON_AddNumberToObject(respJson, "version", version) == NULL ||
        cJSON_AddStringToObject(respJson, "contentType", "content_tonie") == NULL ||
        cJSON_AddStringToObject(respJson, "tonieType", "content") == NULL ||
        cJSON_AddStringToObject(respJson, "resumeBehavior", resumeBehavior) == NULL ||
        cJSON_AddStringToObject(respJson, "tonieSalesId", tonieSalesId ? tonieSalesId : "") == NULL ||
        (contentArray = cJSON_AddArrayToObject(respJson, "content")) == NULL)
    {
        cJSON_Delete(respJson);
        return ERROR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        const v3_local_content_chapter_t *chapter = &generation->chapters[i];
        cJSON *contentItem = cJSON_CreateObject();
        cJSON *analytics = NULL;
        if (contentItem == NULL ||
            cJSON_AddStringToObject(contentItem, "type", "audio") == NULL ||
            cJSON_AddStringToObject(contentItem, "name", chapter->name) == NULL ||
            cJSON_AddStringToObject(contentItem, "auth", "") == NULL ||
            (analytics = cJSON_AddObjectToObject(contentItem, "analytics")) == NULL ||
            cJSON_AddNumberToObject(contentItem, "fileSize", (double)chapter->file_size) == NULL ||
            !cJSON_AddItemToArray(contentArray, contentItem))
        {
            cJSON_Delete(contentItem);
            cJSON_Delete(respJson);
            return ERROR_OUT_OF_MEMORY;
        }
        (void)analytics;
        TRACE_INFO("V3 local generation overlay=%u rUID=%s version=%" PRIu32 " chapter=%" PRIuSIZE " name=%s bytes=%" PRIu32 "\r\n",
                   (unsigned)settings->internal.overlayNumber,
                   generation->ruid,
                   version,
                   chapter->index,
                   chapter->name,
                   chapter->file_size);
    }

    char *response_json = cJSON_PrintUnformatted(respJson);
    if (response_json == NULL)
    {
        cJSON_Delete(respJson);
        return ERROR_OUT_OF_MEMORY;
    }

    size_t dataLen = osStrlen(response_json);
    TRACE_INFO("V3 Content Meta response: size=%" PRIuSIZE ", content=%s\n", dataLen, response_json);

    httpPrepareHeader(connection, "application/json; charset=utf-8", dataLen);
    error_t error = httpWriteResponse(connection, (uint8_t *)response_json, dataLen, false);

    cJSON_free(response_json);
    cJSON_Delete(respJson);
    if (error == NO_ERROR && !v3_local_hashed_uid_add(settings, ruid))
    {
        TRACE_WARNING("Could not persist V3 hashed chapter transition for rUID %s\r\n", ruid);
    }
    return error;
}

typedef enum
{
    V3_MANUAL_DOWNLOAD_GENERATION,
    V3_MANUAL_DOWNLOAD_POLICY,
    V3_MANUAL_DOWNLOAD_AUTH,
    V3_MANUAL_DOWNLOAD_MANIFEST,
    V3_MANUAL_DOWNLOAD_CHAPTER,
    V3_MANUAL_DOWNLOAD_ACTIVATION,
    V3_MANUAL_DOWNLOAD_COMPLETE,
} v3_manual_download_stage_t;

static const char *v3_manual_download_stage_name(v3_manual_download_stage_t stage)
{
    switch (stage)
    {
    case V3_MANUAL_DOWNLOAD_GENERATION:
        return "generation";
    case V3_MANUAL_DOWNLOAD_POLICY:
        return "policy";
    case V3_MANUAL_DOWNLOAD_AUTH:
        return "auth";
    case V3_MANUAL_DOWNLOAD_MANIFEST:
        return "manifest";
    case V3_MANUAL_DOWNLOAD_CHAPTER:
        return "chapter";
    case V3_MANUAL_DOWNLOAD_ACTIVATION:
        return "activation";
    case V3_MANUAL_DOWNLOAD_COMPLETE:
        return "complete";
    default:
        return "unknown";
    }
}

static uint_t v3_manual_download_http_status(v3_manual_download_stage_t stage)
{
    switch (stage)
    {
    case V3_MANUAL_DOWNLOAD_AUTH:
        return 401;
    case V3_MANUAL_DOWNLOAD_POLICY:
        return 403;
    case V3_MANUAL_DOWNLOAD_GENERATION:
        return 400;
    case V3_MANUAL_DOWNLOAD_MANIFEST:
    case V3_MANUAL_DOWNLOAD_CHAPTER:
        return 502;
    case V3_MANUAL_DOWNLOAD_ACTIVATION:
        return 500;
    case V3_MANUAL_DOWNLOAD_COMPLETE:
    default:
        return 200;
    }
}

static const char *v3_manual_download_message(v3_manual_download_stage_t stage,
                                              error_t error,
                                              uint32_t upstream_status)
{
    if (stage == V3_MANUAL_DOWNLOAD_GENERATION)
    {
        return "Selected overlay is not a TB2";
    }
    if (stage == V3_MANUAL_DOWNLOAD_POLICY)
    {
        return "TB2 cloud access is blocked for this RUID";
    }
    if (stage == V3_MANUAL_DOWNLOAD_AUTH)
    {
        return upstream_status == 401 || upstream_status == 403
                   ? "TONIES rejected the stored TB2 content authentication"
                   : "Stored TB2 content authentication is missing or invalid";
    }
    if (stage == V3_MANUAL_DOWNLOAD_MANIFEST)
    {
        if (error == ERROR_ACCESS_DENIED && upstream_status == 0)
        {
            return "TB2 V3 cloud path is disabled";
        }
        if (error == ERROR_INVALID_NAME)
        {
            return "TONIES V3 content-meta contains an invalid chapter name";
        }
        return "TONIES V3 content-meta download or staging failed";
    }
    if (stage == V3_MANUAL_DOWNLOAD_CHAPTER)
    {
        return upstream_status == 404
                   ? "A referenced TONIES V3 chapter is missing"
                   : "TONIES V3 chapter download or staging failed";
    }
    if (stage == V3_MANUAL_DOWNLOAD_ACTIVATION)
    {
        return "The complete TB2 V3 cache version could not be activated";
    }
    return error2text(error);
}

static error_t v3_manual_download_write_result(HttpConnection *connection,
                                               v3_manual_download_stage_t stage,
                                               const char *message,
                                               const char *ruid,
                                               const char *chapter,
                                               uint32_t upstream_status,
                                               uint32_t version,
                                               size_t completed,
                                               size_t total)
{
    cJSON *response = cJSON_CreateObject();
    if (response == NULL ||
        cJSON_AddBoolToObject(response, "success",
                              stage == V3_MANUAL_DOWNLOAD_COMPLETE) == NULL ||
        cJSON_AddStringToObject(response, "stage",
                                v3_manual_download_stage_name(stage)) == NULL ||
        cJSON_AddStringToObject(response, "message",
                                message != NULL ? message : "") == NULL ||
        cJSON_AddStringToObject(response, "ruid", ruid != NULL ? ruid : "") == NULL ||
        cJSON_AddNumberToObject(response, "upstreamStatus", upstream_status) == NULL ||
        cJSON_AddNumberToObject(response, "version", version) == NULL ||
        cJSON_AddNumberToObject(response, "objectsCompleted", completed) == NULL ||
        cJSON_AddNumberToObject(response, "objectsTotal", total) == NULL ||
        cJSON_AddNumberToObject(response, "chaptersCompleted", completed) == NULL ||
        cJSON_AddNumberToObject(response, "chaptersTotal", total) == NULL ||
        (chapter != NULL &&
         cJSON_AddStringToObject(response, "object", chapter) == NULL) ||
        (chapter != NULL &&
         cJSON_AddStringToObject(response, "chapter", chapter) == NULL))
    {
        cJSON_Delete(response);
        return ERROR_OUT_OF_MEMORY;
    }

    char *body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    connection->response.statusCode = v3_manual_download_http_status(stage);
    connection->response.noCache = TRUE;
    size_t length = osStrlen(body);
    httpPrepareHeader(connection, "application/json; charset=utf-8", length);
    error_t error = httpWriteResponse(connection, (uint8_t *)body, length, false);
    cJSON_free(body);
    return error;
}

static void v3_manual_download_restore_active_route(settings_t *settings,
                                                    const char *ruid)
{
    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t version = 0;
    if (v3_native_cache_read_active_manifest(settings->internal.cachedirfull,
                                             settings->internal.overlayNumber,
                                             ruid, &manifest, &manifest_length,
                                             &version) == NO_ERROR)
    {
        TRACE_DEBUG("Restored active TB2 V3 cache route overlay=%u rUID=%s version=%" PRIu32 " after manual download failure\r\n",
                    (unsigned)settings->internal.overlayNumber, ruid, version);
    }
    osFreeMem(manifest);
}

static error_t v3_manual_download_fail(HttpConnection *connection,
                                       settings_t *settings,
                                       v3_manual_download_stage_t stage,
                                       error_t error,
                                       const char *ruid,
                                       const char *chapter,
                                       uint32_t upstream_status,
                                       uint32_t version,
                                       size_t completed,
                                       size_t total,
                                       bool_t restore_active)
{
    if (restore_active)
    {
        v3_manual_download_restore_active_route(settings, ruid);
    }
    TRACE_ERROR("TB2 manual content download failed stage=%s overlay=%u rUID=%s chapter=%s upstream=%" PRIu32 " error=%s\r\n",
                v3_manual_download_stage_name(stage),
                (unsigned)settings->internal.overlayNumber,
                ruid != NULL ? ruid : "",
                chapter != NULL ? chapter : "",
                upstream_status,
                error2text(error));
    return v3_manual_download_write_result(
        connection, stage,
        v3_manual_download_message(stage, error, upstream_status), ruid,
        chapter, upstream_status, version, completed, total);
}

error_t handleCloudContentDownloadV3(HttpConnection *connection, const char *ruid,
                                     const contentJson_t *content,
                                     client_ctx_t *client_ctx)
{
    char canonical_ruid[TB2_RUID_SIZE];
    settings_t *settings = client_ctx != NULL ? client_ctx->settings : NULL;
    if (connection == NULL || settings == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (settings->toniebox.boxGeneration != GENERATION_TB2 ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_GENERATION,
                                       ERROR_INVALID_PARAMETER, ruid, NULL, 0,
                                       0, 0, 0, FALSE);
    }
    if (!settings->cloud.tb2_v3_enabled)
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_MANIFEST,
                                       ERROR_ACCESS_DENIED, canonical_ruid,
                                       NULL, 0, 0, 0, 0, FALSE);
    }

    // A manual V3 download is an original-content operation. Keep the
    // established source and NoCloud semantics authoritative instead of
    // introducing a second policy path here.
    if (content == NULL || content->nocloud ||
        (content->source != NULL && content->source[0] != '\0') ||
        tb2_ruid_classify(canonical_ruid) != TB2_RUID_CONTENT)
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_POLICY,
                                       ERROR_ACCESS_DENIED, canonical_ruid,
                                       NULL, 0, 0, 0, 0, FALSE);
    }

    const uint8_t *stored_auth = content != NULL && content->_has_cloud_auth &&
                                         content->cloud_auth != NULL &&
                                         content->cloud_auth_len == TONIE_AUTH_TOKEN_LENGTH
                                     ? content->cloud_auth
                                     : NULL;
    tb2_content_identity_t identity;
    if (!tb2_content_identity_resolve(canonical_ruid, stored_auth, content,
                                      &identity) ||
        identity.auth == NULL || identity.auth_len != TONIE_AUTH_TOKEN_LENGTH)
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_AUTH,
                                       ERROR_AUTH_REQUIRED, canonical_ruid,
                                       NULL, 0, 0, 0, 0, FALSE);
    }

    char *meta_uri = custom_asprintf("/v3/content-meta/%s", identity.ruid);
    if (meta_uri == NULL)
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_MANIFEST,
                                       ERROR_OUT_OF_MEMORY, canonical_ruid,
                                       NULL, 0, 0, 0, 0, FALSE);
    }

    v3_native_meta_cbr_t meta;
    osMemset(&meta, 0, sizeof(meta));
    meta.cache_error = ERROR_IN_PROGRESS;
    fillBaseCtx(NULL, meta_uri, "", V3_CONTENT_META, &meta.passthrough,
                client_ctx);
    v3_native_cache_meta_capture_init(&meta.cache,
                                      settings->internal.cachedirfull,
                                      settings->internal.overlayNumber,
                                      canonical_ruid);
    if (meta.cache.failed)
    {
        v3_native_cache_meta_capture_abort(&meta.cache);
        osFreeMem(meta_uri);
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_MANIFEST,
                                       ERROR_OUT_OF_MEMORY, canonical_ruid,
                                       NULL, 0, 0, 0, 0, TRUE);
    }
    req_cbr_t meta_cbr = {
        .ctx = &meta,
        .response = v3_native_meta_response,
        .header = v3_native_meta_header,
        .body = v3_native_meta_body,
        .disconnect = v3_native_meta_disconnect,
    };
    TRACE_INFO("Starting TB2 manual content download overlay=%u rUID=%s upstreamRUID=%s\r\n",
               (unsigned)settings->internal.overlayNumber, canonical_ruid,
               identity.ruid);
    error_t request_error = cloud_request_tb2_get(
        settings->cloud.remote_hostname_tb2, 0, meta_uri, "", identity.auth,
        &meta_cbr);
    osFreeMem(meta_uri);

    v3_manual_download_stage_t meta_stage =
        meta.cache.status_code == 401 || meta.cache.status_code == 403
            ? V3_MANUAL_DOWNLOAD_AUTH
            : V3_MANUAL_DOWNLOAD_MANIFEST;
    if (request_error != NO_ERROR || meta.cache.status_code != 200 ||
        !meta.finished || meta.cache_error != NO_ERROR)
    {
        error_t error = request_error != NO_ERROR
                            ? request_error
                            : (meta.cache_error != NO_ERROR
                                   ? meta.cache_error
                                   : ERROR_INVALID_RESPONSE);
        uint32_t status_code = meta.cache.status_code;
        v3_native_cache_meta_capture_abort(&meta.cache);
        return v3_manual_download_fail(connection, settings, meta_stage, error,
                                       canonical_ruid, NULL, status_code, 0,
                                       0, 0, TRUE);
    }

    uint32_t meta_status = meta.cache.status_code;
    v3_native_cache_download_plan_t plan;
    error_t plan_error = v3_native_cache_download_plan_get(
        settings->internal.overlayNumber, canonical_ruid, &plan);
    v3_native_cache_meta_capture_abort(&meta.cache);
    if (plan_error != NO_ERROR)
    {
        v3_manual_download_stage_t stage = plan_error == ERROR_AUTH_REQUIRED
                                               ? V3_MANUAL_DOWNLOAD_AUTH
                                               : V3_MANUAL_DOWNLOAD_MANIFEST;
        return v3_manual_download_fail(connection, settings, stage, plan_error,
                                       canonical_ruid, NULL, meta_status,
                                       0, 0, 0, TRUE);
    }

    size_t completed = 0;
    for (size_t i = 0; i < plan.object_count; i++)
    {
        const v3_native_cache_download_object_t *object = &plan.objects[i];
        v3_native_cache_chapter_capture_t capture;
        char *serve_path = NULL;
        v3_native_cache_chapter_action_t action =
            v3_native_cache_chapter_prepare(settings->internal.cachedirfull,
                                            settings->internal.overlayNumber,
                                            object->name, &capture,
                                            &serve_path);
        if (action == V3_NATIVE_CHAPTER_SERVE ||
            action == V3_NATIVE_CHAPTER_STAGED)
        {
            osFreeMem(serve_path);
            completed++;
            continue;
        }
        if (action != V3_NATIVE_CHAPTER_CAPTURE)
        {
            v3_native_cache_chapter_abort(&capture);
            error_t response_error = v3_manual_download_fail(
                connection, settings, V3_MANUAL_DOWNLOAD_CHAPTER,
                ERROR_UNEXPECTED_STATE, canonical_ruid, object->name, 0,
                plan.version, completed, plan.object_count, TRUE);
            v3_native_cache_download_plan_free(&plan);
            return response_error;
        }

        char *chapter_uri = custom_asprintf("/v3/chapter/%s", object->name);
        char *chapter_query = object->auth[0] != '\0'
                                  ? custom_asprintf("auth=%s", object->auth)
                                  : strdup("");
        if (chapter_uri == NULL || chapter_query == NULL)
        {
            osFreeMem(chapter_uri);
            osFreeMem(chapter_query);
            v3_native_cache_chapter_abort(&capture);
            error_t response_error = v3_manual_download_fail(
                connection, settings, V3_MANUAL_DOWNLOAD_CHAPTER,
                ERROR_OUT_OF_MEMORY, canonical_ruid, object->name, 0,
                plan.version, completed, plan.object_count, TRUE);
            v3_native_cache_download_plan_free(&plan);
            return response_error;
        }

        v3_native_chapter_cbr_t chapter_context;
        osMemset(&chapter_context, 0, sizeof(chapter_context));
        chapter_context.cache = capture;
        chapter_context.cache_enabled = TRUE;
        chapter_context.cache_error = ERROR_IN_PROGRESS;
        osMemset(&capture, 0, sizeof(capture));
        fillBaseCtx(NULL, chapter_uri, chapter_query, V3_CHAPTER,
                    &chapter_context.passthrough, client_ctx);
        req_cbr_t chapter_cbr = {
            .ctx = &chapter_context,
            .response = v3_native_chapter_response,
            .header = v3_native_chapter_header,
            .body = v3_native_chapter_body,
            .disconnect = v3_native_chapter_disconnect,
        };
        TRACE_INFO("Downloading TB2 V3 object overlay=%u rUID=%s version=%" PRIu32 " object=%" PRIuSIZE "/%" PRIuSIZE " name=%s type=%s bytes=%" PRIu32 "\r\n",
                   (unsigned)settings->internal.overlayNumber,
                   canonical_ruid, plan.version, i + 1U, plan.object_count,
                   object->name, object->type, object->file_size);
        request_error = cloud_request_tb2_get(
            settings->cloud.remote_hostname_tb2, 0, chapter_uri,
            chapter_query, identity.auth, &chapter_cbr);
        osFreeMem(chapter_uri);
        osFreeMem(chapter_query);

        uint32_t chapter_status = chapter_context.status_code;
        error_t chapter_error = request_error != NO_ERROR
                                    ? request_error
                                    : (chapter_context.cache_error != NO_ERROR
                                           ? chapter_context.cache_error
                                           : ERROR_INVALID_RESPONSE);
        bool_t chapter_success = request_error == NO_ERROR &&
                                 chapter_context.status_code == 200 &&
                                 chapter_context.finished &&
                                 chapter_context.cache_error == NO_ERROR;
        v3_native_cache_chapter_abort(&chapter_context.cache);
        if (!chapter_success)
        {
            v3_manual_download_stage_t failure_stage =
                chapter_status == 401 || chapter_status == 403
                    ? V3_MANUAL_DOWNLOAD_AUTH
                    : V3_MANUAL_DOWNLOAD_CHAPTER;
            error_t response_error = v3_manual_download_fail(
                connection, settings, failure_stage, chapter_error,
                canonical_ruid, object->name, chapter_status, plan.version,
                completed, plan.object_count, TRUE);
            v3_native_cache_download_plan_free(&plan);
            return response_error;
        }
        completed++;
    }

    uint32_t active_version = 0;
    bool_t activated = v3_native_cache_active_version(
                           settings->internal.cachedirfull,
                           settings->internal.overlayNumber,
                           canonical_ruid, &active_version) &&
                       active_version == plan.version;
    uint32_t version = plan.version;
    size_t total = plan.object_count;
    v3_native_cache_download_plan_free(&plan);
    if (!activated)
    {
        return v3_manual_download_fail(connection, settings,
                                       V3_MANUAL_DOWNLOAD_ACTIVATION,
                                       ERROR_UNEXPECTED_STATE, canonical_ruid,
                                       NULL, 0, version, completed, total,
                                       TRUE);
    }

    TRACE_INFO("Completed TB2 manual content download overlay=%u rUID=%s version=%" PRIu32 " objects=%" PRIuSIZE "\r\n",
               (unsigned)settings->internal.overlayNumber, canonical_ruid,
               version, total);
    return v3_manual_download_write_result(connection,
                                           V3_MANUAL_DOWNLOAD_COMPLETE,
                                           "TB2 V3 cache version activated",
                                           canonical_ruid, NULL, 200, version,
                                           completed, total);
}

error_t handleCloudContentMetaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudContentMetaTime", current_time, client_ctx);

    TRACE_INFO("V3 Content Meta request: %s\n", uri);

    #define RUID_URI_CONTENT_META_BEGIN 17
    char ruid[17];
    error_t error = NO_ERROR;
    bool_t tonie_marked = false;
    bool noPassword = false;

    tonie_info_t *tonieInfo = getTonieInfoForRequest(connection, uri, RUID_URI_CONTENT_META_BEGIN, queryString, client_ctx, noPassword, ruid, &tonie_marked, &error);

    if (tonieInfo == NULL)
    {
        return error;
    }
    uint8_t *token = connection->private.authentication_token;
    char canonical_ruid[TB2_RUID_SIZE];
    bool_t cache_ruid_valid = tb2_ruid_canonicalize(ruid, canonical_ruid);
    bool_t source_configured = tonieInfo->json.source != NULL &&
                               tonieInfo->json.source[0] != '\0';

    bool_t local_candidate = tonieInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION ||
                             tonieInfo->json._source_type == CT_SOURCE_TONIEPLAY_COLLECTION ||
                             tonieInfo->json._source_type == CT_SOURCE_TAP_STREAM ||
                             (tonieInfo->exists && tonieInfo->valid &&
                              tonieInfo->json._source_type != CT_SOURCE_TAF_INCOMPLETE);
    if (local_candidate)
    {
        uint32_t naturalVersion = 0;
        if (tonieInfo->json._source_type == CT_SOURCE_TONIEPLAY_COLLECTION &&
            freshness_get_natural_server_audio_id(tonieInfo,
                                                  &naturalVersion, NULL))
        {
            uint32_t contentVersion = freshness_v3_content_meta_version(
                client_ctx->settings, tonieInfo, ruid, naturalVersion);
            uint8_t *manifest = NULL;
            size_t manifest_length = 0;
            error = contentVersion == 0
                        ? ERROR_FAILURE
                        : v3_tonieplay_library_activate(
                              client_ctx->settings->internal.librarydirfull,
                              tonieInfo->json.source,
                              client_ctx->settings->internal.overlayNumber,
                              ruid, contentVersion, &manifest,
                              &manifest_length);
            if (error == NO_ERROR)
            {
                connection->response.keepAlive = true;
                connection->response.noCache = true;
                httpPrepareHeader(connection, "application/json",
                                  manifest_length);
                error = httpWriteResponse(connection, manifest,
                                          manifest_length, false);
                cJSON_free(manifest);
                if (error == NO_ERROR)
                {
                    freshness_clear_cache_after_content_request(
                        client_ctx, V3_CONTENT_META, ruid);
                }
                freeTonieInfo(tonieInfo);
                return error;
            }
            cJSON_free(manifest);
        }
        v3_tonieplay_library_deactivate(
            client_ctx->settings->internal.overlayNumber);
        v3_local_content_generation_t generation;
        osMemset(&generation, 0, sizeof(generation));
        if (!freshness_get_natural_server_audio_id(tonieInfo, &naturalVersion, NULL))
        {
            error = ERROR_INVALID_FILE;
        }
        else
        {
            uint32_t contentVersion = freshness_v3_content_meta_version(client_ctx->settings,
                                                                        tonieInfo,
                                                                        ruid,
                                                                        naturalVersion);
            if (contentVersion == 0)
            {
                error = ERROR_FAILURE;
            }
            else if (tonieInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION)
            {
                error = v3_native_collection_generation_load(
                    client_ctx->settings, tonieInfo->json.source, ruid,
                    contentVersion, &generation);
            }
            else if (v3_source_is_tap(tonieInfo))
            {
                error = v3_tap_load_or_prepare_generation(
                    client_ctx->settings, tonieInfo, ruid, contentVersion,
                    &generation, &contentVersion);
            }
            else
            {
                error = v3_local_load_or_prepare_generation(
                    client_ctx->settings, tonieInfo, ruid, contentVersion,
                    TRUE, &generation);
            }
            if (error == NO_ERROR)
            {
                connection->response.keepAlive = true;
                error = sendLocalContentMetaV3(connection,
                                               client_ctx->settings,
                                               ruid,
                                               contentVersion,
                                               freshness_v3_resume_behavior(client_ctx->settings,
                                                                            &tonieInfo->json,
                                                                            ruid),
                                               tonieInfo->json.tonie_model,
                                               &generation);
                if (error == NO_ERROR)
                {
                    freshness_clear_cache_after_content_request(client_ctx,
                                                                V3_CONTENT_META,
                                                                ruid);
                }
                v3_local_content_generation_free(&generation);
                freeTonieInfo(tonieInfo);
                return error;
            }
        }

        v3_local_content_generation_free(&generation);
        TRACE_ERROR("Could not prepare complete V3 local generation for rUID %s: %s\r\n",
                    ruid,
                    error2text(error));
        bool_t authoritative_source = tonieInfo->json.source != NULL &&
                                      tonieInfo->json.source[0] != '\0';
        if (authoritative_source || !tonie_cloud_access_allowed(tonieInfo))
        {
            freeTonieInfo(tonieInfo);
            httpPrepareHeader(connection, NULL, 0);
            connection->response.statusCode = 500;
            return httpWriteResponse(connection, NULL, 0, false);
        }
        TRACE_WARNING("Falling back to TONIES after local V3 generation failure for rUID %s\r\n",
                      ruid);
    }

    uint64_t cache_uid = 0;
    bool_t cache_stale = cache_ruid_valid &&
                         freshness_ruid_to_uid(canonical_ruid, &cache_uid) &&
                         freshness_settings_array_contains(client_ctx->settings,
                                                           "internal.freshnessCache",
                                                           cache_uid);
    if (!source_configured && cache_ruid_valid &&
        client_ctx->settings->cloud.cacheContentV3 && !cache_stale)
    {
        uint8_t *manifest = NULL;
        size_t manifest_length = 0;
        uint32_t manifest_version = 0;
        error_t cache_error = v3_native_cache_read_active_manifest(
            client_ctx->settings->internal.cachedirfull,
            client_ctx->settings->internal.overlayNumber,
            canonical_ruid, &manifest, &manifest_length, &manifest_version);
        if (cache_error == NO_ERROR)
        {
            TRACE_INFO("TB2 V3 content route source=original-cache cache=hit overlay=%u rUID=%s effectiveVersion=%" PRIu32 " activeVersion=%" PRIu32 " requestedVersion=unknown action=local\r\n",
                       (unsigned)client_ctx->settings->internal.overlayNumber,
                       canonical_ruid, manifest_version, manifest_version);
            v3_native_import_library_if_enabled(client_ctx, canonical_ruid);
            connection->response.keepAlive = true;
            connection->response.noCache = true;
            httpPrepareHeader(connection, "application/json", manifest_length);
            error = httpWriteResponse(connection, manifest, manifest_length, false);
            osFreeMem(manifest);
            freeTonieInfo(tonieInfo);
            return error;
        }
        osFreeMem(manifest);
    }

    if (client_ctx->settings->cloud.tb2_v3_enabled &&
        client_ctx->settings->cloud.enableV3ContentMeta &&
        tonie_cloud_access_allowed(tonieInfo))
    {
        if (tonieInfo->json.cloud_override)
        {
            token = tonieInfo->json.cloud_auth;
            char msg[TONIE_AUTH_TOKEN_LENGTH * 2 + 1] = {0};
            convertTokenBytesToString(token, msg, client_ctx->settings->log.logFullAuth);
            osMemcpy((char_t *)&uri[RUID_URI_CONTENT_META_BEGIN], tonieInfo->json.cloud_ruid, osStrlen(tonieInfo->json.cloud_ruid));
            TRACE_INFO("Serve cloud from alternative rUID %s, auth %s\r\n", tonieInfo->json.cloud_ruid, msg);
        }

        v3_native_meta_cbr_t cache_ctx;
        osMemset(&cache_ctx, 0, sizeof(cache_ctx));
        fillBaseCtx(connection, uri, queryString, V3_CONTENT_META,
                    &cache_ctx.passthrough, client_ctx);
        if (cache_ruid_valid && client_ctx->settings->cloud.cacheContentV3)
        {
            v3_native_cache_meta_capture_init(
                &cache_ctx.cache, client_ctx->settings->internal.cachedirfull,
                client_ctx->settings->internal.overlayNumber, canonical_ruid);
        }
        else if (cache_ruid_valid)
        {
            v3_native_cache_meta_observe_init(
                &cache_ctx.cache, client_ctx->settings->internal.overlayNumber,
                canonical_ruid);
        }
        req_cbr_t cbr = {
            .ctx = &cache_ctx,
            .response = v3_native_meta_response,
            .header = v3_native_meta_header,
            .body = v3_native_meta_body,
            .disconnect = v3_native_meta_disconnect,
        };
        TRACE_INFO("TB2 V3 content route source=original-upstream cache=%s overlay=%u rUID=%s effectiveVersion=unknown activeVersion=%s requestedVersion=unknown action=upstream\r\n",
                   client_ctx->settings->cloud.cacheContentV3
                       ? (cache_stale ? "stale" : "miss")
                       : "disabled",
                   (unsigned)client_ctx->settings->internal.overlayNumber,
                   cache_ruid_valid ? canonical_ruid : ruid,
                   cache_stale ? "stale" : "none");
        error_t request_error = cloud_request_tb2_get(
            client_ctx->settings->cloud.remote_hostname_tb2, 0, uri,
            queryString, token, &cbr);
        v3_native_cache_meta_capture_abort(&cache_ctx.cache);
        if (!request_error)
        {
            freeTonieInfo(tonieInfo);
            return NO_ERROR;
        }
        freshness_clear_cache_after_content_request(client_ctx, V3_CONTENT_META, ruid);
    }
    else if (!tonie_cloud_access_allowed(tonieInfo))
    {
        TRACE_INFO("V3 content meta marked as no cloud and no content locally available\r\n");
    }

    freeTonieInfo(tonieInfo);

    httpPrepareHeader(connection, NULL, 0);
    connection->response.statusCode = 404;
    return httpWriteResponse(connection, NULL, 0, false);
}

error_t handleCloudChapterV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudChapterTime", current_time, client_ctx);

    TRACE_INFO("V3 Chapter request: %s\n", uri);

    bool_t local_chapter = osStrncmp(uri,
                                     V3_CHAPTER_URI_PREFIX,
                                     V3_CHAPTER_URI_PREFIX_LENGTH) == 0 &&
                           osStrncmp(uri + V3_CHAPTER_URI_PREFIX_LENGTH,
                                     "teddycloud_",
                                     V3_LOCAL_CHAPTER_PREFIX_LENGTH) == 0;
    if (local_chapter)
    {
        v3_local_chapter_request_t request;
        if (!v3_local_parse_chapter_request(uri, &request))
        {
            TRACE_DEBUG("Rejecting malformed V3 local chapter URI %s\r\n", uri);
            return v3_local_write_empty_status(connection, 404);
        }

        char requested_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
        error_t error = NO_ERROR;
        bool_t tonie_marked = FALSE;
        tonie_info_t *tonieInfo = getTonieInfoForRequest(connection,
                                                         uri,
                                                         (int)request.ruid_uri_offset,
                                                         queryString,
                                                         client_ctx,
                                                         false,
                                                         requested_ruid,
                                                         &tonie_marked,
                                                         &error);
        if (tonieInfo == NULL)
        {
            return error;
        }
        if (osStrcasecmp(requested_ruid, request.ruid) != 0)
        {
            freeTonieInfo(tonieInfo);
            return v3_local_write_empty_status(connection, 404);
        }

        uint32_t naturalVersion = 0;
        if (!freshness_get_natural_server_audio_id(tonieInfo, &naturalVersion, NULL))
        {
            TRACE_DEBUG("Rejecting V3 local chapter for rUID %s: content version unavailable\r\n",
                        request.ruid);
            freeTonieInfo(tonieInfo);
            return v3_local_write_empty_status(connection, 404);
        }
        uint32_t effectiveVersion = freshness_v3_content_meta_version(client_ctx->settings,
                                                                      tonieInfo,
                                                                      request.ruid,
                                                                      naturalVersion);
        if (effectiveVersion == 0)
        {
            TRACE_DEBUG("Rejecting V3 local chapter for rUID %s: effective version unavailable\r\n",
                        request.ruid);
            freeTonieInfo(tonieInfo);
            return v3_local_write_empty_status(connection, 404);
        }

        v3_local_content_generation_t generation;
        osMemset(&generation, 0, sizeof(generation));
        const v3_local_content_chapter_t *chapter = NULL;
        bool_t native_collection =
            tonieInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION;
        bool_t tap_source = v3_source_is_tap(tonieInfo);
        if (native_collection && request.format == V3_LOCAL_CHAPTER_HASHED)
        {
            error = v3_native_collection_generation_load(
                client_ctx->settings, tonieInfo->json.source, request.ruid,
                effectiveVersion, &generation);
            if (error == NO_ERROR)
            {
                char canonical_name[V3_LOCAL_CONTENT_NAME_SIZE];
                int name_length = osSnprintf(
                    canonical_name, sizeof(canonical_name),
                    "teddycloud_%s_%02" PRIuSIZE "_%s.opus",
                    request.hash_prefix, request.chapter_index, request.ruid);
                if (name_length > 0 &&
                    (size_t)name_length < sizeof(canonical_name))
                {
                    chapter = v3_local_content_find_chapter(&generation,
                                                            canonical_name);
                }
            }
        }
        else if (native_collection)
        {
            error = ERROR_NOT_FOUND;
            TRACE_DEBUG("Rejecting legacy V3 chapter for native collection rUID %s\r\n",
                        request.ruid);
        }
        else if (tap_source && request.format == V3_LOCAL_CHAPTER_HASHED)
        {
            uint32_t versions[4] = {0};
            size_t version_count = 0;
            v3_local_tap_state_t tap_state;
            bool_t tap_state_valid =
                v3_local_tap_state_load(client_ctx->settings->internal.cachedirfull,
                                        client_ctx->settings->internal.overlayNumber,
                                        request.ruid, &tap_state) == NO_ERROR &&
                tap_state.valid;
            if (tap_state_valid)
            {
                versions[version_count++] = tap_state.prepared_version;
                if (tap_state.playing_version != 0 &&
                    tap_state.playing_version != versions[0])
                {
                    versions[version_count++] = tap_state.playing_version;
                }
                if (tap_state.previous_version != 0 &&
                    tap_state.previous_version != versions[0] &&
                    (version_count < 2 || tap_state.previous_version != versions[1]))
                {
                    versions[version_count++] = tap_state.previous_version;
                }
            }
            else
            {
                versions[version_count++] = effectiveVersion;
            }

            error = ERROR_NOT_FOUND;
            for (size_t i = 0; i < version_count; i++)
            {
                error_t load_error = v3_local_content_generation_load(
                    client_ctx->settings->internal.cachedirfull,
                    client_ctx->settings->internal.overlayNumber,
                    request.ruid, versions[i], &generation);
                if (load_error != NO_ERROR ||
                    (tap_state_valid &&
                     generation.source_kind != V3_LOCAL_CONTENT_SOURCE_TAP))
                {
                    v3_local_content_generation_free(&generation);
                    continue;
                }

                char canonical_name[V3_LOCAL_CONTENT_NAME_SIZE];
                int name_length = osSnprintf(
                    canonical_name, sizeof(canonical_name),
                    "teddycloud_%s_%02" PRIuSIZE "_%s.opus",
                    request.hash_prefix, request.chapter_index, request.ruid);
                if (name_length > 0 &&
                    (size_t)name_length < sizeof(canonical_name))
                {
                    chapter = v3_local_content_find_chapter(&generation,
                                                            canonical_name);
                }
                if (chapter != NULL)
                {
                    effectiveVersion = versions[i];
                    error = NO_ERROR;
                    break;
                }
                v3_local_content_generation_free(&generation);
            }
        }
        else if (request.format == V3_LOCAL_CHAPTER_HASHED)
        {
            error = v3_local_content_generation_load(client_ctx->settings->internal.cachedirfull,
                                                     client_ctx->settings->internal.overlayNumber,
                                                     request.ruid,
                                                     effectiveVersion,
                                                     &generation);
            if (error == NO_ERROR)
            {
                char canonical_name[V3_LOCAL_CONTENT_NAME_SIZE];
                int name_length = osSnprintf(canonical_name,
                                             sizeof(canonical_name),
                                             "teddycloud_%s_%02" PRIuSIZE "_%s.opus",
                                             request.hash_prefix,
                                             request.chapter_index,
                                             request.ruid);
                if (name_length > 0 && (size_t)name_length < sizeof(canonical_name))
                {
                    chapter = v3_local_content_find_chapter(&generation,
                                                            canonical_name);
                }
            }
        }
        else
        {
            uint64_t uid = 0;
            bool_t legacy_allowed = freshness_ruid_to_uid(request.ruid, &uid) &&
                                    !freshness_settings_array_contains(client_ctx->settings,
                                                                       "internal.v3HashedChapterUids",
                                                                       uid) &&
                                    !freshness_source_changed_contains_ruid(client_ctx->settings,
                                                                            request.ruid);
            if (legacy_allowed)
            {
                TRACE_DEBUG("Serving transitional V3 legacy chapter teddycloud_%02" PRIuSIZE "_%s\r\n",
                            request.chapter_index,
                            request.ruid);
                error = v3_local_load_or_prepare_generation(client_ctx->settings,
                                                            tonieInfo,
                                                            request.ruid,
                                                            effectiveVersion,
                                                            FALSE,
                                                            &generation);
                if (error == NO_ERROR && request.chapter_index < generation.chapter_count)
                {
                    chapter = &generation.chapters[request.chapter_index];
                }
            }
            else
            {
                error = ERROR_NOT_FOUND;
                TRACE_DEBUG("Rejecting V3 legacy chapter for rUID %s: hash mode or source change active\r\n",
                            request.ruid);
            }
        }

        if (error != NO_ERROR || chapter == NULL)
        {
            TRACE_DEBUG("Rejecting stale or unknown V3 local chapter for rUID %s version=%" PRIu32 ": %s\r\n",
                        request.ruid,
                        effectiveVersion,
                        error2text(error));
            v3_local_content_generation_free(&generation);
            freeTonieInfo(tonieInfo);
            return v3_local_write_empty_status(connection, 404);
        }

        if (request.format == V3_LOCAL_CHAPTER_LEGACY && connection->request.Range.start > 0)
        {
            TRACE_DEBUG("Rejecting resumed V3 legacy chapter for rUID %s with 416 to prevent mixed .part bytes\r\n",
                        request.ruid);
            uint32_t file_size = chapter->file_size;
            v3_local_content_generation_free(&generation);
            freeTonieInfo(tonieInfo);
            return v3_local_reject_legacy_range(connection, file_size);
        }

        TRACE_INFO("Serve V3 local chapter overlay=%u rUID=%s version=%" PRIu32 " chapter=%" PRIuSIZE " name=%s bytes=%" PRIu32 "\r\n",
                   (unsigned)client_ctx->settings->internal.overlayNumber,
                   request.ruid,
                   effectiveVersion,
                   chapter->index,
                   chapter->name,
                   chapter->file_size);
        connection->response.keepAlive = true;
        connection->response.contentType = "audio/ogg";
        error = httpSendResponseStreamUnsafe(connection,
                                             uri,
                                             chapter->path,
                                             false);
        if (error == NO_ERROR)
        {
            freshness_clear_cache_after_content_request(client_ctx,
                                                        V3_CHAPTER,
                                                        request.ruid);
        }
        else
        {
            TRACE_WARNING("Could not send V3 local chapter %s: %s\r\n",
                          chapter->name,
                          error2text(error));
        }
        v3_local_content_generation_free(&generation);
        freeTonieInfo(tonieInfo);
        return error;
    }

    v3_native_cache_chapter_capture_t native_capture;
    osMemset(&native_capture, 0, sizeof(native_capture));
    v3_native_cache_chapter_action_t native_action = V3_NATIVE_CHAPTER_BYPASS;
    char *native_path = NULL;
    const char *native_name = osStrncmp(uri, V3_CHAPTER_URI_PREFIX,
                                        V3_CHAPTER_URI_PREFIX_LENGTH) == 0
                                  ? uri + V3_CHAPTER_URI_PREFIX_LENGTH
                                  : NULL;
    if (native_name != NULL)
    {
        char *tonieplay_path = NULL;
        char tonieplay_content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE] = {0};
        if (v3_tonieplay_library_resolve(
                client_ctx->settings->internal.overlayNumber, native_name,
                &tonieplay_path, tonieplay_content_type))
        {
            TRACE_INFO("Serve local TB2 Tonieplay object overlay=%u name=%s contentType=%s\r\n",
                       (unsigned)client_ctx->settings->internal.overlayNumber,
                       native_name, tonieplay_content_type);
            connection->response.keepAlive = true;
            connection->response.noCache = true;
            connection->response.contentType = tonieplay_content_type;
            error_t local_error = httpSendResponseStreamUnsafe(
                connection, uri, tonieplay_path, false);
            osFreeMem(tonieplay_path);
            return local_error;
        }
        if (v3_tonieplay_library_route_assigned(
                client_ctx->settings->internal.overlayNumber))
        {
            TRACE_DEBUG("Rejecting object outside active local Tonieplay collection overlay=%u name=%s\r\n",
                        (unsigned)client_ctx->settings->internal.overlayNumber,
                        native_name);
            return v3_local_write_empty_status(connection, 404);
        }
        native_action = v3_native_cache_chapter_prepare(
            client_ctx->settings->internal.cachedirfull,
            client_ctx->settings->internal.overlayNumber, native_name,
            &native_capture, &native_path);
        if (native_action == V3_NATIVE_CHAPTER_SERVE)
        {
            TRACE_INFO("TB2 V3 chapter route source=original-cache cache=hit overlay=%u rUID=%s activeVersion=%" PRIu32 " requestedVersion=%" PRIu32 " name=%s action=local\r\n",
                       (unsigned)client_ctx->settings->internal.overlayNumber,
                       native_capture.ruid, native_capture.version,
                       native_capture.version, native_name);
            connection->response.keepAlive = true;
            connection->response.noCache = true;
            connection->response.contentType =
                native_capture.content_type[0] != '\0'
                    ? native_capture.content_type
                    : "application/octet-stream";
            error_t cache_error = httpSendResponseStreamUnsafe(
                connection, uri, native_path, false);
            osFreeMem(native_path);
            return cache_error;
        }
        if (native_action == V3_NATIVE_CHAPTER_REJECT)
        {
            TRACE_WARNING("Rejecting inconsistent TB2 V3 cache chapter mapping overlay=%u name=%s\r\n",
                          (unsigned)client_ctx->settings->internal.overlayNumber,
                          native_name);
            return v3_local_write_empty_status(connection, 500);
        }
        if (native_action == V3_NATIVE_CHAPTER_CAPTURE ||
            native_action == V3_NATIVE_CHAPTER_FORWARD ||
            native_action == V3_NATIVE_CHAPTER_STAGED)
        {
            tonie_info_t *route_info = getTonieInfoFromRuid(
                native_capture.ruid, false, client_ctx->settings);
            bool_t cloud_allowed = route_info != NULL &&
                                   tonie_cloud_access_allowed(route_info);
            if (route_info != NULL)
            {
                freeTonieInfo(route_info);
            }
            if (!cloud_allowed)
            {
                TRACE_INFO("Rejecting TONIES V3 chapter fallback for NoCloud rUID %s name=%s\r\n",
                           native_capture.ruid, native_name);
                v3_native_cache_chapter_abort(&native_capture);
                return v3_local_write_empty_status(connection, 404);
            }
        }
        else if (native_action == V3_NATIVE_CHAPTER_BYPASS)
        {
            TRACE_DEBUG("Rejecting V3 chapter without current content-meta route overlay=%u name=%s\r\n",
                        (unsigned)client_ctx->settings->internal.overlayNumber,
                        native_name);
            return v3_local_write_empty_status(connection, 404);
        }
    }

    if (client_ctx->settings->cloud.tb2_v3_enabled && client_ctx->settings->cloud.enableV3Chapter)
    {
        cbr_ctx_t ctx;
        v3_native_chapter_cbr_t cache_ctx;
        req_cbr_t cbr;
        if (native_action == V3_NATIVE_CHAPTER_CAPTURE ||
            native_action == V3_NATIVE_CHAPTER_FORWARD ||
            native_action == V3_NATIVE_CHAPTER_STAGED)
        {
            osMemset(&cache_ctx, 0, sizeof(cache_ctx));
            cache_ctx.cache = native_capture;
            cache_ctx.cache_enabled = native_action == V3_NATIVE_CHAPTER_CAPTURE;
            osMemset(&native_capture, 0, sizeof(native_capture));
            fillBaseCtx(connection, uri, queryString, V3_CHAPTER,
                        &cache_ctx.passthrough, client_ctx);
            cbr = (req_cbr_t){
                .ctx = &cache_ctx,
                .response = v3_native_chapter_response,
                .header = v3_native_chapter_header,
                .body = v3_native_chapter_body,
                .disconnect = v3_native_chapter_disconnect,
            };
        }
        else
        {
            cbr = getCloudCbr(connection, uri, queryString, V3_CHAPTER, &ctx,
                              client_ctx);
        }
        // Note: chapter uses a query parameter `auth=...` which is extracted in handler if we need it, but proxying it just passes URI+Query.
        // We will pass the authentication token if available, though for V3_CHAPTER it seems to use `?auth=XXX` anyway.
        error_t request_error = cloud_request_tb2_get(
            client_ctx->settings->cloud.remote_hostname_tb2, 0, uri,
            queryString, NULL, &cbr);
        if (native_action == V3_NATIVE_CHAPTER_CAPTURE ||
            native_action == V3_NATIVE_CHAPTER_FORWARD ||
            native_action == V3_NATIVE_CHAPTER_STAGED)
        {
            v3_native_cache_chapter_abort(&cache_ctx.cache);
        }
        if (!request_error)
        {
            return NO_ERROR;
        }
    }

    if (native_action == V3_NATIVE_CHAPTER_CAPTURE ||
        native_action == V3_NATIVE_CHAPTER_FORWARD ||
        native_action == V3_NATIVE_CHAPTER_STAGED)
    {
        v3_native_cache_chapter_abort(&native_capture);
    }

    httpPrepareHeader(connection, NULL, 0);
    connection->response.statusCode = 404;
    return httpWriteResponse(connection, NULL, 0, false);
}

error_t handleCloudOtaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    error_t ret = NO_ERROR;
    char *query = strdup(queryString);
    char *localUri = strdup(uri);
    char *savelocalUri = localUri;
    
    char *typeStr = strtok_r(&localUri[8], "/", &savelocalUri);
    char *hash = strtok_r(NULL, "?", &savelocalUri);
    
    if (!typeStr || !hash) {
        osFreeMem(localUri);    
        osFreeMem(query);
        return ERROR_FAILURE;
    }
    
    cloudapi_ota_t fileId = (cloudapi_ota_t)atoi(typeStr);
    
    TRACE_INFO(" >> V3 OTA-Request for type %d with hash %s\r\n", fileId, hash);

    char *folder;
    switch (client_ctx->settings->internal.toniebox_firmware.boxIC)
    {
    case BOX_CC3200:
        folder = custom_asprintf("cc3200%c", PATH_SEPARATOR);
        break;
    case BOX_CC3235:
        folder = custom_asprintf("cc3235%c", PATH_SEPARATOR);
        break;
    case BOX_ESP32:
        folder = custom_asprintf("esp32%c", PATH_SEPARATOR);
        break;
    case BOX_TB2:
        folder = custom_asprintf("tb2%c", PATH_SEPARATOR);
        break;
    default:
        folder = strdup("");
        break;
    }
    char *local_dir = custom_asprintf("%s%cota%c%s%" PRIu8 "%c", client_ctx->settings->internal.firmwaredirfull, PATH_SEPARATOR, PATH_SEPARATOR, folder, fileId, PATH_SEPARATOR);
    osFreeMem(folder);

    // Provide the original URI and everything to cache it
    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudOtaTime", current_time, client_ctx);

    if (client_ctx->settings->cloud.tb2_v3_enabled && client_ctx->settings->cloud.enableV3Ota)
    {
        ota_ctx_t ota_ctx;
        cbr_ctx_t ctx;
        req_cbr_t cbr;
        
        if (client_ctx->settings->cloud.cacheOta)
        {
            cbr = getCloudOtaCbr(NULL, uri, queryString, V3_OTA, &ctx, client_ctx);
        }
        else
        {
            cbr = getCloudCbr(connection, uri, queryString, V3_OTA, &ctx, client_ctx);
        }
        ota_ctx.fileId = fileId;
        ctx.customData = &ota_ctx;
        cloud_request_tb2_get(client_ctx->settings->cloud.remote_hostname_tb2, 0, uri, queryString, NULL, &cbr);

        if (!client_ctx->settings->cloud.cacheOta)
        {
            osFreeMem(local_dir);
            osFreeMem(query);
            osFreeMem(localUri);
            return ret;
        }
    }

    char *local_file = custom_asprintf("%s%s.bin", local_dir, hash);
    bool new_ota = false;
    
    if (fsFileExists(local_file))
    {
        if (client_ctx->settings->cloud.localOta)
        {
            TRACE_INFO(" >> Found OTA %" PRIu8 " with hash %s\r\n", fileId, hash);
            new_ota = true;
        }
        else
        {
            TRACE_INFO(" >> Found OTA %" PRIu8 " with hash %s but local OTA disabled\r\n", fileId, hash);
        }
    }
    else
    {
        TRACE_INFO(" >> No OTA found for %" PRIu8 " %s\r\n", fileId, hash);
    }
    
    if (new_ota)
    {
        ret = httpSendResponseStreamUnsafe(connection, uri, local_file, false);
    }
    else
    {
        httpPrepareHeader(connection, NULL, 0);
        connection->response.statusCode = 404; // Not found locally (or ignored)
        ret = httpWriteResponse(connection, NULL, 0, false);
    }

    osFreeMem(local_file);
    osFreeMem(local_dir);
    osFreeMem(query);
    osFreeMem(localUri);
    return ret;
}

error_t handleCloudReset(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastCloudResetTime", current_time, client_ctx);

    // EMPTY POST REQUEST?
    if (client_ctx->settings->cloud.enabled && client_ctx->settings->cloud.enableV1CloudReset)
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, queryString, V1_CLOUDRESET, &ctx, client_ctx);
        cloud_request_post(NULL, 0, uri, queryString, NULL, 0, NULL, &cbr);
    }
    else
    {
        httpPrepareHeader(connection, "application/json; charset=utf-8", 2);
        connection->response.keepAlive = true;
        return httpWriteResponseString(connection, "{}", false);
    }
    return NO_ERROR;
}
