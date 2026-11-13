/* Real queue/policy/worker with real files and synchronization; transport only
 * is replaced. Run inside a disposable directory via the Python launcher. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRACE_LEVEL 0
#include "../src/original_content_cache.c"
#include "../src/original_content_api.c"

static settings_t settings[MAX_OVERLAYS];
static OsMutex locks[MUTEX_LAST];
static atomic_uint calls;
static unsigned order[64];
static bool library_flags[64];
static uint8_t owners[64];
static bool refresh_flags[64];
static atomic_bool adapter_wait;
static error_t adapter_error[ORIGINAL_CACHE_FORMAT_COUNT];
static bool local_complete[ORIGINAL_CACHE_FORMAT_COUNT];
static const char *test_ruid = "8194F21C500304E0";
static const char *auth_hex = "0102030405060708010203040506070801020304050607080102030405060708";
static const char *cache_preference = "auto";
static const char *refresh_result = "unchanged";
static cJSON *http_result;

settings_t *get_settings(void) { return &settings[0]; }
settings_t *get_settings_id(uint8_t index) { assert(index < MAX_OVERLAYS); return &settings[index]; }
uint8_t get_overlay_id(const char *owner)
{
    for (unsigned i = 1; i < MAX_OVERLAYS; i++)
        if (same_text(settings[i].internal.overlayUniqueId, owner)) return (uint8_t)i;
    return 0;
}
void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{ (void)connection; (void)type; (void)length; }
error_t httpWriteResponse(HttpConnection *connection, void *data, size_t size, bool_t release)
{
    (void)connection;
    cJSON_Delete(http_result);
    http_result = cJSON_ParseWithLength(data, size);
    assert(http_result);
    if (release) free(data);
    return NO_ERROR;
}
void mutex_lock(mutex_id_t id) { osAcquireMutex(&locks[id]); }
void mutex_unlock(mutex_id_t id) { osReleaseMutex(&locks[id]); }
const settings_cert_t *tb2_client_identity_resolve(settings_t *value, const char **source)
{ (void)source; return complete_identity(&value->internal.client_tb2) ? &value->internal.client_tb2 : NULL; }
void getContentPathFromCharRUID(char ruid[17], char **path, settings_t *value)
{
    size_t size = strlen(value->internal.contentdirfull) + strlen(ruid) + 2;
    *path = malloc(size);
    assert(*path);
    snprintf(*path, size, "%s/%s", value->internal.contentdirfull, ruid);
}
bool_t v3_native_cache_active_is_tonieplay(const char *cache, const char *library,
    uint8_t overlay, const char *ruid)
{ (void)cache; (void)library; (void)overlay; (void)ruid; return FALSE; }
bool_t v3_native_cache_active_info(const char *cache, const char *library,
    uint8_t overlay, const char *ruid, uint32_t *version, size_t *count, bool_t *tonieplay)
{
    (void)cache; (void)library; (void)overlay; (void)ruid;
    (void)version; (void)count; (void)tonieplay;
    return local_complete[ORIGINAL_CACHE_V3];
}
char *taf_original_cache_resolve(const char *raw, const settings_t *value)
{ (void)raw; (void)value; return local_complete[ORIGINAL_CACHE_TAF] ? strdup("cached.taf") : NULL; }
bool taf_original_file_complete(const char *path)
{ (void)path; return local_complete[ORIGINAL_CACHE_TAF]; }

error_t original_content_cache_materialize(original_content_cache_job_t *job,
    original_cache_format_t format, bool library, const char **reason)
{
    assert(original_content_cache_job_current(job, format));
    if (!local_complete[format]) assert(original_content_cache_job_allowed(job, format));
    unsigned index = atomic_fetch_add(&calls, 1);
    assert(index < 64);
    order[index] = format;
    library_flags[index] = library;
    owners[index] = job->settings.internal.overlayNumber;
    refresh_flags[index] = job->refresh_v3;
    assert(job->settings.cloud.remote_hostname != settings[1].cloud.remote_hostname);
    assert(!strcmp(job->settings.cloud.remote_hostname, "boxine.test"));
    assert(job->tb1_identity.key != settings[1].internal.client_tb1.key);
    assert(!strcmp(job->tb1_identity.key, "key"));
    assert(job->auth[0] == 1 && job->auth[31] == 8);
    if (job->refresh_v3)
    {
        assert(format == ORIGINAL_CACHE_V3 && !job->both_formats && !job->automatic);
        const settings_cert_t *identity = &settings[owners[index]].internal.client_tb2;
        assert(job->tb2_identity.key != identity->key);
        assert(!strcmp(job->tb2_identity.key, identity->key));
    }
    while (atomic_load(&adapter_wait)) osDelayTask(1);
    *reason = adapter_error[format] ? "transport_failure" : job->refresh_v3 ? refresh_result : NULL;
    return adapter_error[format];
}

static void write_policy(const char *ruid, const char *source, bool nocloud, const char *auth)
{
    char path[100]; snprintf(path, sizeof(path), "%s.json", ruid);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fprintf(file, "{\"source\":\"%s\",\"nocloud\":%s,\"cloud_ruid\":\"%s\","
        "\"cloud_auth\":\"%s\",\"cloud_override\":false,\"cache_preference\":\"%s\"}", source,
        nocloud ? "true" : "false", ruid, auth, cache_preference) > 0);
    assert(fclose(file) == 0);
}

static void setup(void)
{
    memset(&original_cache, 0, sizeof(original_cache));
    memset(settings, 0, sizeof(settings));
    for (unsigned i = 0; i < MUTEX_LAST; i++) assert(osCreateMutex(&locks[i]));
    settings[0].cloud.cacheBothFormatsOnDownload = true;
    settings[0].cloud.cacheBothFormatsOnPlayback = true;
    settings[1].internal = (settings_internal_t){.config_used = true, .overlayNumber = 1,
        .overlayUniqueId = "box-a", .contentdirfull = ".", .cachedirfull = "cache", .librarydirfull = "library"};
    settings[1].cloud = (settings_cloud_t){.enabled = true, .enableV2Content = true,
        .cacheContent = true, .cacheToLibrary = true, .tb2_v3_enabled = true,
        .enableV3ContentMeta = true, .enableV3Chapter = true, .cacheContentV3 = true,
        .cacheToLibraryV3 = true, .remote_hostname = "boxine.test", .remote_hostname_tb2 = "tonies.test"};
    settings[1].internal.client_tb1 = (settings_cert_t){.ca = "ca", .crt = "crt", .key = "key"};
    settings[1].internal.client_tb2 = settings[1].internal.client_tb1;
    settings[0].internal.client_tb1 = settings[1].internal.client_tb1;
    settings[1].toniebox.boxGeneration = GENERATION_TB1;
    settings[2].internal.config_used = true;
    settings[2].toniebox.boxGeneration = GENERATION_TB2;
    write_policy(test_ruid, "", false, auth_hex);
}

static unsigned pending_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
        count += original_cache.records[i].job != NULL;
    return count;
}

static void queue_tests(void)
{
    assert(osCreateEvent(&original_cache.wake)); original_cache.ready = true;
    const char *reason = NULL;
    for (unsigned i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
    {
        char ruid[17]; snprintf(ruid, sizeof(ruid), "8194F21C%08X", i);
        write_policy(ruid, "", false, auth_hex);
        assert(original_content_cache_submit(&settings[1], ruid, ORIGINAL_CACHE_TAF, false, 1, &reason) == NO_ERROR);
        assert(original_cache.records[i].sequence == i + 1U);
    }
    assert(pending_count() == ORIGINAL_CONTENT_CACHE_CAPACITY);
    assert(original_content_cache_submit(&settings[1], "8194F21C00000000", ORIGINAL_CACHE_TAF, false, 1, &reason) == NO_ERROR);
    assert(!strcmp(reason, "already_queued"));
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_TAF, false, 1, &reason) == ERROR_OUT_OF_RESOURCES);
    assert(!strcmp(reason, "queue_full"));
    original_record_t *record = &original_cache.records[0];
    free_job(record->job); record->job = NULL;
    assert(original_content_cache_submit(&settings[1], "8194F21C00000000", ORIGINAL_CACHE_TAF, true, 1, &reason) == NO_ERROR);
    assert(!strcmp(reason, "already_attempted") && !record->job);
    assert(original_content_cache_submit(&settings[1], "8194F21C00000000", ORIGINAL_CACHE_TAF, true, 2, &reason) == NO_ERROR);
    assert(record->job && record->observed_version == 2);
    free_job(record->job); record->job = NULL;
    assert(original_content_cache_submit(&settings[1], "8194F21C00000000", ORIGINAL_CACHE_TAF, false, 2, &reason) == NO_ERROR);
    assert(record->job); /* Manual retry is explicit even for the same version. */
    for (unsigned i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++) free_job(original_cache.records[i].job);
    puts("PASS queue: capacity, FIFO sequence, dedup, automatic terminal suppression, explicit retry");
}

static void policy_tests(void)
{
    assert(original_content_cache_playback_enabled());
    settings[2].internal.config_used = false;
    assert(!original_content_cache_playback_enabled());
    settings[2].internal.config_used = true;
    settings[0].cloud.cacheBothFormatsOnPlayback = false;
    assert(!original_content_cache_playback_enabled());
    settings[0].cloud.cacheBothFormatsOnPlayback = true;
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_TAF);
    settings[1].toniebox.boxGeneration = GENERATION_TB2;
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_V3);
    cache_preference = "taf"; write_policy(test_ruid, "", false, auth_hex);
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_TAF);
    settings[1].toniebox.boxGeneration = GENERATION_TB1;
    cache_preference = "v3"; write_policy(test_ruid, "", false, auth_hex);
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_V3);
    cache_preference = "invalid"; write_policy(test_ruid, "", false, auth_hex);
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_TAF);
    char broken_path[100]; snprintf(broken_path, sizeof(broken_path), "%s.json", test_ruid);
    FILE *broken = fopen(broken_path, "wb"); assert(broken);
    assert(fwrite("{broken", 1, 7, broken) == 7); assert(fclose(broken) == 0);
    assert(original_content_cache_preferred(&settings[1], test_ruid) == ORIGINAL_CACHE_TAF);
    char unchanged[8] = {0}; broken = fopen(broken_path, "rb"); assert(broken);
    assert(fread(unchanged, 1, sizeof(unchanged), broken) == 7); assert(fclose(broken) == 0);
    assert(!strcmp(unchanged, "{broken"));
    cache_preference = "auto"; write_policy(test_ruid, "", false, auth_hex);
    assert(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF) == NULL);
    assert(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_V3) == NULL);
    write_policy(test_ruid, "lib://private.taf", false, auth_hex);
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF), "private_source"));
    write_policy(test_ruid, "", true, auth_hex);
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF), "nocloud"));
    write_policy(test_ruid, "", false, "zz");
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF), "missing_content_auth"));
    write_policy(test_ruid, "", false, auth_hex);
    settings[1].cloud.cacheContent = false;
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF), "taf_cache_disabled"));
    settings[1].cloud.cacheContent = true;
    settings[1].cloud.tb2_enabled = true;
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_V3), "tonies_forwarding_disabled"));
    settings[1].cloud.tb2_enabled = false;
    original_policy_t policy;
    assert(read_policy(&settings[1], test_ruid, &policy) == NULL);
    original_content_cache_job_t *job = copy_job(&settings[1], test_ruid, &policy);
    assert(job); job->preferred = ORIGINAL_CACHE_TAF;
    assert(original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    assert(original_content_cache_job_blocked_reason(job, ORIGINAL_CACHE_TAF) == NULL);
    char *owned_key = job->tb1_identity.key;
    job->tb1_identity.key = NULL;
    assert(!strcmp(original_content_cache_job_blocked_reason(job, ORIGINAL_CACHE_TAF), "missing_tb1_certificates"));
    job->tb1_identity.key = owned_key;
    job->settings.cloud.enabled = false;
    assert(!strcmp(original_content_cache_job_blocked_reason(job, ORIGINAL_CACHE_TAF), "boxine_forwarding_disabled"));
    job->settings.cloud.enabled = true;
    settings[1].internal.overlayUniqueId = "reused-slot";
    assert(!original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    settings[1].internal.overlayUniqueId = "box-a";
    write_policy(test_ruid, "lib://new-assignment.taf", false, auth_hex);
    assert(!original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    write_policy(test_ruid, "", false, auth_hex);
    settings[1].internal.client_tb1.key = "new-key";
    assert(!original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    assert(original_content_cache_job_current(job, ORIGINAL_CACHE_TAF));
    settings[1].internal.client_tb1.key = "key";
    assert(original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    settings[1].cloud.enabled = false;
    local_complete[ORIGINAL_CACHE_TAF] = true;
    assert(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF) == NULL);
    assert(original_content_cache_job_current(job, ORIGINAL_CACHE_TAF));
    assert(!original_content_cache_job_allowed(job, ORIGINAL_CACHE_TAF));
    local_complete[ORIGINAL_CACHE_TAF] = false;
    assert(!strcmp(original_content_cache_blocked_reason(&settings[1], test_ruid, ORIGINAL_CACHE_TAF), "boxine_forwarding_disabled"));
    free_job(job);
    puts("PASS policy: private/source/noCloud/auth/cache/forwarding, immutable owned identity, publication rechecks");
}

static void wait_idle(void)
{
    for (unsigned spin = 0; spin < 3000; spin++)
    {
        mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
        unsigned pending = pending_count();
        mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
        if (!pending) return;
        osDelayTask(1);
    }
    assert(!"worker did not become idle");
}

static void worker_tests(void)
{
    adapter_error[ORIGINAL_CACHE_V3] = ERROR_TIMEOUT;
    settings[1].cloud.cacheToLibrary = false;
    original_content_cache_init();
    assert(original_cache.ready);
    const char *reason = NULL;
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_V3, false, 19, &reason) == NO_ERROR);
    wait_idle();
    assert(atomic_load(&calls) == 2 && order[0] == ORIGINAL_CACHE_V3 && order[1] == ORIGINAL_CACHE_TAF);
    assert(library_flags[0] && !library_flags[1]);
    cJSON *state = cJSON_CreateObject(); original_content_cache_add_status(state, &settings[1], test_ruid);
    const cJSON *download = cJSON_GetObjectItemCaseSensitive(state, "download");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(download, "v3"), "state")), "failed"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(download, "taf"), "state")), "complete"));
    cJSON_Delete(state);
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_V3, true, 19, &reason) == NO_ERROR);
    assert(!strcmp(reason, "already_attempted"));
    assert(atomic_load(&calls) == 2);
    settings[2].internal.config_used = false;
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_V3, true, 20, &reason) == ERROR_ABORTED);
    assert(!strcmp(reason, "mixed_generations_required"));
    settings[2].internal.config_used = true;
    settings[0].cloud.cacheBothFormatsOnDownload = false;
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_TAF, false, 20, &reason) == NO_ERROR);
    wait_idle(); assert(atomic_load(&calls) == 3);
    local_complete[ORIGINAL_CACHE_TAF] = true;
    settings[1].cloud.enabled = false;
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_TAF, false, 21, &reason) == NO_ERROR);
    wait_idle(); assert(atomic_load(&calls) == 4);
    /* A known disc must not try Boxine before its first V3 manifest exists. */
    const char *disc_ruid = "8194F21C500104E0";
    write_policy(disc_ruid, "", false, auth_hex);
    settings[1].cloud.enabled = true;
    settings[0].cloud.cacheBothFormatsOnDownload = true;
    local_complete[ORIGINAL_CACHE_TAF] = false;
    adapter_error[ORIGINAL_CACHE_V3] = NO_ERROR;
    assert(original_content_cache_submit(&settings[1], disc_ruid, ORIGINAL_CACHE_TAF, false, 1, &reason) == NO_ERROR);
    wait_idle(); assert(atomic_load(&calls) == 5 && order[4] == ORIGINAL_CACHE_V3);
    state = cJSON_CreateObject(); original_content_cache_add_status(state, &settings[1], disc_ruid);
    download = cJSON_GetObjectItemCaseSensitive(state, "download");
    const cJSON *taf = cJSON_GetObjectItemCaseSensitive(download, "taf");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(taf, "state")), "skipped"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(taf, "reason")), "tonieplay_has_no_taf"));
    cJSON_Delete(state);
    original_content_cache_deinit();
    assert(original_content_cache_submit(&settings[1], test_ruid, ORIGINAL_CACHE_TAF, false, 21, &reason) == ERROR_OUT_OF_RESOURCES);
    puts("PASS worker: preferred first, serial formats, partial success, per-format library flags, no automatic retry, shutdown");
}

static void configure_refresh_group(void)
{
    for (unsigned i = 2; i <= 6; i++)
    {
        settings[i] = settings[1];
        settings[i].internal.overlayNumber = (uint8_t)i;
        settings[i].toniebox.boxGeneration = GENERATION_TB2;
    }
    settings[2].internal.overlayUniqueId = "box-b";
    settings[2].boxName = "Blue";
    settings[2].internal.client_tb2.key = "blue-key";
    settings[3].internal.overlayUniqueId = "box-c";
    settings[3].boxName = "Green";
    settings[3].internal.client_tb2.key = "green-key";
    settings[4].internal.overlayUniqueId = "tb1";
    settings[4].toniebox.boxGeneration = GENERATION_TB1;
    settings[5].internal.overlayUniqueId = "other-group";
    settings[5].internal.contentdirfull = "other-content";
    settings[6].internal.overlayUniqueId = "unused";
    settings[6].internal.config_used = false;
}

static void discard_queued_jobs(void)
{
    for (unsigned i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
        free_job(original_cache.records[i].job);
    memset(original_cache.records, 0, sizeof(original_cache.records));
}

static const cJSON *target_at(const cJSON *targets, int index, const char *owner, bool accepted)
{
    const cJSON *target = cJSON_GetArrayItem(targets, index);
    assert(target);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(target, "overlay")), owner));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(target, "accepted")) == accepted);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(target, "state")),
                   accepted ? "queued" : "rejected"));
    return target;
}

static void assert_reason(const cJSON *item, const char *reason)
{
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "reason")), reason));
}

static void refresh_group_tests(void)
{
    configure_refresh_group();
    assert(osCreateEvent(&original_cache.wake)); original_cache.ready = true;
    cJSON *targets = cJSON_CreateArray();
    assert(!original_content_cache_refresh_group(&settings[1], test_ruid, targets));
    assert(cJSON_GetArraySize(targets) == 2 && pending_count() == 2);
    target_at(targets, 0, "box-b", true); target_at(targets, 1, "box-c", true);
    for (unsigned i = 0; i < 2; i++)
    {
        const original_content_cache_job_t *job = original_cache.records[i].job;
        assert(job->refresh_v3 && job->preferred == ORIGINAL_CACHE_V3 && !job->both_formats);
        assert(job->settings.internal.overlayNumber == i + 2);
        assert(!strcmp(job->tb2_identity.key, i ? "green-key" : "blue-key"));
    }
    cJSON_Delete(targets);
    targets = cJSON_CreateArray();
    assert(!original_content_cache_refresh_group(&settings[1], test_ruid, targets));
    assert(pending_count() == 2);
    assert_reason(target_at(targets, 0, "box-b", true), "already_queued");
    cJSON_Delete(targets);
    const char *reason = NULL;
    /* A V3-only refresh cannot satisfy a concurrent normal TAF/both-formats job. */
    assert(original_content_cache_submit(&settings[2], test_ruid, ORIGINAL_CACHE_TAF,
        false, 0, &reason) == ERROR_IN_PROGRESS);
    assert(!strcmp(reason, "download_busy") && pending_count() == 2);
    discard_queued_jobs();

    assert(!original_content_cache_submit(&settings[2], test_ruid, ORIGINAL_CACHE_TAF, false, 0, &reason));
    targets = cJSON_CreateArray();
    assert(!original_content_cache_refresh_group(&settings[1], test_ruid, targets));
    assert_reason(target_at(targets, 0, "box-b", false), "download_busy");
    target_at(targets, 1, "box-c", true);
    assert(!original_cache.records[0].job->refresh_v3 && original_cache.records[0].job->both_formats);
    cJSON_Delete(targets); discard_queued_jobs();

    /* A complete old cache must not waive permissions for the cloud recheck. */
    local_complete[ORIGINAL_CACHE_V3] = true;
    settings[2].cloud.enableV3Chapter = false;
    settings[3].internal.client_tb2.key = NULL;
    targets = cJSON_CreateArray();
    assert(original_content_cache_refresh_group(&settings[1], test_ruid, targets) == ERROR_ACCESS_DENIED);
    assert_reason(target_at(targets, 0, "box-b", false), "tonies_forwarding_disabled");
    assert_reason(target_at(targets, 1, "box-c", false), "missing_tb2_certificates");
    assert(!pending_count()); cJSON_Delete(targets);
    settings[2].cloud.enableV3Chapter = true; settings[3].internal.client_tb2.key = "green-key";
    write_policy(test_ruid, "lib://private.taf", false, auth_hex);
    targets = cJSON_CreateArray();
    assert(original_content_cache_refresh_group(&settings[1], test_ruid, targets) == ERROR_ACCESS_DENIED);
    assert_reason(target_at(targets, 0, "box-b", false), "private_source");
    assert_reason(target_at(targets, 1, "box-c", false), "private_source");
    cJSON_Delete(targets);
    puts("PASS refresh group: TB2-only root scope, owned identities, V3-only, dedup/busy, no cache policy bypass");
}

static uint_t call_cache_api(const char *query)
{
    HttpConnection connection = {0};
    client_ctx_t ctx = {0};
    char uri[64]; snprintf(uri, sizeof(uri), "/api/content/cache/%s", test_ruid);
    assert(handleApiOriginalCache(&connection, uri, query, &ctx) == NO_ERROR);
    return connection.response.statusCode;
}

static void refresh_api_tests(void)
{
    configure_refresh_group();
    assert(osCreateEvent(&original_cache.wake)); original_cache.ready = true;
    assert(call_cache_api("overlay=box-a") == 202);
    assert(pending_count() == 1 && original_cache.records[0].job->both_formats);
    assert(!original_cache.records[0].job->refresh_v3);
    discard_queued_jobs();
    assert(call_cache_api("overlay=box-a&refresh=taf") == 400 && !pending_count());
    assert(call_cache_api("overlay=box-a&refresh=v3&refresh=v3") == 400 && !pending_count());
    assert(call_cache_api("overlay=unknown&refresh=v3") == 400 && !pending_count());
    settings[3].cloud.enableV3ContentMeta = false;
    assert(call_cache_api("overlay=box-a&refresh=v3") == 202);
    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(http_result, "targets");
    assert(cJSON_GetArraySize(targets) == 2);
    target_at(targets, 0, "box-b", true);
    assert_reason(target_at(targets, 1, "box-c", false), "tonies_forwarding_disabled");
    discard_queued_jobs();
    settings[2].cloud.enableV3ContentMeta = false;
    assert(call_cache_api("overlay=box-a&refresh=v3") == 409 && !pending_count());
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(http_result, "ok")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(http_result, "targets")) == 2);
    settings[2].cloud.enableV3ContentMeta = true; settings[3].cloud.enableV3ContentMeta = true;
    original_cache.ready = false;
    assert(call_cache_api("overlay=box-a&refresh=v3") == 503 && !pending_count());
    cJSON_Delete(http_result); http_result = NULL;
    puts("PASS refresh API: legacy POST, exact mode/overlay, partial 202, all-rejected 409 and unavailable 503");
}

static void refresh_worker_tests(void)
{
    configure_refresh_group();
    original_content_cache_init(); assert(original_cache.ready);
    const char *results[] = {"unchanged", "updated", "downloaded"};
    for (unsigned pass = 0; pass < 4; pass++)
    {
        refresh_result = results[pass % 3];
        adapter_error[ORIGINAL_CACHE_V3] = pass == 3 ? ERROR_TIMEOUT : NO_ERROR;
        cJSON *targets = cJSON_CreateArray();
        assert(!original_content_cache_refresh_group(&settings[1], test_ruid, targets));
        cJSON_Delete(targets); wait_idle();
        assert(atomic_load(&calls) == (pass + 1) * 2);
        for (unsigned box = 2; box <= 3; box++)
        {
            cJSON *state = cJSON_CreateObject();
            original_content_cache_add_status(state, &settings[box], test_ruid);
            const cJSON *download = cJSON_GetObjectItemCaseSensitive(state, "download");
            const cJSON *v3 = cJSON_GetObjectItemCaseSensitive(download, "v3");
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v3, "state")),
                           pass == 3 ? "failed" : "complete"));
            assert_reason(v3, pass == 3 ? "transport_failure" : refresh_result);
            assert_reason(cJSON_GetObjectItemCaseSensitive(download, "taf"), "not_requested");
            cJSON_Delete(state);
        }
    }
    for (unsigned i = 0; i < 8; i++)
        assert(order[i] == ORIGINAL_CACHE_V3 && refresh_flags[i] && owners[i] == 2 + i % 2);
    osDelayTask(30);
    assert(atomic_load(&calls) == 8 && !pending_count());
    original_content_cache_deinit();
    puts("PASS refresh worker: serial own-box V3-only jobs, honest outcomes/failures, no automatic follow-up");
}

int main(int argc, char **argv)
{
    assert(argc == 2); setup();
    if (!strcmp(argv[1], "queue")) queue_tests();
    else if (!strcmp(argv[1], "policy")) policy_tests();
    else if (!strcmp(argv[1], "worker")) worker_tests();
    else if (!strcmp(argv[1], "refresh-group")) refresh_group_tests();
    else if (!strcmp(argv[1], "refresh-api")) refresh_api_tests();
    else if (!strcmp(argv[1], "refresh-worker")) refresh_worker_tests();
    else assert(!"unknown case");
    return 0;
}
