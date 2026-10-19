/* Real queue/policy/worker with real files and synchronization; transport only
 * is replaced. Run inside a disposable directory via the Python launcher. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRACE_LEVEL 0
#include "../src/original_content_cache.c"

static settings_t settings[MAX_OVERLAYS];
static OsMutex locks[MUTEX_LAST];
static atomic_uint calls;
static unsigned order[64];
static bool library_flags[64];
static atomic_bool adapter_wait;
static error_t adapter_error[ORIGINAL_CACHE_FORMAT_COUNT];
static bool local_complete[ORIGINAL_CACHE_FORMAT_COUNT];
static const char *test_ruid = "8194F21C500304E0";
static const char *auth_hex = "0102030405060708010203040506070801020304050607080102030405060708";
static const char *cache_preference = "auto";

settings_t *get_settings(void) { return &settings[0]; }
settings_t *get_settings_id(uint8_t index) { assert(index < MAX_OVERLAYS); return &settings[index]; }
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
    assert(job->settings.cloud.remote_hostname != settings[1].cloud.remote_hostname);
    assert(!strcmp(job->settings.cloud.remote_hostname, "boxine.test"));
    assert(job->tb1_identity.key != settings[1].internal.client_tb1.key);
    assert(!strcmp(job->tb1_identity.key, "key"));
    assert(job->auth[0] == 1 && job->auth[31] == 8);
    while (atomic_load(&adapter_wait)) osDelayTask(1);
    *reason = adapter_error[format] ? "transport_failure" : NULL;
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

int main(int argc, char **argv)
{
    assert(argc == 2); setup();
    if (!strcmp(argv[1], "queue")) queue_tests();
    else if (!strcmp(argv[1], "policy")) policy_tests();
    else if (!strcmp(argv[1], "worker")) worker_tests();
    else assert(!"unknown case");
    return 0;
}
