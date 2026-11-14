/* Execute the production selection decision with controlled content/cache data.
 * The separate V3 route tests cover real files, manifests and atomic publication. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handler_cloud.h"
#include "v3_native_cache.h"
#include "cJSON.h"
#include "mutex_manager.h"

#undef TRACE_INFO
#define TRACE_INFO(...) ((void)0)
#undef TRACE_WARNING
#define TRACE_WARNING(...) ((void)0)
#undef TRACE_DEBUG
#define TRACE_DEBUG(...) ((void)0)

#define ORIGINAL_VERSION UINT32_C(1794257152)
#define OLD_TAF_VERSION UINT32_C(1758527875)
#define ORIGINAL_CHAPTERS 24U
#define OLD_TAF_CHAPTERS 11U
#define ASSIGNED_VERSION UINT32_C(1700000000)
#define TEST_RUID "00112233445566E0"

static bool_t original_complete = TRUE;
static unsigned cache_reads;
static tonie_info_t *current_tonie;
static uint64_t cloud_pending;
static char *original_manifest;
static char *response_body;
static unsigned response_count, route_releases;
static settings_t *current_settings;
static uint32_t box_version = OLD_TAF_VERSION;
static bool_t inventory_known = TRUE;
static uint32_t target_version;
static unsigned state_lock, hint_calls;
static bool_t hint_admitted = TRUE;
static bool_t source_changed;
static void (*after_cache_read)(void);

/* Only the ledger boundary is substituted here; its reservation semantics are
 * exercised with production code in test_mqtt_fresh_tonies_runtime. */
struct mqtt_freshness_snapshot { unsigned bound; };

bool_t mqtt_server_freshness_bind_original_locked(struct mqtt_freshness_snapshot *snapshot,
    uint64_t uid, uint32_t version)
{
    assert(state_lock == 1 && uid != 0 && version == target_version);
    if (snapshot == NULL) return FALSE;
    snapshot->bound++;
    return TRUE;
}

void osFreeMem(void *pointer) { free(pointer); }

tonie_info_t *getTonieInfoFromRuid(char ruid[17], bool lock, settings_t *settings)
{
    assert(strcmp(ruid, TEST_RUID) == 0 && !lock && settings != NULL);
    return current_tonie;
}

void freeTonieInfo(tonie_info_t *tonie) { assert(tonie != NULL && tonie == current_tonie); }

tonie_info_t *getTonieInfoFromUid(uint64_t uid, bool lock, settings_t *settings)
{
    assert(uid != 0 && !lock && settings == current_settings);
    return current_tonie;
}

settings_t *get_settings_id(uint8_t overlay)
{
    assert(overlay == 4);
    return current_settings;
}

void mutex_lock(mutex_id_t mutex)
{
    assert(mutex == MUTEX_MQTT_SESSION && state_lock == 0);
    state_lock++;
}

void mutex_unlock(mutex_id_t mutex)
{
    assert(mutex == MUTEX_MQTT_SESSION && state_lock == 1);
    state_lock--;
}

bool original_content_cache_job_current(const original_content_cache_job_t *job,
    original_cache_format_t format)
{
    return job != NULL && format == ORIGINAL_CACHE_V3;
}

static bool_t freshness_inventory_find_uid(settings_t *settings, uint64_t uid,
    uint32_t *version)
{
    assert(settings == current_settings && uid != 0);
    if (!inventory_known) return FALSE;
    *version = box_version;
    return TRUE;
}

static bool_t freshness_settings_array_contains_locked(settings_t *settings,
    const char *key, uint64_t uid)
{
    assert(settings == current_settings && uid != 0 && state_lock == 1);
    assert(strcmp(key, "internal.freshnessCacheSourceChangedUids") == 0);
    return source_changed;
}

static bool_t freshness_cache_source_changed_contains(settings_t *settings, uint64_t uid)
{
    mutex_lock(MUTEX_MQTT_SESSION);
    bool_t result = freshness_settings_array_contains_locked(settings,
        "internal.freshnessCacheSourceChangedUids", uid);
    mutex_unlock(MUTEX_MQTT_SESSION);
    return result;
}

static bool_t freshness_cache_add_uid(settings_t *settings, uint64_t uid, bool_t *added)
{
    assert(settings == current_settings && uid != 0 && state_lock == 1);
    if (added != NULL) *added = TRUE;
    return TRUE;
}

static bool_t freshness_cache_remove_uid_locked(settings_t *settings, uint64_t uid)
{
    assert(settings == current_settings && uid != 0 && state_lock == 1);
    target_version = 0;
    return TRUE;
}

static bool_t freshness_cache_remove_uid(settings_t *settings, uint64_t uid)
{
    mutex_lock(MUTEX_MQTT_SESSION);
    bool_t result = freshness_cache_remove_uid_locked(settings, uid);
    mutex_unlock(MUTEX_MQTT_SESSION);
    return result;
}

static uint32_t freshness_v3_content_meta_version(settings_t *settings,
    tonie_info_t *tonie, const char *ruid, uint32_t fallback)
{
    assert(settings == current_settings && tonie == current_tonie && fallback == 0);
    assert(strcmp(ruid, TEST_RUID) == 0 && source_changed);
    return ASSIGNED_VERSION;
}

static bool_t freshness_forced_version_find(settings_t *settings, uint64_t uid,
    uint32_t natural, uint32_t *forced)
{
    assert(settings != NULL && uid != 0 && natural != 0);
    *forced = ASSIGNED_VERSION;
    return TRUE;
}

uint64_t mqtt_server_original_cloud_pending_locked(uint8_t overlay, uint64_t uid)
{
    assert(overlay == 4 && uid != 0 && state_lock == 1);
    return cloud_pending;
}

bool_t mqtt_server_original_cloud_checked_locked(uint8_t overlay, uint64_t uid, uint64_t generation)
{
    assert(overlay == 4 && uid != 0 && state_lock == 1);
    if (!current_settings->internal.config_used || generation != cloud_pending) return FALSE;
    cloud_pending = 0;
    return TRUE;
}

bool_t mqtt_server_original_target_locked(uint8_t overlay, uint64_t uid, uint32_t version)
{
    assert(overlay == 4 && uid != 0 && state_lock == 1);
    target_version = version;
    return TRUE;
}

bool_t mqtt_server_original_confirm_locked(uint8_t overlay, uint64_t uid, uint32_t version)
{
    assert(overlay == 4 && uid != 0 && state_lock == 1);
    return target_version == version && cloud_pending == 0;
}

bool_t freshness_original_cloud_hint(settings_t *settings, const char *ruid)
{
    assert(settings == current_settings && strcmp(ruid, TEST_RUID) == 0 && state_lock == 0);
    hint_calls++;
    cloud_pending++;
    return hint_admitted;
}

uint64_t mqtt_server_original_cloud_pending(uint8_t overlay_id, uint64_t uid)
{
    assert(overlay_id == 4 && uid != 0);
    return cloud_pending;
}

#define freshness_ruid_to_uid tb2_ruid_to_uid
#define freshness_uid_to_ruid tb2_ruid_from_uid

/* METADATA_ROUTE_CLOUD_CHECK */

error_t v3_native_cache_open_active_manifest(const char *cache_root,
    const char *library_root, uint8_t overlay_id, const char *ruid,
    uint8_t **data, size_t *length, uint32_t *version,
    v3_native_cache_route_handle_t *handle)
{
    assert(strcmp(cache_root, "cache") == 0 && strcmp(library_root, "library") == 0);
    assert(overlay_id == 4 && strcmp(ruid, TEST_RUID) == 0);
    if (!original_complete) return ERROR_NOT_FOUND;
    *data = (uint8_t *)strdup(original_manifest);
    assert(*data != NULL);
    *length = strlen(original_manifest);
    *version = ORIGINAL_VERSION;
    handle->serial = 1;
    return NO_ERROR;
}

void v3_native_cache_route_release(v3_native_cache_route_handle_t *handle)
{
    if (handle->serial != 0) route_releases++;
    memset(handle, 0, sizeof(*handle));
}

void v3_native_cache_invalidate_routes(uint8_t overlay_id, const char *ruid)
{
    assert(overlay_id == 4 && strcmp(ruid, TEST_RUID) == 0);
}

static void v3_native_import_library_if_enabled(client_ctx_t *context,
    const char *ruid, uint32_t version)
{
    assert(context != NULL && strcmp(ruid, TEST_RUID) == 0 && version == ORIGINAL_VERSION);
}

static error_t v3_local_write_empty_status(HttpConnection *connection, uint16_t status)
{
    connection->response.statusCode = status;
    return NO_ERROR;
}

void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{
    assert(strcmp(type, "application/json") == 0);
    connection->response.contentLength = length;
}

error_t httpWriteResponse(HttpConnection *connection, void *data, size_t length, bool_t free_memory)
{
    assert(!free_memory && length == connection->response.contentLength);
    free(response_body);
    response_body = strndup(data, length);
    assert(response_body != NULL);
    response_count++;
    return NO_ERROR;
}

bool_t v3_native_cache_active_info(const char *cache_root, const char *library_root,
    uint8_t overlay_id, const char *ruid, uint32_t *version, size_t *object_count,
    bool_t *tonieplay)
{
    assert(strcmp(cache_root, "cache") == 0);
    assert(strcmp(library_root, "library") == 0);
    assert(overlay_id == 4);
    assert(strcmp(ruid, TEST_RUID) == 0);
    assert(state_lock == 0); /* Never perform complete-cache resolution under state lock. */
    cache_reads++;
    if (!original_complete) return FALSE;
    if (version != NULL) *version = ORIGINAL_VERSION;
    if (object_count != NULL) *object_count = ORIGINAL_CHAPTERS;
    if (tonieplay != NULL) *tonieplay = FALSE;
    if (after_cache_read != NULL)
    {
        void (*callback)(void) = after_cache_read;
        after_cache_read = NULL;
        callback();
    }
    return TRUE;
}

bool_t v3_native_library_source_audio_id(const char *source, uint32_t *audio_id)
{
    (void)source;
    *audio_id = ASSIGNED_VERSION;
    return TRUE;
}

/* ORIGINAL_SELECTION_FUNCTIONS */

/* CLOUD_FRESHNESS_REQUEST_BUILDER */

static void test_cloud_request_versions_and_aliases(settings_t *settings, tonie_info_t *tonie)
{
    uint64_t first_uid, second_uid, upstream_uid;
    const char *second_ruid = "10213243546576E0";
    const char *upstream_ruid = "AABBCCDDEEFF00E0";
    assert(tb2_ruid_to_uid(TEST_RUID, &first_uid));
    assert(tb2_ruid_to_uid(second_ruid, &second_uid));
    assert(tb2_ruid_to_uid(upstream_ruid, &upstream_uid));
    TonieFCInfo first = {.uid = first_uid, .audio_id = OLD_TAF_VERSION};
    TonieFCInfo second = {.uid = second_uid, .audio_id = OLD_TAF_VERSION - 7U};
    TonieFCInfo *infos[] = {&first, &second};
    TonieFreshnessCheckRequest request = {.n_tonie_infos = 1, .tonie_infos = infos};
    v3_original_freshness_t originals[] = {
        {.uid = first_uid, .upstream_uid = first_uid, .box_version = first.audio_id,
         .original = TRUE, .complete = TRUE},
        {.uid = second_uid, .upstream_uid = upstream_uid, .box_version = second.audio_id,
         .cache_version = ORIGINAL_VERSION + 2U, .original = TRUE, .complete = TRUE},
    };
    assert(freshness_original_selected_version(settings, tonie, first_uid, &originals[0].cache_version));
    cbr_ctx_t context = {.freshnessOriginals = originals, .freshnessOriginalCount = 1};
    cJSON *json = build_cloud_freshness_request(request, context);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
    assert(cJSON_GetObjectItemCaseSensitive(content, TEST_RUID)->valuedouble == ORIGINAL_VERSION);
    assert(originals[0].requested && originals[0].query_version == ORIGINAL_VERSION);
    assert(first.audio_id == OLD_TAF_VERSION && first.uid == first_uid);
    cJSON_Delete(json);

    /* cloud_ruid changes only the owned request, never the box inventory. */
    originals[0].upstream_uid = upstream_uid;
    json = build_cloud_freshness_request(request, context);
    content = cJSON_GetObjectItemCaseSensitive(json, "content");
    assert(cJSON_GetObjectItemCaseSensitive(content, TEST_RUID) == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(content, upstream_ruid)->valuedouble == ORIGINAL_VERSION);
    cJSON_Delete(json);

    request.n_tonie_infos = context.freshnessOriginalCount = 2;
    json = build_cloud_freshness_request(request, context);
    content = cJSON_GetObjectItemCaseSensitive(json, "content");
    assert(cJSON_GetArraySize(content) == 1); /* identical upstream identity, one POST entry */
    assert(cJSON_GetObjectItemCaseSensitive(content, upstream_ruid)->valuedouble == ORIGINAL_VERSION);
    assert(originals[0].query_version == ORIGINAL_VERSION && originals[1].query_version == ORIGINAL_VERSION);
    cJSON_Delete(json);

    originals[1].complete = FALSE;
    json = build_cloud_freshness_request(request, context);
    content = cJSON_GetObjectItemCaseSensitive(json, "content");
    assert(cJSON_GetObjectItemCaseSensitive(content, upstream_ruid)->valuedouble == second.audio_id);
    assert(originals[0].query_version == second.audio_id && originals[1].query_version == second.audio_id);
    assert(first.audio_id == OLD_TAF_VERSION && first.uid == first_uid);
    assert(second.audio_id == OLD_TAF_VERSION - 7U && second.uid == second_uid);
    cJSON_Delete(json);
}

static void publish_newer_target_after_cache_read(void)
{
    assert(state_lock == 0);
    target_version = ORIGINAL_VERSION + 1U;
}

static void test_original_completion_and_http(settings_t *settings, tonie_info_t *tonie)
{
    current_settings = settings;
    current_tonie = tonie;
    settings->internal.overlayUniqueId = "box-test";
    original_content_cache_job_t job = {0};
    job.settings = *settings;
    strcpy(job.owner, settings->internal.overlayUniqueId);
    strcpy(job.ruid, TEST_RUID);
    target_version = 0;
    current_tonie = NULL; /* Failed content lookup must not clear pending work. */
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == 0);
    assert(!freshness_confirm_v3_content_version(settings, TEST_RUID, ORIGINAL_VERSION));
    current_tonie = tonie;
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == ORIGINAL_VERSION); /* manual refresh without previous hint */
    inventory_known = FALSE;
    target_version = 0;
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == 0); /* no assumed box mismatch */
    cloud_pending = 1;
    job.sync_generation = 1;
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == ORIGINAL_VERSION && cloud_pending == 0); /* consumed real hint still reaches box */
    inventory_known = TRUE;
    target_version = 0;
    cloud_pending = 2;
    job.sync_generation = 1;
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == 0 && cloud_pending == 2); /* old completion cannot clear newer work */
    job.sync_generation = 2;
    original_content_cache_v3_complete(&job, ORIGINAL_VERSION);
    assert(target_version == ORIGINAL_VERSION && cloud_pending == 0);

    after_cache_read = publish_newer_target_after_cache_read;
    assert(!freshness_confirm_v3_content_version(settings, TEST_RUID, ORIGINAL_VERSION));
    assert(target_version == ORIGINAL_VERSION + 1U);
    target_version = ORIGINAL_VERSION;

    assert(!freshness_confirm_v3_content_version(settings, TEST_RUID, OLD_TAF_VERSION));
    assert(target_version == ORIGINAL_VERSION);
    cloud_pending = 3;
    assert(!freshness_confirm_v3_content_version(settings, TEST_RUID, ORIGINAL_VERSION));
    assert(target_version == ORIGINAL_VERSION);
    cloud_pending = 0;
    assert(freshness_confirm_v3_content_version(settings, TEST_RUID, ORIGINAL_VERSION));
    assert(target_version == 0);
    tonie->json.source = "lib://private.taf";
    source_changed = TRUE;
    target_version = ASSIGNED_VERSION;
    assert(!freshness_confirm_v3_content_version(settings, TEST_RUID, ORIGINAL_VERSION));
    assert(freshness_confirm_v3_content_version(settings, TEST_RUID, ASSIGNED_VERSION));
    assert(target_version == 0);
    source_changed = FALSE;
    tonie->json.source = "";

    uint64_t uid;
    assert(tb2_ruid_to_uid(TEST_RUID, &uid));
    uint64_t marked = uid;
    TonieFreshnessCheckResponse response = {.n_tonie_marked = 1, .tonie_marked = &marked};
    v3_original_freshness_t original = {.uid = uid, .box_version = OLD_TAF_VERSION,
        .cache_version = ORIGINAL_VERSION, .query_version = ORIGINAL_VERSION,
        .original = TRUE, .complete = TRUE, .requested = TRUE};
    client_ctx_t client = {.settings = settings};
    struct mqtt_freshness_snapshot snapshot = {0};
    cbr_ctx_t context = {.client_ctx = &client, .freshnessOwner = "box-test",
        .customData = &response, .customDataLen = 1, .freshnessOriginals = &original,
        .freshnessOriginalCount = 1, .freshnessCloudValid = TRUE, .freshnessSnapshot = &snapshot};
    target_version = 0;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 1 && target_version == ORIGINAL_VERSION && hint_calls == 0);
    assert(snapshot.bound == 1);

    /* A late stale response must not notify while a NEWER accepted check runs. */
    original.stale = TRUE;
    cloud_pending = 2;
    target_version = 0;
    response.n_tonie_marked = 1;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 0 && cloud_pending == 2 && target_version == 0 && hint_calls == 0);

    cloud_pending = 0;
    response.n_tonie_marked = 1;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 0 && cloud_pending == 1 && hint_calls == 1);
    cloud_pending = 0;
    hint_admitted = FALSE;
    response.n_tonie_marked = 1;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 1 && cloud_pending == 1 && hint_calls == 2);

    /* A failed cloud query must not newly roll the box back to stale TC bytes. */
    cloud_pending = 0;
    context.freshnessCloudValid = FALSE;
    original.stale = FALSE;
    response.n_tonie_marked = 1;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 0 && hint_calls == 2);
    tonie->json.nocloud = TRUE;
    /* NoCloud may be enabled after this HTTP snapshot and a newer hint. */
    cloud_pending = 3;
    response.n_tonie_marked = 1;
    freshness_original_finish_response(&context);
    assert(response.n_tonie_marked == 1 && target_version == ORIGINAL_VERSION && hint_calls == 2);
    tonie->json.nocloud = FALSE;
    hint_admitted = TRUE;
    settings->internal.overlayUniqueId = NULL;
}

static void test_original_manifest_delivery(settings_t *settings, tonie_info_t *tonie)
{
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddNumberToObject(manifest, "version", ORIGINAL_VERSION);
    cJSON *chapters = cJSON_AddArrayToObject(manifest, "content");
    for (unsigned chapter = 0; chapter < ORIGINAL_CHAPTERS; chapter++)
    {
        cJSON *object = cJSON_CreateObject();
        char name[32];
        snprintf(name, sizeof(name), "original-%02u.opus", chapter);
        cJSON_AddStringToObject(object, "name", name);
        cJSON_AddStringToObject(object, "type", "audio");
        cJSON_AddNumberToObject(object, "fileSize", 4);
        cJSON_AddItemToArray(chapters, object);
    }
    original_manifest = cJSON_PrintUnformatted(manifest);
    cJSON_Delete(manifest);
    assert(original_manifest != NULL);
    HttpConnection connection = {0};
    client_ctx_t context = {.settings = settings};
    error_t error = ERROR_FAILURE;
    current_tonie = tonie;
    uint64_t stale_uid;
    assert(tb2_ruid_to_uid(TEST_RUID, &stale_uid));
    settings->internal.freshnessCache = &stale_uid;

    /* A stale BOX is not a stale TC cache: deliver all 24 original chapters. */
    assert(!metadata_route_cloud_check(&context, tonie, TEST_RUID));
    assert(v3_native_serve_cached_original(&connection, &context, TEST_RUID,
        metadata_route_cloud_check(&context, tonie, TEST_RUID), &error));
    assert(error == NO_ERROR && response_count == 1 && route_releases == 1);
    assert(strcmp(response_body, original_manifest) == 0);
    manifest = cJSON_Parse(response_body);
    assert(cJSON_GetObjectItemCaseSensitive(manifest, "version")->valuedouble == ORIGINAL_VERSION);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(manifest, "content")) == ORIGINAL_CHAPTERS);
    cJSON_Delete(manifest);

    cloud_pending = 1;
    assert(metadata_route_cloud_check(&context, tonie, TEST_RUID));
    assert(!v3_native_serve_cached_original(&connection, &context, TEST_RUID,
        metadata_route_cloud_check(&context, tonie, TEST_RUID), &error));
    assert(response_count == 1);

    /* NoCloud retains local availability even with an older pending check. */
    tonie->json.nocloud = TRUE;
    assert(!metadata_route_cloud_check(&context, tonie, TEST_RUID));
    assert(v3_native_serve_cached_original(&connection, &context, TEST_RUID,
        metadata_route_cloud_check(&context, tonie, TEST_RUID), &error));
    assert(response_count == 2 && strcmp(response_body, original_manifest) == 0);
    tonie->json.nocloud = FALSE;
    cloud_pending = 0;

    tonie->json.source = "lib://private.taf";
    assert(!v3_native_serve_cached_original(&connection, &context, TEST_RUID, FALSE, &error));
    assert(response_count == 2);
    tonie->json.source = "";
    settings->internal.freshnessCache = NULL;
    free(original_manifest);
    free(response_body);
}

int main(void)
{
    TonieboxAudioFileHeader header = {0};
    tonie_info_t tonie = {0};
    settings_t settings = {0};
    settings.toniebox.boxGeneration = GENERATION_TB2;
    settings.internal.config_used = TRUE;
    settings.internal.overlayNumber = 4;
    settings.internal.cachedirfull = "cache";
    settings.internal.librarydirfull = "library";
    settings.internal.contentdirfull = "content";
    uint64_t uid;
    assert(tb2_ruid_to_uid(TEST_RUID, &uid));
    header.audio_id = OLD_TAF_VERSION;
    header.n_track_page_nums = OLD_TAF_CHAPTERS;
    tonie.valid = TRUE;
    tonie.exists = TRUE;
    tonie.tafHeader = &header;
    tonie.json.source = "";
    tonie.json.cache_preference = CONTENT_JSON_CACHE_PREFERENCE_V3;
    uint32_t version = 0;
    bool_t valid = freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                         &version, NULL);
    if (!valid || version != ORIGINAL_VERSION)
    {
        fprintf(stderr, "FAIL: complete V3 (%u chapters, version %" PRIu32
                ") must win over old TAF (%u chapters, got %" PRIu32 ")\n",
                ORIGINAL_CHAPTERS, ORIGINAL_VERSION, OLD_TAF_CHAPTERS, version);
        return EXIT_FAILURE;
    }
    assert(cache_reads == 1);
    bool_t forced = TRUE;
    assert(freshness_get_effective_server_audio_id(&settings, &tonie, uid,
        &version, NULL, &forced));
    assert(version == ORIGINAL_VERSION && !forced);
    tonie.json.cache_preference = CONTENT_JSON_CACHE_PREFERENCE_AUTO;
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == ORIGINAL_VERSION);

    /* NoCloud forbids new cloud work, not playback of an existing original. */
    tonie.json.nocloud = TRUE;
    tonie.json.cloud_override = TRUE;
    assert(freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(version == ORIGINAL_VERSION);
    tonie.json.nocloud = FALSE;
    tonie.json.cloud_override = FALSE;

    /* Explicit TAF preference remains authoritative when a usable TAF exists. */
    tonie.json.cache_preference = CONTENT_JSON_CACHE_PREFERENCE_TAF;
    cache_reads = 0;
    assert(!freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == OLD_TAF_VERSION && cache_reads == 0);
    forced = FALSE;
    assert(freshness_get_effective_server_audio_id(&settings, &tonie, uid,
        &version, NULL, &forced));
    assert(version == ASSIGNED_VERSION && forced);
    tonie.valid = FALSE;
    assert(freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(version == ORIGINAL_VERSION);
    tonie.valid = TRUE;
    tonie.json._source_type = CT_SOURCE_TAF_INCOMPLETE;
    assert(freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(version == ORIGINAL_VERSION);
    tonie.json._source_type = CT_SOURCE_NONE;

    /* A private source may never be replaced by the retained original cache. */
    tonie.json.source = "lib://assigned.taf";
    tonie.json._source_type = CT_SOURCE_TAF;
    tonie.json.cache_preference = CONTENT_JSON_CACHE_PREFERENCE_V3;
    cache_reads = 0;
    assert(!freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == OLD_TAF_VERSION && cache_reads == 0);
    tonie.json._source_type = CT_SOURCE_NATIVE_COLLECTION;
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == ASSIGNED_VERSION && cache_reads == 0);
    tonie.json._source_type = CT_SOURCE_TAP_CACHED;
    tonie.json._tap._valid = TRUE;
    tonie.json._tap.audio_id = ASSIGNED_VERSION;
    bool_t tap_version = FALSE;
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, &tap_version));
    assert(version == ASSIGNED_VERSION && tap_version && cache_reads == 0);
    tonie.json._source_type = CT_SOURCE_NONE;
    tonie.json.source = "";

    /* This fix must not change TB1's selected TAF version. */
    settings.toniebox.boxGeneration = GENERATION_TB1;
    assert(!freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == OLD_TAF_VERSION && cache_reads == 0);
    settings.toniebox.boxGeneration = GENERATION_TB2;

    test_original_manifest_delivery(&settings, &tonie);
    test_original_completion_and_http(&settings, &tonie);
    test_cloud_request_versions_and_aliases(&settings, &tonie);

    /* A missing native generation is not claimed to be complete/current. */
    original_complete = FALSE;
    assert(!freshness_original_selected_version(&settings, &tonie, uid, &version));
    assert(freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                &version, NULL));
    assert(version == OLD_TAF_VERSION);
    tonie.valid = FALSE;
    assert(!freshness_get_natural_server_audio_id(&settings, &tonie, uid,
                                                 &version, NULL));

    puts("PASS: 24-chapter original delivery despite box-stale marker; V3/TAF selection, NoCloud, private sources, TB1 and cache miss");
    return EXIT_SUCCESS;
}
