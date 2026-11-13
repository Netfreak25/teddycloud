#include "original_content_transfer.h"
#include "taf_original_cache.h"
#include "v3_native_cache.h"
#include "fs_ext.h"
#include "mutex_manager.h"
#include "server_helpers.h"
#include <string.h>

error_t original_content_publish_taf(const char *staged, const char *raw,
    settings_t *settings, bool import_to_library)
{
    if (!taf_original_file_complete(staged) || !isValidTaf(staged, true))
        return ERROR_INVALID_FILE;
    mutex_lock(MUTEX_ORIGINAL_CONTENT_FILES);
    /* Both files are on the same content filesystem. Never copy/truncate a
     * published TAF, nor write through a resolved Library reference. */
    error_t error = fsRenameFile(staged, raw);
    if (error == NO_ERROR && import_to_library)
        error = taf_original_cache_import(raw, settings);
    mutex_unlock(MUTEX_ORIGINAL_CONTENT_FILES);
    return error;
}

typedef struct
{
    cbr_ctx_t base;
    FsFile *file;
    const char *temporary;
    error_t error;
    uint32_t status;
    bool complete;
} original_taf_response_t;

static void original_taf_response(void *source, HttpClientContext *cloud)
{
    original_taf_response_t *ctx = source;
    ctx->status = cloud->statusCode;
    if (ctx->status != 200)
    {
        ctx->error = ERROR_INVALID_RESPONSE;
        return;
    }
    ctx->file = fsOpenFile(ctx->temporary, FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
    if (ctx->file == NULL)
        ctx->error = ERROR_FILE_OPENING_FAILED;
}

static void original_taf_body(void *source, HttpClientContext *cloud,
    const char *data, size_t length, error_t error)
{
    (void)cloud;
    original_taf_response_t *ctx = source;
    if (ctx->error == NO_ERROR && length != 0 && ctx->file != NULL)
        ctx->error = fsWriteFile(ctx->file, (void *)data, length);
    if (error == ERROR_END_OF_STREAM && !ctx->complete)
    {
        ctx->complete = true;
        if (ctx->file != NULL)
        {
            error_t flush = fsFlushFile(ctx->file);
            if (ctx->error == NO_ERROR)
                ctx->error = flush;
            fsCloseFile(ctx->file);
            ctx->file = NULL;
        }
    }
    else if (error != NO_ERROR && error != ERROR_END_OF_STREAM)
        ctx->error = error;
}

static error_t original_content_materialize_taf(original_content_cache_job_t *job,
    bool import_to_library, const char **reason)
{
    settings_t *settings = &job->settings;
    char *raw = NULL;
    getContentPathFromCharRUID(job->ruid, &raw, settings);
    if (raw == NULL)
        return ERROR_OUT_OF_MEMORY;
    char *reference = taf_original_cache_resolve(raw, settings);
    bool raw_complete = taf_original_file_complete(raw);
    if (reference != NULL || raw_complete)
    {
        error_t error = NO_ERROR;
        if (import_to_library && raw_complete)
        {
            mutex_lock(MUTEX_ORIGINAL_CONTENT_FILES);
            error = original_content_cache_job_current(job, ORIGINAL_CACHE_TAF)
                ? taf_original_cache_import(raw, settings) : ERROR_ACCESS_DENIED;
            mutex_unlock(MUTEX_ORIGINAL_CONTENT_FILES);
        }
        osFreeMem(reference);
        osFreeMem(raw);
        return error;
    }
    char *directory = strdup(raw);
    char *temporary = custom_asprintf("%s.original-download.tmp", raw);
    if (directory == NULL || temporary == NULL)
    {
        osFreeMem(directory);
        osFreeMem(temporary);
        osFreeMem(raw);
        return ERROR_OUT_OF_MEMORY;
    }
    directory[strlen(directory) - 9] = '\0';
    error_t error = fsCreateDir(directory);
    if (fsDirExists(directory))
        error = NO_ERROR;
    osFreeMem(directory);
    char *uri = custom_asprintf("/v2/content/%s", job->upstream_ruid);
    client_ctx_t client = {0};
    client.settings = settings;
    original_taf_response_t ctx = {0};
    fillBaseCtx(NULL, uri, "", V2_CONTENT, &ctx.base, &client);
    ctx.base.tb1_identity = &job->tb1_identity;
    ctx.base.reject_redirects = true;
    ctx.temporary = temporary;
    req_cbr_t callbacks = {.ctx = &ctx, .response = original_taf_response,
                          .body = original_taf_body};
    if (error == NO_ERROR && uri != NULL && original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF))
        error = cloud_request_get(settings->cloud.remote_hostname, 0, uri, "", job->auth, &callbacks);
    else if (error == NO_ERROR)
        error = uri == NULL ? ERROR_OUT_OF_MEMORY : ERROR_ACCESS_DENIED;
    if (ctx.file != NULL)
        fsCloseFile(ctx.file);
    if (error == NO_ERROR)
        error = ctx.error;
    if (error == NO_ERROR && (!ctx.complete || ctx.status != 200))
        error = ERROR_INVALID_RESPONSE;
    if (error == NO_ERROR && !original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF))
        error = ERROR_ACCESS_DENIED;
    if (error == NO_ERROR)
        error = original_content_publish_taf(temporary, raw, settings, import_to_library);
    if (ctx.status == 404 && reason != NULL)
        *reason = "taf_not_available";
    if (fsFileExists(temporary))
        fsDeleteFile(temporary);
    osFreeMem(temporary);
    osFreeMem(uri);
    osFreeMem(raw);
    return error;
}

error_t original_content_cache_materialize(original_content_cache_job_t *job,
    original_cache_format_t format, bool import_to_library, const char **reason)
{
    if (!original_content_cache_job_current(job, format))
    {
        if (reason != NULL)
            *reason = "policy_changed_or_disabled";
        return ERROR_ACCESS_DENIED;
    }
    if (format == ORIGINAL_CACHE_TAF)
        return original_content_materialize_taf(job, import_to_library, reason);
    settings_t *settings = &job->settings;
    uint32_t version = 0;
    bool_t tonieplay = false;
    error_t error = NO_ERROR;
    if (job->refresh_v3 || !v3_native_cache_active_info(settings->internal.cachedirfull,
        settings->internal.librarydirfull, settings->internal.overlayNumber,
        job->ruid, &version, NULL, &tonieplay))
    {
        error = original_content_download_v3(job);
        if (error == NO_ERROR && !v3_native_cache_active_info(settings->internal.cachedirfull,
            settings->internal.librarydirfull, settings->internal.overlayNumber,
            job->ruid, &version, NULL, &tonieplay))
            error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR && (tonieplay ? settings->cloud.cacheTonieplayToLibraryV3 : import_to_library))
    {
        if (!original_content_cache_job_current(job, format))
            return ERROR_ACCESS_DENIED;
        error = tonieplay ? v3_native_cache_import_active_tonieplay_library_version(
                settings->internal.cachedirfull, settings->internal.librarydirfull,
                settings->internal.overlayNumber, job->ruid, version)
            : v3_native_cache_import_active_library_version(settings->internal.cachedirfull,
                settings->internal.librarydirfull, settings->internal.overlayNumber,
                job->ruid, version, NULL);
    }
    if (error == NO_ERROR && job->refresh_v3 && reason != NULL)
        *reason = job->refresh_result;
    if (error != NO_ERROR && job->refresh_v3)
        job->refresh_result = NULL;
    return error;
}
