#include "original_content_api.h"
#include "original_content_cache.h"
#include "handler_api.h"
#include "server_helpers.h"
#include "v3_native_cache.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define ORIGINAL_CACHE_API "/api/content/cache/"
#define NATIVE_AUDIO_API "/api/content/native/"

static error_t original_api_json(HttpConnection *connection, uint_t status, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL)
        return ERROR_OUT_OF_MEMORY;
    httpPrepareHeader(connection, "application/json; charset=utf-8", strlen(body));
    connection->response.statusCode = status;
    connection->response.noCache = TRUE;
    return httpWriteResponse(connection, body, strlen(body), true);
}

static error_t original_api_error(HttpConnection *connection, uint_t status, const char *reason)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "error", reason);
    return original_api_json(connection, status, json);
}

/* These endpoints accept exact single query keys, never truncated/embedded-NUL
 * values or substring matches from the legacy generic query parser. */
static bool original_api_query(const char *query, const char *key, char *value, size_t capacity)
{
    value[0] = '\0';
    bool found = false;
    for (const char *part = query; part != NULL && *part != '\0';)
    {
        const char *end = strchr(part, '&');
        if (end == NULL) end = part + strlen(part);
        size_t key_length = strlen(key);
        if ((size_t)(end - part) > key_length && !strncmp(part, key, key_length) && part[key_length] == '=')
        {
            if (found) return false;
            found = true;
            size_t used = 0;
            for (const char *p = part + key_length + 1; p < end; p++)
            {
                unsigned char ch = (unsigned char)*p;
                if (ch == '%')
                {
                    if (end - p < 3 || !isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2]))
                        return false;
                    char hex[3] = {p[1], p[2], '\0'};
                    ch = (unsigned char)strtoul(hex, NULL, 16);
                    p += 2;
                }
                if (ch == 0 || ch == '+' || used + 1 >= capacity)
                    return false;
                value[used++] = (char)ch;
            }
            value[used] = '\0';
        }
        part = *end == '&' ? end + 1 : NULL;
    }
    return true;
}

/* An invalid nonempty overlay must never fall back to global credentials. */
static bool original_api_overlay(const char *query, client_ctx_t *ctx)
{
    char overlay[ORIGINAL_CONTENT_CACHE_OWNER_SIZE] = {0};
    if (!original_api_query(query, "overlay", overlay, sizeof(overlay)))
        return false;
    uint8_t id = get_overlay_id(overlay);
    if (overlay[0] != '\0' && (id == 0 || !get_settings_id(id)->internal.config_used))
        return false;
    ctx->settings = get_settings_id(id);
    return ctx->settings != NULL;
}

error_t handleApiOriginalCache(HttpConnection *connection, const char *uri,
    const char *query, client_ctx_t *ctx)
{
    char ruid[TB2_RUID_SIZE];
    if (!original_api_overlay(query, ctx) ||
        !tb2_ruid_canonicalize(uri + strlen(ORIGINAL_CACHE_API), ruid))
        return original_api_error(connection, 400, "invalid_ruid_or_overlay");
    char refresh[8];
    if (!original_api_query(query, "refresh", refresh, sizeof(refresh)) ||
        (refresh[0] && strcmp(refresh, "v3")))
        return original_api_error(connection, 400, "invalid_refresh");
    if (refresh[0])
    {
        cJSON *json = cJSON_CreateObject();
        cJSON *targets = cJSON_AddArrayToObject(json, "targets");
        if (!targets) { cJSON_Delete(json); return ERROR_OUT_OF_MEMORY; }
        error_t error = original_content_cache_refresh_group(ctx->settings, ruid, targets);
        cJSON_AddBoolToObject(json, "ok", error == NO_ERROR);
        if (error)
            cJSON_AddStringToObject(json, "error", error == ERROR_OUT_OF_MEMORY
                ? "out_of_memory" : cJSON_GetArraySize(targets)
                ? "refresh_not_accepted" : "no_tb2_in_group");
        uint_t status = error == NO_ERROR ? 202 :
            error == ERROR_OUT_OF_RESOURCES || error == ERROR_OUT_OF_MEMORY ? 503 : 409;
        return original_api_json(connection, status, json);
    }
    original_cache_format_t preferred = original_content_cache_preferred(ctx->settings, ruid);
    const char *reason = NULL;
    error_t error = original_content_cache_submit(ctx->settings, ruid,
        preferred, false, 0, &reason);
    if (error != NO_ERROR)
        return original_api_error(connection,
            error == ERROR_OUT_OF_RESOURCES ? 503 : 409,
            reason != NULL ? reason : "cache_request_rejected");
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    original_content_cache_add_status(json, ctx->settings, ruid);
    return original_api_json(connection, 202, json);
}

static bool original_api_uint(const char *text, uint32_t *number)
{
    if (text == NULL || *text == '\0')
        return false;
    uint64_t value = 0;
    for (; *text; text++)
    {
        if (*text < '0' || *text > '9')
            return false;
        value = value * 10 + (unsigned)(*text - '0');
        if (value > UINT32_MAX)
            return false;
    }
    *number = (uint32_t)value;
    return true;
}

static error_t original_api_audio_file(HttpConnection *connection,
    const v3_native_library_collection_chapter_t *chapter)
{
    uint32_t size = 0;
    if (fsGetFileSize(chapter->path, &size) != NO_ERROR || size != chapter->file_size)
        return original_api_error(connection, 404, "chapter_no_longer_available");
    FsFile *file = fsOpenFile(chapter->path, FS_FILE_MODE_READ);
    if (file == NULL)
        return ERROR_NOT_FOUND;
    uint32_t start = connection->request.Range.start;
    uint32_t end = connection->request.Range.end;
    bool partial = start != 0 || end != 0;
    if (end == 0 || end >= size)
        end = size ? size - 1 : 0;
    if (size == 0 || start >= size || start > end)
    {
        fsCloseFile(file);
        return original_api_error(connection, 416, "invalid_range");
    }
    char range[96];
    uint32_t remaining = end - start + 1;
    httpPrepareHeader(connection, "audio/ogg", remaining);
    connection->response.noCache = TRUE;
    connection->response.statusCode = partial ? 206 : 200;
    if (partial)
    {
        osSnprintf(range, sizeof(range), "bytes %" PRIu32 "-%" PRIu32 "/%" PRIu32, start, end, size);
        connection->response.contentRange = range;
    }
    error_t error = fsSeekFile(file, start, FS_SEEK_SET);
    if (error == NO_ERROR)
        error = httpWriteHeader(connection);
    connection->response.contentRange = NULL;
    while (error == NO_ERROR && remaining > 0)
    {
        size_t length = MIN(remaining, HTTP_SERVER_BUFFER_SIZE);
        size_t read = 0;
        error = fsReadFile(file, connection->buffer, length, &read);
        if (read > 0)
        {
            error = httpWriteStream(connection, connection->buffer, read);
            remaining -= read;
        }
        else if (error == NO_ERROR)
            error = ERROR_END_OF_FILE;
    }
    fsCloseFile(file);
    return error == NO_ERROR && remaining == 0 ? httpFlushStream(connection) : error;
}

error_t handleApiNativeContent(HttpConnection *connection, const char *uri,
    const char *query, client_ctx_t *ctx)
{
    const char *suffix = uri + strlen(NATIVE_AUDIO_API);
    char ruid[TB2_RUID_SIZE];
    char raw[TB2_RUID_SIZE];
    if (strlen(suffix) < TB2_RUID_SIZE - 1 || !original_api_overlay(query, ctx))
        return original_api_error(connection, 400, "invalid_ruid_or_overlay");
    memcpy(raw, suffix, TB2_RUID_SIZE - 1);
    raw[TB2_RUID_SIZE - 1] = '\0';
    if (!tb2_ruid_canonicalize(raw, ruid))
        return original_api_error(connection, 400, "invalid_ruid");
    suffix += TB2_RUID_SIZE - 1;
    uint32_t index = 0, expected_version = 0;
    bool chapter = *suffix != '\0';
    if (chapter)
    {
        char digits[16] = {0}, version[16] = {0};
        int end = 0;
        if (sscanf(suffix, "/chapters/%15[0-9].opus%n", digits, &end) != 1 ||
            end == 0 || suffix[end] != '\0' || !original_api_uint(digits, &index) ||
            !original_api_query(query, "contentVersion", version, sizeof(version)) ||
            !original_api_uint(version, &expected_version) || expected_version == 0)
            return original_api_error(connection, 400, "invalid_chapter_or_version");
    }
    v3_native_library_collection_t audio = {0};
    uint32_t version = 0;
    error_t error = v3_native_cache_audio_snapshot(ctx->settings->internal.cachedirfull,
        ctx->settings->internal.librarydirfull, ctx->settings->internal.overlayNumber,
        ruid, expected_version, &version, &audio);
    if (error != NO_ERROR)
        return original_api_error(connection, 404, "complete_native_audio_not_found");
    if (chapter)
        error = index < audio.chapter_count ? original_api_audio_file(connection, &audio.chapters[index])
            : original_api_error(connection, 404, "chapter_not_found");
    else
    {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddNumberToObject(json, "contentVersion", version);
        cJSON *chapters = cJSON_AddArrayToObject(json, "chapters");
        for (size_t i = 0; i < audio.chapter_count; i++)
        {
            char *url = custom_asprintf(NATIVE_AUDIO_API "%s/chapters/%" PRIuSIZE ".opus?overlay=%s&contentVersion=%" PRIu32,
                ruid, i, ctx->settings->internal.overlayUniqueId, version);
            if (url == NULL)
            {
                cJSON_Delete(json);
                v3_native_library_collection_free(&audio);
                return ERROR_OUT_OF_MEMORY;
            }
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "index", i);
            cJSON_AddNumberToObject(entry, "size", audio.chapters[i].file_size);
            cJSON_AddStringToObject(entry, "url", url);
            cJSON_AddItemToArray(chapters, entry);
            osFreeMem(url);
        }
        error = original_api_json(connection, 200, json);
    }
    v3_native_library_collection_free(&audio);
    return error;
}
