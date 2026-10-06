/* Production OTA module, real SHA-256/files/POSIX synchronization; HTTP is fake. */
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stdio.h>
#define TRACE_LEVEL 0
#include "fs_ext.h"
static atomic_bool fail_write;
static error_t test_write(FsFile *file, void *data, size_t size)
{
    if (atomic_load(&fail_write)) return ERROR_WRITE_FAILED;
    return fsWriteFile(file, data, size);
}
#define fsWriteFile test_write
#include "../src/tb2_ota_cache.c"
#undef fsWriteFile

static settings_t settings[MAX_OVERLAYS];
static OsMutex settings_mutex;
static HttpConnection connection;
static client_ctx_t client;
static const char *payloads[] = {"firmware for slot zero", "different firmware for slot one"};
static char manifest[4096], forwarded[OTA_MANIFEST_LIMIT + 1024];
static const char *response_body;
static size_t forwarded_length;
static unsigned response_status = 200;
static bool response_eof = true;
static atomic_uint requests[2], completed[2];
static atomic_int blocked_slot, bad_slot;
static atomic_bool corrupt, cut_short, bad_status;
static atomic_bool legacy_started, legacy_blocked;
static char certificate_key[] = "fixture-key";
static const char *expected_query[2] = {"auth=fixture-only", "auth=fixture-only"};
static unsigned delivered;

settings_t *get_settings(void) { return &settings[0]; }
settings_t *get_settings_id(uint8_t id) { return &settings[id]; }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_SETTINGS); osAcquireMutex(&settings_mutex); }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_SETTINGS); osReleaseMutex(&settings_mutex); }
const settings_cert_t *tb2_client_identity_resolve(settings_t *s, const char **source)
{ (void)source; return &s->internal.client_tb2; }

error_t fsCreateDirEx(const char *path, bool_t recursive)
{
    assert(recursive);
    char copy[PATH_LEN]; snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) if (*p == '/')
    { *p = 0; mkdir(copy, 0700); *p = '/'; }
    return mkdir(copy, 0700) == 0 || errno == EEXIST ? NO_ERROR : ERROR_FAILURE;
}
error_t fsFlushFile(FsFile *file) { return fflush(file) ? ERROR_WRITE_FAILED : NO_ERROR; }
void httpPrepareHeader(HttpConnection *c, const void *type, size_t size)
{ (void)type; (void)size; c->response.statusCode = 200; }
error_t httpWriteResponse(HttpConnection *c, void *data, size_t size, bool_t release)
{ (void)c; (void)data; (void)size; (void)release; return NO_ERROR; }
error_t httpSendResponseStreamUnsafe(HttpConnection *c, const char *uri, const char *path, bool_t stream)
{ (void)c; (void)uri; assert(!stream && fsFileExists(path)); delivered++; return NO_ERROR; }
static void forward_response(void *context, HttpClientContext *http)
{ assert(((cbr_ctx_t *)context)->connection == &connection); assert(http->statusCode == response_status); }
static void forward_header(void *context, HttpClientContext *http, const char *key, const char *value)
{ (void)context; (void)http; (void)key; (void)value; }
static void forward_body(void *context, HttpClientContext *http, const char *data, size_t len, error_t error)
{
    (void)http; (void)error; assert(((cbr_ctx_t *)context)->connection == &connection);
    assert(forwarded_length + len < sizeof(forwarded));
    memcpy(forwarded + forwarded_length, data, len); forwarded_length += len;
}
req_cbr_t getCloudCbr(HttpConnection *c, const char *uri, const char *query,
                     cloudapi_t api, cbr_ctx_t *ctx, client_ctx_t *cl)
{
    memset(ctx, 0, sizeof(*ctx)); ctx->connection = c; ctx->client_ctx = cl;
    ctx->uri = uri; ctx->queryString = query; ctx->api = api;
    return (req_cbr_t){.ctx = ctx, .response = forward_response,
        .header = forward_header, .body = forward_body};
}
int_t cloud_request_tb2_post(const char *host, int port, const char *uri, const char *query,
    const uint8_t *body, size_t length, const uint8_t *hash, req_cbr_t *cb)
{
    (void)port; (void)query; (void)body; (void)length; (void)hash;
    assert(!strcmp(host, "tbs2.example.test") && !strcmp(uri, "/v3/check-ota"));
    HttpClientContext http = {0}; http.statusCode = response_status;
    http.bodyLen = strlen(response_body);
    cb->response(cb->ctx, &http); cb->header(cb->ctx, &http, NULL, NULL);
    for (size_t pos = 0; pos < http.bodyLen;)
    {
        size_t n = MIN(13U, http.bodyLen - pos);
        cb->body(cb->ctx, &http, response_body + pos, n, NO_ERROR); pos += n;
    }
    if (response_eof) cb->body(cb->ctx, &http, "", 0, ERROR_END_OF_STREAM);
    return NO_ERROR;
}
int_t cloud_request_tb2_get(const char *host, int port, const char *uri, const char *query,
                           const uint8_t *hash, req_cbr_t *cb)
{
    (void)hash;
    assert(!strcmp(host, "tbs2.example.test") && port == 443);
    unsigned slot = strstr(uri, "resource-zero") ? 0 : 1;
    assert(strstr(uri, "resource-zero") || strstr(uri, "resource-one"));
    assert(!strcmp(query, expected_query[slot]));
    cbr_ctx_t *ctx = cb->ctx;
    assert(ctx->reject_redirects && !strcmp(ctx->tb2_identity->key, "fixture-key"));
    assert(!strcmp(ctx->user_agent, "TB2/fixture"));
    atomic_fetch_add(&requests[slot], 1);
    while (atomic_load(&blocked_slot) == (int)slot) osDelayTask(1);
    assert(!strcmp(ctx->tb2_identity->key, "fixture-key"));
    HttpClientContext http = {0}; http.statusCode = atomic_load(&bad_status) ? 503 : 200;
    cb->response(cb->ctx, &http);
    if (cb->header) cb->header(cb->ctx, &http, "Content-Disposition", "attachment;filename=server-name.bin");
    const char *body = payloads[slot];
    if (atomic_load(&corrupt)) body = "corrupted payload";
    cb->body(cb->ctx, &http, body, strlen(body), NO_ERROR);
    if (atomic_load(&bad_slot) == (int)slot)
    { cb->body(cb->ctx, &http, "", 0, ERROR_TIMEOUT); return ERROR_TIMEOUT; }
    if (!atomic_load(&cut_short)) cb->body(cb->ctx, &http, "", 0, ERROR_END_OF_STREAM);
    atomic_fetch_add(&completed[slot], 1);
    return NO_ERROR;
}

static void hash_string(const char *body, char result[65])
{
    uint8_t digest[32]; sha256Compute(body, strlen(body), digest);
    for (unsigned i = 0; i < 32; i++) snprintf(result + i * 2, 3, "%02x", digest[i]);
}
static void check(const char *body)
{
    forwarded_length = 0; response_body = body;
    assert(!tb2_ota_cache_check(&connection, "/v3/check-ota", "", &client, NULL, 0));
    assert(forwarded_length == strlen(body) && !memcmp(body, forwarded, forwarded_length));
}
static bool request(unsigned slot)
{
    bool handled = false;
    const char *uri = slot ? "/v3/ota/7/resource-one" : "/v3/ota/7/resource-zero";
    assert(!tb2_ota_cache_request(&connection, uri, "auth=fixture-only", &client, &handled));
    return handled;
}
static void wait_count(atomic_uint *value, unsigned expected)
{
    systime_t start = osGetSystemTime();
    while (atomic_load(value) < expected)
    { assert(osGetSystemTime() - start < 3000); osDelayTask(1); }
}
static void wait_idle(void)
{
    systime_t start = osGetSystemTime();
    for (;;)
    {
        osAcquireMutex(&ota.mutex);
        bool busy = ota.working;
        for (unsigned i = 0; i < MAX_OVERLAYS; i++) busy |= ota.pending[i] != NULL;
        for (unsigned i = 0; i < OTA_ACTIVE_MAX; i++) busy |= ota.active[i].users != 0;
        osReleaseMutex(&ota.mutex);
        if (!busy) return;
        assert(osGetSystemTime() - start < 3000); osDelayTask(1);
    }
}
static void *request_one(void *unused) { (void)unused; assert(request(1)); return NULL; }
static error_t legacy(HttpConnection *c, const char *uri, const char *query, client_ctx_t *cl)
{
    (void)c; (void)query; (void)cl;
    assert(!strcmp(uri, "/v3/ota/7/resource-one"));
    atomic_store(&legacy_started, true);
    while (atomic_load(&legacy_blocked)) osDelayTask(1);
    assert(!fsCreateDirEx("firmware/ota/tb2/7", true));
    FsFile *file = fsOpenFile("firmware/ota/tb2/7/resource-one.bin", FS_FILE_MODE_WRITE);
    assert(file && !fsWriteFile(file, (void *)payloads[1], strlen(payloads[1]))); fsCloseFile(file);
    return NO_ERROR;
}
static void *legacy_request(void *unused)
{
    (void)unused;
    assert(!tb2_ota_cache_serve(&connection, "/v3/ota/7/resource-one", "auth=fixture-only", &client, legacy));
    return NULL;
}
static void *release_download(void *unused)
{ (void)unused; osDelayTask(30); atomic_store(&blocked_slot, -1); return NULL; }
static void configure(void)
{
    assert(osCreateMutex(&settings_mutex));
    settings[0].cloud.cacheOtaV3BothSlots = true;
    settings[1].toniebox.boxGeneration = GENERATION_TB2;
    settings[1].cloud.tb2_v3_enabled = settings[1].cloud.enableV3Ota = settings[1].cloud.cacheOta = true;
    settings[1].cloud.remote_hostname_tb2 = "tbs2.example.test";
    settings[1].cloud.remote_port_tb2 = 443;
    settings[1].core.http_client_timeout = 1000;
    settings[1].internal.firmwaredirfull = "firmware";
    settings[1].internal.overlayNumber = 1;
    settings[1].internal.overlayUniqueId = "synthetic-box";
    settings[1].internal.client_tb2 = (settings_cert_t){.ca="fixture-ca", .crt="fixture-cert", .key=certificate_key};
    client.settings = &settings[1]; strcpy(connection.request.userAgent, "TB2/fixture");
    atomic_store(&blocked_slot, -1); atomic_store(&bad_slot, -1);
    char zero[65], one[65]; hash_string(payloads[0], zero); hash_string(payloads[1], one);
    snprintf(manifest, sizeof(manifest),
        "{\"FirmwareSlot0\":{\"url\":\"https://tbs2.example.test/v3/ota/7/resource-zero?auth=fixture-only\","
        "\"sha2_256\":\"%s\",\"version_string\":\"v1.2.3\"},"
        "\"FirmwareSlot1\":{\"url\":\"/v3/ota/7/resource-one?auth=fixture-only\","
        "\"sha2_256\":\"%s\",\"version_string\":\"v1.2.3\"}}", zero, one);
    tb2_ota_cache_init(); assert(ota.ready);
}
int main(int argc, char **argv)
{
    assert(argc == 2); configure(); const char *test = argv[1];
    if (!strcmp(test, "disabled"))
    {
        /* Even a stale/manual box override cannot enable this global switch. */
        settings[1].cloud.cacheOtaV3BothSlots = true;
        settings[0].cloud.cacheOtaV3BothSlots = false; tb2_ota_cache_set_enabled(false);
        check(manifest); assert(!request(0)); assert(!requests[0] && !requests[1]);
    }
    else if (!strcmp(test, "invalid-offers"))
    {
        check(manifest); assert(ota.offers[1].valid);
        tb2_ota_cache_set_enabled(true); assert(ota.offers[1].valid);
        check(""); assert(!request(0));
        check("{}"); assert(!request(0));
        check("{broken"); assert(!request(0));
        response_eof = false; check(manifest); assert(!request(0)); response_eof = true;
        response_status = 503; check(manifest); assert(!request(0)); response_status = 200;
        char large[OTA_MANIFEST_LIMIT + 2]; memset(large, ' ', sizeof(large)); large[sizeof(large)-1] = 0;
        check(large); assert(!request(0));
        ota_offer_t offer = {0};
        const char *duplicate = "{\"FirmwareSlot0\":{},\"FirmwareSlot0\":{}}";
        assert(!ota_parse(duplicate, strlen(duplicate), "tbs2.example.test", 443, &offer));
        ota_image_t image = {0};
        assert(!ota_url("https://evil.example/v3/ota/7/resource-one", "tbs2.example.test", 443, &image));
        assert(!ota_url("/v3/ota/7/../escape", "tbs2.example.test", 443, &image));
        assert(!ota_url("/v3/ota/7/a%2fb", "tbs2.example.test", 443, &image));
        cJSON *root = cJSON_Parse(manifest);
        cJSON *slot1 = cJSON_GetObjectItemCaseSensitive(root, "FirmwareSlot1");
        cJSON_ReplaceItemInObjectCaseSensitive(slot1, "url", cJSON_CreateString("/v3/ota/7/resource-zero"));
        char *ambiguous = cJSON_PrintUnformatted(root); check(ambiguous); assert(!request(0));
        free(ambiguous); cJSON_Delete(root);
    }
    else if (!strcmp(test, "failures"))
    {
        check(manifest);
        atomic_store(&corrupt, true); assert(request(0));
        assert(!fsFileExists("firmware/ota/tb2/7/resource-zero.bin"));
        atomic_store(&corrupt, false); atomic_store(&cut_short, true); assert(request(0));
        atomic_store(&cut_short, false); atomic_store(&bad_status, true); assert(request(0));
        atomic_store(&bad_status, false); atomic_store(&bad_slot, 0); assert(request(0));
        atomic_store(&bad_slot, -1); atomic_store(&fail_write, true); assert(request(0));
        assert(!fsFileExists("firmware/ota/tb2/7/resource-zero.bin"));
        assert(!fsFileExists("firmware/ota/tb2/7/resource-zero.bin.tmp"));
        assert(!requests[1]); atomic_store(&fail_write, false);
        assert(request(0)); wait_count(&completed[1], 1); wait_idle();
    }
    else if (!strcmp(test, "concurrent") || !strcmp(test, "concurrent-failure"))
    {
        bool fail = !strcmp(test, "concurrent-failure");
        if (fail) atomic_store(&bad_slot, 1);
        check(manifest); atomic_store(&blocked_slot, 1);
        assert(request(0)); wait_count(&requests[1], 1);
        pthread_t second; assert(!pthread_create(&second, NULL, request_one, NULL));
        osDelayTask(30); assert(requests[1] == 1);
        atomic_store(&blocked_slot, -1); pthread_join(second, NULL); wait_idle();
        assert(requests[0] == 1 && requests[1] == 1);
        if (fail) assert(!fsFileExists("firmware/ota/tb2/7/resource-one.bin"));
    }
    else if (!strcmp(test, "legacy-first"))
    {
        atomic_store(&legacy_blocked, true);
        pthread_t first; assert(!pthread_create(&first, NULL, legacy_request, NULL));
        while (!atomic_load(&legacy_started)) osDelayTask(1);
        check(manifest); assert(request(0));
        osDelayTask(30); assert(!requests[1]);
        atomic_store(&legacy_blocked, false); pthread_join(first, NULL); wait_idle();
        assert(requests[0] == 1 && !requests[1]);
        assert(request(1)); wait_idle(); assert(!requests[1]);
    }
    else if (!strcmp(test, "policy"))
    {
        settings[1].toniebox.boxGeneration = GENERATION_TB1;
        check(manifest); assert(!request(0));
        settings[1].toniebox.boxGeneration = GENERATION_TB2;
        bool *gates[] = {&settings[1].cloud.tb2_v3_enabled, &settings[1].cloud.enableV3Ota,
                         &settings[1].cloud.cacheOta};
        for (size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
        {
            *gates[i] = false; check(manifest); assert(!request(0)); *gates[i] = true;
        }
        assert(!requests[0] && !requests[1]);
        check(manifest); assert(request(0)); wait_idle();
        settings[1].cloud.tb2_v3_enabled = false;
        settings[1].cloud.localOta = true;
        assert(!request(0));
        assert(!tb2_ota_cache_serve(&connection, "/v3/ota/7/resource-zero", "auth=fixture-only", &client, legacy));
        assert(delivered == 1 && requests[0] == 1 && requests[1] == 1);
    }
    else if (!strcmp(test, "both-present"))
    {
        check(manifest); assert(!fsCreateDirEx("firmware/ota/tb2/7", true));
        const char *paths[] = {"firmware/ota/tb2/7/resource-zero.bin", "firmware/ota/tb2/7/resource-one.bin"};
        for (unsigned i = 0; i < 2; i++)
        {
            FsFile *file = fsOpenFile(paths[i], FS_FILE_MODE_WRITE); assert(file);
            assert(!fsWriteFile(file, (void *)payloads[i], strlen(payloads[i]))); fsCloseFile(file);
        }
        assert(request(0)); wait_idle(); assert(request(1)); wait_idle();
        assert(!requests[0] && !requests[1]);
    }
    else if (!strcmp(test, "counterpart-failure"))
    {
        check(manifest); atomic_store(&bad_slot, 1);
        expected_query[0] = "box-token=new&keep=%2F+value";
        bool handled;
        assert(!tb2_ota_cache_request(&connection, "/v3/ota/7/resource-zero", expected_query[0], &client, &handled) && handled);
        wait_idle();
        assert(requests[0] == 1 && requests[1] == 1);
        assert(!fsFileExists("firmware/ota/tb2/7/resource-one.bin"));
        assert(!fsFileExists("firmware/ota/tb2/7/resource-one.bin.tmp"));
        atomic_store(&bad_slot, -1); assert(request(0)); wait_idle();
        assert(requests[0] == 1 && requests[1] == 2);
        assert(fsFileExists("firmware/ota/tb2/7/resource-one.bin"));
        assert(!fsFileExists("firmware/ota/tb2/7/server-name.bin"));
    }
    else if (!strcmp(test, "disable-and-snapshot"))
    {
        check(manifest); atomic_store(&blocked_slot, 1);
        assert(request(0)); wait_count(&requests[1], 1);
        assert(request(0));
        osAcquireMutex(&ota.mutex); assert(ota.pending[1]); osReleaseMutex(&ota.mutex);
        settings[0].cloud.cacheOtaV3BothSlots = false; tb2_ota_cache_set_enabled(false);
        strcpy(certificate_key, "changed-key");
        osAcquireMutex(&ota.mutex); assert(!ota.pending[1]); osReleaseMutex(&ota.mutex);
        assert(!ota.offers[1].valid); atomic_store(&blocked_slot, -1);
        wait_count(&completed[1], 1); wait_idle(); assert(!request(0));
    }
    else if (!strcmp(test, "shutdown"))
    {
        check(manifest); atomic_store(&blocked_slot, 1);
        assert(request(0)); wait_count(&requests[1], 1);
        pthread_t release; assert(!pthread_create(&release, NULL, release_download, NULL));
        tb2_ota_cache_deinit(); pthread_join(release, NULL);
        assert(completed[1] == 1 && !ota.working);
        assert(fsFileExists("firmware/ota/tb2/7/resource-one.bin"));
        printf("PASS %s\n", test); return 0;
    }
    else
    {
        unsigned first = !strcmp(test, "slot-one");
        assert(first || !strcmp(test, "slot-zero")); check(manifest);
        settings[1].cloud.localOta = first;
        assert(request(first)); wait_count(&completed[1-first], 1); wait_idle();
        assert(requests[0] == 1 && requests[1] == 1);
        assert(delivered == first);
        assert(fsFileExists("firmware/ota/tb2/7/resource-zero.bin"));
        assert(fsFileExists("firmware/ota/tb2/7/resource-one.bin"));
        assert(request(first)); wait_idle(); assert(requests[0] == 1 && requests[1] == 1);
        bool handled = false;
        assert(!tb2_ota_cache_request(&connection, "/v3/ota/8/mapping", "", &client, &handled) && !handled);
    }
    tb2_ota_cache_deinit(); printf("PASS %s\n", test); return 0;
}
