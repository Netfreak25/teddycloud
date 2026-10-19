/* Production parsers/transfer/TAF validation; only network, policy and I/O faults
 * are controlled. No server or user content is involved. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRACE_LEVEL 0
#include "../src/original_content_api.c"
#include "../src/taf_original_cache.c"

static bool write_failure, flush_failure;
static unsigned write_calls, flush_calls, close_calls;
static error_t transfer_write(FsFile *file, void *data, size_t length)
{
    write_calls++;
    if (write_failure)
    {
        if (length) assert(fsWriteFile(file, data, 1) == NO_ERROR);
        return ERROR_WRITE_FAILED;
    }
    return fsWriteFile(file, data, length);
}
static error_t transfer_flush(FsFile *file)
{
    flush_calls++;
    return flush_failure ? ERROR_WRITE_FAILED : fsFlushFile(file);
}
static void transfer_close(FsFile *file)
{
    close_calls++;
    fsCloseFile(file);
}
#define fsWriteFile transfer_write
#define fsFlushFile transfer_flush
#define fsCloseFile transfer_close
#include "../src/original_content_transfer.c"
#undef fsWriteFile
#undef fsFlushFile
#undef fsCloseFile

static settings_t settings[3];
static OsMutex locks[MUTEX_LAST];
static const char *scenario;
static const char *box_id = "D4594404DEAC";
static const char *ruid = "8194F21C500304E0";
static const char *raw = "content/8194F21C/500304E0";
static uint8_t taf_bytes[TONIEFILE_FRAME_SIZE * 2];
static unsigned requests;

settings_t *get_settings(void) { return &settings[0]; }
settings_t *get_settings_id(uint8_t id) { assert(id < 3); return &settings[id]; }
uint8_t get_overlay_id(const char *id)
{
    if (!strcmp(id, box_id)) return 1;
    return !strcmp(id, "UNUSED") ? 2 : 0;
}
void mutex_lock(mutex_id_t id) { osAcquireMutex(&locks[id]); }
void mutex_unlock(mutex_id_t id) { osReleaseMutex(&locks[id]); }
char *custom_asprintf(const char *format, ...)
{
    va_list args, copy;
    va_start(args, format); va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, args);
    va_end(args);
    assert(length >= 0);
    char *result = malloc((size_t)length + 1);
    assert(result);
    vsnprintf(result, (size_t)length + 1, format, copy);
    va_end(copy);
    return result;
}
void getContentPathFromCharRUID(char id[17], char **path, settings_t *config)
{
    *path = custom_asprintf("%s/%.8s/%.8s", config->internal.contentdirfull, id, id + 8);
}
bool original_content_cache_job_allowed(const original_content_cache_job_t *job, original_cache_format_t format)
{ (void)job; assert(format == ORIGINAL_CACHE_TAF); return true; }
bool original_content_cache_job_current(const original_content_cache_job_t *job, original_cache_format_t format)
{ (void)job; assert(format == ORIGINAL_CACHE_TAF); return true; }
void fillBaseCtx(HttpConnection *connection, const char *uri, const char *query,
    cloudapi_t api, cbr_ctx_t *ctx, client_ctx_t *client)
{
    memset(ctx, 0, sizeof(*ctx)); ctx->connection = connection;
    ctx->uri = uri; ctx->queryString = query; ctx->api = api; ctx->client_ctx = client;
}
bool_t isValidTaf(const char *path, bool full)
{
    taf_original_identity_t identity;
    return taf_original_inspect(path, full, &identity) == NO_ERROR;
}

int_t cloud_request_get(const char *host, int port, const char *uri,
    const char *query, const uint8_t *auth, req_cbr_t *callbacks)
{
    (void)port; (void)auth;
    assert(!strcmp(host, "boxine.example.test") && !strcmp(query, ""));
    assert(!strcmp(uri, "/v2/content/8194F21C500304E0"));
    original_taf_response_t *ctx = callbacks->ctx;
    assert(ctx->base.tb1_identity != NULL && ctx->base.reject_redirects);
    requests++;
    HttpClientContext cloud = {0};
    cloud.statusCode = !strcmp(scenario, "status-error") ? 404 : 200;
    callbacks->response(callbacks->ctx, &cloud);
    size_t size = !strcmp(scenario, "partial") ? TONIEFILE_FRAME_SIZE + 1 : sizeof(taf_bytes);
    callbacks->body(callbacks->ctx, &cloud, (const char *)taf_bytes, 97, NO_ERROR);
    if (!strcmp(scenario, "transport-error"))
    {
        callbacks->body(callbacks->ctx, &cloud, NULL, 0, ERROR_TIMEOUT);
        callbacks->body(callbacks->ctx, &cloud, NULL, 0, ERROR_END_OF_STREAM);
        return NO_ERROR; /* The callback error must not be lost by a successful wrapper return. */
    }
    error_t end = !strcmp(scenario, "missing-eof") ? NO_ERROR : ERROR_END_OF_STREAM;
    callbacks->body(callbacks->ctx, &cloud, (const char *)taf_bytes + 97, size - 97, end);
    if (!strcmp(scenario, "repeated-eof"))
        callbacks->body(callbacks->ctx, &cloud, (const char *)taf_bytes, size, ERROR_END_OF_STREAM);
    return NO_ERROR;
}

static void make_taf(void)
{
    for (size_t i = TONIEFILE_FRAME_SIZE; i < sizeof(taf_bytes); i++)
        taf_bytes[i] = (uint8_t)i;
    uint8_t digest[SHA1_DIGEST_SIZE];
    assert(!sha1Compute(taf_bytes + TONIEFILE_FRAME_SIZE, TONIEFILE_FRAME_SIZE, digest));
    TonieboxAudioFileHeader header = TONIEBOX_AUDIO_FILE_HEADER__INIT;
    header.sha1_hash = (ProtobufCBinaryData){sizeof(digest), digest};
    header.audio_id = 123456;
    header.num_bytes = TONIEFILE_FRAME_SIZE;
    size_t length = toniebox_audio_file_header__pack(&header, taf_bytes + 4);
    taf_bytes[0] = (uint8_t)(length >> 24); taf_bytes[1] = (uint8_t)(length >> 16);
    taf_bytes[2] = (uint8_t)(length >> 8); taf_bytes[3] = (uint8_t)length;
}

static void check_query(void)
{
    char value[16];
    assert(original_api_query(NULL, "overlay", value, sizeof(value)) && !value[0]);
    assert(original_api_query("otheroverlay=bad&overlay=A%42C", "overlay", value, sizeof(value)) && !strcmp(value, "ABC"));
    const char *bad[] = {"overlay=x&overlay=x", "overlay=x&overlay=", "overlay=%00",
        "overlay=x%00tail", "overlay=%", "overlay=%4", "overlay=%GG", "overlay=x+y",
        "overlay=1234567890123456"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!original_api_query(bad[i], "overlay", value, sizeof(value)));
    assert(original_api_query("overlay=123456789012345", "overlay", value, sizeof(value)));
}

static void check_overlay(void)
{
    client_ctx_t ctx = {0};
    assert(original_api_overlay("overlay=D4594404DEAC", &ctx) && ctx.settings == &settings[1]);
    assert(original_api_overlay("overlay=D4594404DE%41C", &ctx) && ctx.settings == &settings[1]);
    const char *bad[] = {"overlay=unknown", "overlay=UNUSED", "overlay=D4594404DEAC%00bad",
        "overlay=D4594404DEAC&overlay=D4594404DEAC", "overlay=D4594404DEAC&overlay=unknown"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        ctx.settings = &settings[1];
        assert(!original_api_overlay(bad[i], &ctx) && ctx.settings == &settings[1]);
    }
    char truncated[ORIGINAL_CONTENT_CACHE_OWNER_SIZE + sizeof("overlay=") + 1];
    memset(truncated, 'A', sizeof(truncated));
    memcpy(truncated, "overlay=", 8); truncated[sizeof(truncated) - 1] = '\0';
    assert(!original_api_overlay(truncated, &ctx) && ctx.settings == &settings[1]);
    assert(original_api_overlay("", &ctx) && ctx.settings == &settings[0]);
    assert(original_api_overlay("overlay=", &ctx) && ctx.settings == &settings[0]);
}

static void check_uint(void)
{
    uint32_t value = 99;
    assert(original_api_uint("0", &value) && value == 0);
    assert(original_api_uint("4294967295", &value) && value == UINT32_MAX);
    const char *bad[] = {NULL, "", "-1", "+1", "1.0", "1tail", "4294967296", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!original_api_uint(bad[i], &value));
}

static void check_transfer(void)
{
    make_taf();
    assert(!fsCreateDir("content"));
    original_content_cache_job_t job = {0};
    job.settings = settings[0];
    strcpy(job.ruid, ruid); strcpy(job.upstream_ruid, ruid);
    write_failure = !strcmp(scenario, "write-error");
    flush_failure = !strcmp(scenario, "flush-error");
    const char *reason = NULL;
    error_t error = original_content_materialize_taf(&job, false, &reason);
    assert(requests == 1);
    bool success = !strcmp(scenario, "success") || !strcmp(scenario, "repeated-eof");
    if (success)
    {
        assert(!error && taf_original_file_complete(raw));
        assert(write_calls == 2 && flush_calls == 1 && close_calls == 1);
        uint8_t actual[sizeof(taf_bytes)];
        FILE *file = fopen(raw, "rb");
        assert(file && fread(actual, 1, sizeof(actual), file) == sizeof(actual));
        assert(fgetc(file) == EOF && !memcmp(actual, taf_bytes, sizeof(actual)));
        fclose(file);
    }
    else
    {
        assert(error != NO_ERROR && !fsFileExists(raw));
        if (!strcmp(scenario, "status-error"))
            assert(reason && !strcmp(reason, "taf_not_available") && close_calls == 0);
        else
            assert(close_calls == 1);
        if (write_failure || flush_failure) assert(error == ERROR_WRITE_FAILED);
        if (!strcmp(scenario, "transport-error")) assert(error == ERROR_TIMEOUT);
        if (!strcmp(scenario, "missing-eof")) assert(flush_calls == 0);
    }
    assert(!fsFileExists("content/8194F21C/500304E0.original-download.tmp"));
    assert(!fsFileExists("content/8194F21C/500304E0.json"));
}

int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    for (size_t i = 0; i < MUTEX_LAST; i++) assert(osCreateMutex(&locks[i]));
    settings[0].internal.contentdirfull = "content";
    settings[0].internal.cachedirfull = "cache";
    settings[0].internal.librarydirfull = "library";
    settings[0].cloud.remote_hostname = "boxine.example.test";
    settings[1].internal.config_used = true;
    if (!strcmp(scenario, "query")) check_query();
    else if (!strcmp(scenario, "overlay")) check_overlay();
    else if (!strcmp(scenario, "uint")) check_uint();
    else check_transfer();
    for (size_t i = 0; i < MUTEX_LAST; i++) osDeleteMutex(&locks[i]);
    printf("PASS original API/transfer boundary: %s\n", scenario);
    return 0;
}
