/* Exercise the production cache and extracted handler callbacks with real POSIX
 * files. Allocation, settings, mutex and HTTP transport are fixture boundaries. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "contentJson.h"
#include "handler.h"
#include "mutex_manager.h"
#include "v3_native_cache.h"
#include "cJSON.h"

#undef TRACE_ERROR
#undef TRACE_WARNING
#define TRACE_ERROR(...) ((void)0)
#define TRACE_WARNING(...) ((void)0)

#define TEST_OVERLAY 5
#define CONTENT_RUID "CC467027500304E0"
#define SYSTEM_RUID "00000AF000000001"
#define TEST_CACHE "cache"
#define TEST_LIBRARY "library"
#define TEST_ROUTE_CAPACITY V3_NATIVE_CACHE_ROUTES_PER_OVERLAY

static settings_t settings;
static unsigned held[MUTEX_LAST];
static tonie_info_t policy_content;
static bool_t policy_missing;
static unsigned policy_reads, policy_frees;
static unsigned transport_responses, imports;
static void (*library_release_hook)(void);

void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *pointer) { free(pointer); }
void osSuspendAllTasks(void) {}
void osResumeAllTasks(void) {}
settings_t *get_settings(void) { return &settings; }

tonie_info_t *getTonieInfoFromRuid(char ruid[17], bool lock, settings_t *current)
{
    assert(ruid != NULL && !lock && current == &settings);
    policy_reads++;
    return policy_missing ? NULL : &policy_content;
}

void freeTonieInfo(tonie_info_t *info)
{
    assert(info == &policy_content);
    policy_frees++;
}

/* HANDLER_AUTH_AND_POLICY_FUNCTIONS */

void cbrCloudResponsePassthrough(void *source, HttpClientContext *cloud)
{
    assert(source != NULL && cloud != NULL);
    transport_responses++;
}

error_t httpSend(HttpConnection *connection, const void *data, size_t length,
                 uint_t flags)
{
    (void)data;
    (void)length;
    (void)flags;
    assert(connection != NULL);
    return NO_ERROR;
}

static void v3_native_import_library_if_enabled(client_ctx_t *client, const char *ruid,
                                                uint32_t version)
{
    assert(client != NULL && ruid != NULL && version != 0);
    imports++;
}

/* HANDLER_CHAPTER_CALLBACKS */

void mutex_lock(mutex_id_t id)
{
    assert(id < MUTEX_LAST && held[id] == 0);
    if (id == MUTEX_V3_NATIVE_LIBRARY)
        assert(held[MUTEX_V3_NATIVE_CACHE] == 0);
    held[id]++;
}

void mutex_unlock(mutex_id_t id)
{
    assert(id < MUTEX_LAST && held[id] == 1);
    held[id]--;
    if (id == MUTEX_V3_NATIVE_LIBRARY && library_release_hook != NULL)
    {
        void (*hook)(void) = library_release_hook;
        library_release_hook = NULL;
        hook();
    }
}

static void meta_body_auth(v3_native_cache_meta_capture_t *capture, uint32_t version,
                           const char *name, const char *second_name, const char *auth)
{
    size_t capacity = strlen(auth) + strlen(name) + 1024U;
    char *manifest = malloc(capacity);
    assert(manifest != NULL);
    char second[512] = {0};
    if (second_name != NULL)
        snprintf(second, sizeof(second),
                 ",{\"name\":\"%s\",\"auth\":\"opaque-token\","
                 "\"type\":\"audio\",\"fileSize\":4}", second_name);
    int size = snprintf(manifest, capacity,
                        "{\"version\":%u,\"content\":[{\"name\":\"%s\","
                        "\"auth\":\"%s\",\"type\":\"audio\",\"fileSize\":4}%s]}",
                        version, name, auth, second);
    assert(size > 0 && (size_t)size < capacity);
    v3_native_cache_meta_capture_response(capture, 200);
    v3_native_cache_meta_capture_append(capture, manifest, (size_t)size);
    free(manifest);
}

static void meta_body(v3_native_cache_meta_capture_t *capture, uint32_t version,
                      const char *name, const char *second_name)
{
    meta_body_auth(capture, version, name, second_name, "opaque-token");
}

static void manifest(const char *ruid, uint32_t version, const char *name,
                     const char *second_name, bool_t store)
{
    v3_native_cache_meta_capture_t capture;
    if (store)
        v3_native_cache_meta_capture_init(&capture, TEST_CACHE, TEST_LIBRARY,
                                           TEST_OVERLAY, ruid);
    else
        v3_native_cache_meta_observe_init(&capture, TEST_OVERLAY, ruid);
    assert(!capture.failed);
    meta_body(&capture, version, name, second_name);
    assert(v3_native_cache_meta_capture_finish(&capture) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&capture);
}

static v3_native_cache_chapter_action_t prepare(
    const char *name, v3_native_cache_chapter_capture_t *capture, char **path)
{
    return v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                            name, NULL, capture, path);
}

static void finish_object(v3_native_cache_chapter_capture_t *capture)
{
    v3_native_cache_chapter_append(capture, "OggS", 4);
    assert(v3_native_cache_chapter_finish(capture) == NO_ERROR);
    v3_native_cache_chapter_abort(capture);
}

static void complete_generation(const char *ruid, uint32_t version, const char *name)
{
    manifest(ruid, version, name, NULL, TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare(name, &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    assert(path == NULL);
    finish_object(&capture);
    assert(v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, ruid, NULL));
}

static void test_interleaved_content_and_system_manifest(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, FALSE);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    manifest(SYSTEM_RUID, 7, "language.opus", NULL, FALSE);
    bool_t original_retained = v3_native_cache_route_matches(
        TEST_OVERLAY, CONTENT_RUID, 7, "original.opus");
    bool_t system_retained = v3_native_cache_route_matches(
        TEST_OVERLAY, SYSTEM_RUID, 7, "language.opus");
    printf("interleaved routes: content=%d system=%d\n",
           original_retained, system_retained);
    fflush(stdout);
    assert(original_retained && system_retained);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(strcmp(capture.ruid, CONTENT_RUID) == 0);
    v3_native_cache_chapter_abort(&capture);
    assert(prepare("language.opus", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(strcmp(capture.ruid, SYSTEM_RUID) == 0);
    v3_native_cache_chapter_abort(&capture);
}

static void test_duplicate_abort_keeps_original_capture(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", "second.opus", TRUE);
    v3_native_cache_chapter_capture_t first, duplicate, third;
    char *path = NULL;
    assert(prepare("original.opus", &first, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    assert(prepare("original.opus", &duplicate, &path) == V3_NATIVE_CHAPTER_FORWARD);
    v3_native_cache_chapter_abort(&duplicate);
    assert(prepare("original.opus", &third, &path) == V3_NATIVE_CHAPTER_FORWARD);
    v3_native_cache_chapter_abort(&third);
    finish_object(&first);
    assert(prepare("second.opus", &first, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&first);
}

static void test_stored_interleaving_completes_both_generations(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t original, language;
    char *path = NULL;
    assert(prepare("original.opus", &original, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    manifest(SYSTEM_RUID, 3, "language.opus", NULL, TRUE);
    assert(prepare("language.opus", &language, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&original);
    finish_object(&language);
    assert(v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID, NULL));
    assert(v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, SYSTEM_RUID, NULL));
    assert(prepare("original.opus", &original, &path) == V3_NATIVE_CHAPTER_SERVE);
    assert(path != NULL);
    free(path);
    v3_native_cache_chapter_abort(&original);
}

static void test_active_info_does_not_rollback_live_route(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    manifest(CONTENT_RUID, 8, "new.opus", NULL, FALSE);
    manifest(SYSTEM_RUID, 3, "language.opus", NULL, FALSE);
    uint32_t version = 0;
    size_t count = 0;
    bool_t tonieplay = TRUE;
    assert(v3_native_cache_active_info(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                        CONTENT_RUID, &version, &count, &tonieplay));
    assert(version == 7 && count == 1 && !tonieplay);
    assert(!v3_native_cache_active_is_tonieplay(TEST_CACHE, TEST_LIBRARY,
                                                TEST_OVERLAY, CONTENT_RUID));
    uint8_t *data = NULL;
    size_t length = 0;
    assert(v3_native_cache_read_active_manifest(TEST_CACHE, TEST_LIBRARY,
                                                 TEST_OVERLAY, CONTENT_RUID,
                                                 &data, &length, &version) == NO_ERROR);
    assert(data != NULL && length > 0 && version == 7);
    free(data);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 8, "new.opus"));
    assert(v3_native_cache_route_matches(TEST_OVERLAY, SYSTEM_RUID, 3, "language.opus"));
}

static void test_out_of_order_meta_is_not_published(void)
{
    v3_native_cache_meta_capture_t old, current;
    v3_native_cache_meta_observe_init(&old, TEST_OVERLAY, CONTENT_RUID);
    meta_body(&old, 7, "old.opus", NULL);
    v3_native_cache_meta_observe_init(&current, TEST_OVERLAY, CONTENT_RUID);
    meta_body(&current, 8, "new.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&current) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&current);
    assert(v3_native_cache_meta_capture_finish(&old) == ERROR_ABORTED);
    v3_native_cache_meta_capture_abort(&old);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 8, "new.opus"));
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "old.opus"));
}

static void test_invalidation_prevents_late_capture_activation(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    v3_native_cache_invalidate(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID);
    v3_native_cache_chapter_append(&capture, "OggS", 4);
    assert(v3_native_cache_chapter_finish(&capture) == ERROR_ABORTED);
    v3_native_cache_chapter_abort(&capture);
    assert(!v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID, NULL));
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
}

static void test_ambiguous_object_name_is_rejected(void)
{
    manifest(CONTENT_RUID, 7, "shared.opus", NULL, FALSE);
    manifest(SYSTEM_RUID, 3, "shared.opus", NULL, FALSE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("shared.opus", &capture, &path) == V3_NATIVE_CHAPTER_REJECT);
    assert(path == NULL);
    v3_native_cache_chapter_abort(&capture);
}

static void test_library_import_keeps_unrelated_route(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    manifest(SYSTEM_RUID, 3, "language.opus", NULL, FALSE);
    char *source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  TEST_OVERLAY, CONTENT_RUID,
                                                  &source) == NO_ERROR);
    assert(source != NULL && v3_native_library_source_is_candidate(source));
    char *repeated = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  TEST_OVERLAY, CONTENT_RUID,
                                                  &repeated) == NO_ERROR);
    assert(repeated != NULL && !strcmp(source, repeated));
    free(repeated);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, SYSTEM_RUID, 3, "language.opus"));
    char *linked = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY,
                                                 TEST_OVERLAY, CONTENT_RUID, 7,
                                                 &linked) == NO_ERROR);
    assert(strcmp(source, linked) == 0);
    free(linked);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_SERVE);
    assert(path != NULL && strstr(path, TEST_LIBRARY) != NULL);
    free(path);
    v3_native_cache_chapter_abort(&capture);
    v3_native_library_collection_t collection;
    assert(v3_native_library_collection_load(TEST_LIBRARY, source, TRUE,
                                              &collection) == NO_ERROR);
    assert(v3_native_library_collection_delete(TEST_LIBRARY, TEST_CACHE,
                                                collection.content_hash) == NO_ERROR);
    v3_native_library_collection_free(&collection);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(v3_native_cache_route_matches(TEST_OVERLAY, SYSTEM_RUID, 3, "language.opus"));
    free(source);
}

static void test_missing_library_object_never_uses_partial_backing(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", "second.opus", TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    assert(prepare("second.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    char *source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  TEST_OVERLAY, CONTENT_RUID,
                                                  &source) == NO_ERROR);
    v3_native_library_collection_t collection;
    assert(v3_native_library_collection_load(TEST_LIBRARY, source, TRUE,
                                              &collection) == NO_ERROR);
    assert(collection.chapter_count == 2);
    assert(fsDeleteFile(collection.chapters[0].path) == NO_ERROR);
    assert(!v3_native_cache_active_info(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                         CONTENT_RUID, NULL, NULL, NULL));
    assert(prepare("second.opus", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(path == NULL);
    v3_native_cache_chapter_abort(&capture);
    v3_native_library_collection_free(&collection);
    free(source);
}

static void test_failed_meta_preserves_other_route(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, FALSE);
    v3_native_cache_meta_capture_t capture;
    v3_native_cache_meta_observe_init(&capture, TEST_OVERLAY, SYSTEM_RUID);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    v3_native_cache_meta_capture_response(&capture, 503);
    assert(v3_native_cache_meta_capture_finish(&capture) != NO_ERROR);
    v3_native_cache_meta_capture_abort(&capture);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, SYSTEM_RUID, 7, "original.opus"));
}

static void test_identical_manifest_preserves_capture_owner(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t original, duplicate;
    char *path = NULL;
    assert(prepare("original.opus", &original, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    assert(prepare("original.opus", &duplicate, &path) == V3_NATIVE_CHAPTER_FORWARD);
    v3_native_cache_chapter_abort(&duplicate);
    finish_object(&original);
}

static void test_invalidated_meta_cannot_restore_route(void)
{
    v3_native_cache_meta_capture_t capture;
    v3_native_cache_meta_observe_init(&capture, TEST_OVERLAY, CONTENT_RUID);
    meta_body(&capture, 7, "original.opus", NULL);
    v3_native_cache_invalidate(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID);
    assert(v3_native_cache_meta_capture_finish(&capture) == ERROR_ABORTED);
    v3_native_cache_meta_capture_abort(&capture);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
}

static void numbered_manifest(unsigned number)
{
    char ruid[TB2_RUID_SIZE];
    char name[64];
    snprintf(ruid, sizeof(ruid), "%016X", number + 1U);
    snprintf(name, sizeof(name), "object-%u.opus", number);
    manifest(ruid, 7, name, NULL, FALSE);
}

static void test_manual_plan_pins_generation_until_free(void)
{
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_observe_init(&meta, TEST_OVERLAY, CONTENT_RUID);
    meta_body(&meta, 7, "original.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_download_plan_t plan;
    assert(v3_native_cache_download_plan_from_meta(&meta, &plan) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    assert(plan.object_count == 1 && plan.version == 7);
    for (unsigned i = 0; i < TEST_ROUTE_CAPACITY + 1U; i++)
        numbered_manifest(i);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(!strcmp(plan.objects[0].name, "original.opus"));
    v3_native_cache_download_plan_free(&plan);
    for (unsigned i = TEST_ROUTE_CAPACITY + 1U; i < 3U * TEST_ROUTE_CAPACITY; i++)
        numbered_manifest(i);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
}

static void test_unpinned_pool_uses_least_recent_route(void)
{
    for (unsigned i = 0; i < TEST_ROUTE_CAPACITY; i++)
        numbered_manifest(i);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("object-0.opus", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    v3_native_cache_chapter_abort(&capture);
    numbered_manifest(TEST_ROUTE_CAPACITY);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, "0000000000000001", 7,
                                          "object-0.opus"));
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, "0000000000000002", 7,
                                           "object-1.opus"));
}

static void test_auth_disambiguates_without_guessing_an_owner(void)
{
    const char *ruids[] = {CONTENT_RUID, SYSTEM_RUID};
    const char *tokens[] = {"opaque-token-one", "opaque-token-two"};
    for (size_t i = 0; i < 2; i++)
    {
        v3_native_cache_meta_capture_t meta;
        v3_native_cache_meta_observe_init(&meta, TEST_OVERLAY, ruids[i]);
        meta_body_auth(&meta, 7, "shared.opus", NULL, tokens[i]);
        assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
        v3_native_cache_meta_capture_abort(&meta);
    }
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("shared.opus", &capture, &path) == V3_NATIVE_CHAPTER_REJECT);
    for (size_t i = 0; i < 2; i++)
    {
        assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                "shared.opus", tokens[i], &capture,
                                                &path) == V3_NATIVE_CHAPTER_FORWARD);
        assert(!strcmp(capture.ruid, ruids[i]));
        v3_native_cache_chapter_abort(&capture);
    }
    v3_native_cache_chapter_action_t action = v3_native_cache_chapter_prepare(
        TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY, "shared.opus", "unmatched-token",
        &capture, &path);
    assert(action == V3_NATIVE_CHAPTER_BYPASS || action == V3_NATIVE_CHAPTER_REJECT);
    assert(path == NULL);
    v3_native_cache_chapter_abort(&capture);
}

static void test_manual_plan_resolves_shared_name_exactly(void)
{
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, CONTENT_RUID);
    meta_body(&meta, 7, "shared.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_download_plan_t plan;
    assert(v3_native_cache_download_plan_from_meta(&meta, &plan) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    manifest(SYSTEM_RUID, 3, "shared.opus", NULL, FALSE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("shared.opus", &capture, &path) == V3_NATIVE_CHAPTER_REJECT);
    assert(v3_native_cache_chapter_prepare_plan(TEST_CACHE, TEST_LIBRARY, &plan,
                                                 "shared.opus", &capture,
                                                 &path) == V3_NATIVE_CHAPTER_CAPTURE);
    assert(!strcmp(capture.ruid, CONTENT_RUID) && capture.version == 7);
    finish_object(&capture);
    v3_native_cache_download_plan_free(&plan);
}

static void test_old_completion_cannot_rollback_active_version(void)
{
    manifest(CONTENT_RUID, 7, "old.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t old, current;
    char *path = NULL;
    assert(prepare("old.opus", &old, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    manifest(CONTENT_RUID, 8, "new.opus", NULL, TRUE);
    assert(prepare("new.opus", &current, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&current);
    v3_native_cache_chapter_append(&old, "OggS", 4);
    error_t error = v3_native_cache_chapter_finish(&old);
    assert(error == NO_ERROR || error == ERROR_ABORTED);
    v3_native_cache_chapter_abort(&old);
    uint32_t version = 0;
    assert(v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID, &version));
    assert(version == 8);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 8, "new.opus"));
}

static void test_stale_abort_does_not_remove_new_capture_file(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t stale, current;
    char *path = NULL;
    assert(prepare("original.opus", &stale, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    v3_native_cache_chapter_append(&stale, "xx", 2);
    v3_native_cache_invalidate(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID);
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    assert(prepare("original.opus", &current, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    assert(stale.route_handle.serial != current.route_handle.serial);
    v3_native_cache_chapter_abort(&stale);
    finish_object(&current);
    assert(prepare("original.opus", &current, &path) == V3_NATIVE_CHAPTER_SERVE);
    assert(path != NULL);
    free(path);
    v3_native_cache_chapter_abort(&current);
}

static void test_meta_reservations_are_bounded_and_released(void)
{
    v3_native_cache_meta_capture_t pending[TEST_ROUTE_CAPACITY], overflow;
    for (unsigned i = 0; i < TEST_ROUTE_CAPACITY; i++)
    {
        char ruid[TB2_RUID_SIZE];
        snprintf(ruid, sizeof(ruid), "%016X", i + 1U);
        v3_native_cache_meta_observe_init(&pending[i], TEST_OVERLAY, ruid);
        assert(!pending[i].failed);
    }
    v3_native_cache_meta_observe_init(&overflow, TEST_OVERLAY, CONTENT_RUID);
    assert(overflow.failed);
    v3_native_cache_meta_capture_abort(&overflow);
    v3_native_cache_meta_capture_abort(&pending[0]);
    v3_native_cache_meta_observe_init(&overflow, TEST_OVERLAY, CONTENT_RUID);
    assert(!overflow.failed);
    meta_body(&overflow, 7, "original.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&overflow) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&overflow);
    for (unsigned i = 1; i < TEST_ROUTE_CAPACITY; i++)
        v3_native_cache_meta_capture_abort(&pending[i]);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
}

static void test_unknown_objects_and_other_overlay_do_not_route(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, FALSE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("unknown.opus", &capture, &path) == V3_NATIVE_CHAPTER_BYPASS);
    assert(path == NULL);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY + 1, CONTENT_RUID, 7,
                                          "original.opus"));
    v3_native_cache_chapter_abort(&capture);
}

static void test_handler_auth_is_strict_and_opaque(void)
{
    char auth[V3_NATIVE_CACHE_OBJECT_AUTH_SIZE];
    const char *value = NULL;
    const char *absent[] = {NULL, "", "xauth=wrong", "authentication=wrong", "x=1&y=2"};
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
    {
        assert(v3_native_chapter_auth_parse(absent[i], auth, &value));
        assert(value == NULL);
    }
    const char *invalid[] = {"auth", "auth=", "auth=x&auth=y", "auth=%",
                             "auth=%1", "auth=%GG", "auth=%00", "auth=%1F",
                             "auth=%7f", "auth=x&auth", "auth=x&auth="};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!v3_native_chapter_auth_parse(invalid[i], auth, &value));
    assert(v3_native_chapter_auth_parse("xauth=wrong&auth=a+b%2B%252B%3D&x=1", auth, &value));
    assert(value == auth && !strcmp(value, "a+b+%2B="));
    char long_query[V3_NATIVE_CACHE_OBJECT_AUTH_SIZE + sizeof("auth=")];
    strcpy(long_query, "auth=");
    memset(long_query + 5, 'x', V3_NATIVE_CACHE_OBJECT_AUTH_SIZE - 1U);
    long_query[5 + V3_NATIVE_CACHE_OBJECT_AUTH_SIZE - 1U] = '\0';
    assert(v3_native_chapter_auth_parse(long_query, auth, &value));
    assert(strlen(value) == V3_NATIVE_CACHE_OBJECT_AUTH_SIZE - 1U);
    long_query[5 + V3_NATIVE_CACHE_OBJECT_AUTH_SIZE - 1U] = 'x';
    long_query[5 + V3_NATIVE_CACHE_OBJECT_AUTH_SIZE] = '\0';
    assert(!v3_native_chapter_auth_parse(long_query, auth, &value));
    const char *token = "a+b/c==&percent% token";
    char *query = v3_native_chapter_auth_query(token);
    assert(query != NULL);
    assert(!strcmp(query, "auth=a%2Bb%2Fc%3D%3D%26percent%25%20token"));
    assert(v3_native_chapter_auth_parse(query, auth, &value));
    assert(!strcmp(value, token));
    free(query);
}

static void test_handler_preserves_source_and_nocloud_authority(void)
{
    assert(v3_native_original_content_allowed(&settings, CONTENT_RUID, FALSE));
    assert(v3_native_original_content_allowed(&settings, CONTENT_RUID, TRUE));
    policy_content.json.nocloud = TRUE;
    assert(v3_native_original_content_allowed(&settings, CONTENT_RUID, FALSE));
    assert(!v3_native_original_content_allowed(&settings, CONTENT_RUID, TRUE));
    policy_content.json.cloud_override = TRUE;
    assert(v3_native_original_content_allowed(&settings, CONTENT_RUID, TRUE));
    policy_content.json.source = "lib://assigned-source";
    assert(!v3_native_original_content_allowed(&settings, CONTENT_RUID, FALSE));
    assert(!v3_native_original_content_allowed(&settings, CONTENT_RUID, TRUE));
    assert(policy_reads == policy_frees);
    policy_missing = TRUE;
    assert(!v3_native_original_content_allowed(&settings, CONTENT_RUID, FALSE));
    assert(!v3_native_original_content_allowed(&settings, CONTENT_RUID, TRUE));
    assert(policy_reads == policy_frees + 2U);
    assert(!v3_native_original_content_allowed(NULL, CONTENT_RUID, TRUE));
    assert(!v3_native_original_content_allowed(&settings, NULL, TRUE));
    assert(!v3_native_original_content_allowed(&settings, "", TRUE));
}

static void test_storage_failure_keeps_only_observed_mapping(void)
{
    FILE *blocked_root = fopen(TEST_CACHE, "wb");
    assert(blocked_root != NULL && fclose(blocked_root) == 0);
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, CONTENT_RUID);
    meta_body(&meta, 7, "original.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&meta) != NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(!v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID, NULL));
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(path == NULL && capture.file == NULL);
    v3_native_cache_chapter_abort(&capture);
}

static void test_evicted_staging_identity_cannot_be_replaced(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    for (unsigned i = 0; i < TEST_ROUTE_CAPACITY; i++)
        numbered_manifest(i);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, CONTENT_RUID);
    meta_body(&meta, 7, "contradiction.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&meta) == ERROR_INVALID_FILE);
    v3_native_cache_meta_capture_abort(&meta);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "contradiction.opus"));
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
}

static void test_partial_response_does_not_activate_generation(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_chapter_cbr_t context = {0};
    char *path = NULL;
    assert(prepare("original.opus", &context.cache, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    context.cache_enabled = TRUE;
    HttpClientContext cloud = {0};
    cloud.statusCode = 206;
    v3_native_chapter_response(&context, &cloud);
    assert(context.response_successful && context.cache.failed);
    v3_native_chapter_body(&context, &cloud, "Og", 2, NO_ERROR);
    v3_native_chapter_body(&context, &cloud, NULL, 0, ERROR_END_OF_STREAM);
    assert(context.finished && context.successful);
    assert(context.cache_error != NO_ERROR && imports == 0 && transport_responses == 0);
    v3_native_cache_chapter_abort(&context.cache);
    assert(!v3_native_cache_active_version(TEST_CACHE, TEST_OVERLAY, CONTENT_RUID, NULL));
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(prepare("original.opus", &context.cache, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&context.cache);
}

static void test_sequential_versions_do_not_exhaust_unpinned_pool(void)
{
    char name[64];
    for (unsigned i = 1; i <= TEST_ROUTE_CAPACITY + 2U; i++)
    {
        snprintf(name, sizeof(name), "version-%u.opus", i);
        manifest(CONTENT_RUID, i, name, NULL, FALSE);
    }
    assert(v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID,
                                          TEST_ROUTE_CAPACITY + 2U, name));
}

static void test_observed_generation_can_enable_storage(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, FALSE);
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
}

static void test_repaired_storage_retries_identical_manifest(void)
{
    test_storage_failure_keeps_only_observed_mapping();
    assert(fsDeleteFile(TEST_CACHE) == NO_ERROR);
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
}

static void test_shared_library_keeps_two_overlay_origins(void)
{
    const uint8_t other_overlay = TEST_OVERLAY + 1U;
    const char *other_ruid = "DC467027500304E0";
    complete_generation(CONTENT_RUID, 7, "original.opus");
    char *first_source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  TEST_OVERLAY, CONTENT_RUID,
                                                  &first_source) == NO_ERROR);
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       other_overlay, other_ruid);
    meta_body(&meta, 7, "renamed.opus", NULL);
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, other_overlay,
                                            "renamed.opus", NULL, &capture,
                                            &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    char *second_source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  other_overlay, other_ruid,
                                                  &second_source) == NO_ERROR);
    assert(first_source != NULL && second_source != NULL);
    assert(!strcmp(first_source, second_source));
    v3_native_library_collection_t collection;
    assert(v3_native_library_collection_load(TEST_LIBRARY, first_source, TRUE,
                                              &collection) == NO_ERROR);
    assert(collection.origin_count == 2 && collection.chapter_count == 1);
    bool_t first_origin = FALSE, second_origin = FALSE;
    for (size_t i = 0; i < collection.origin_count; i++)
    {
        const v3_native_library_origin_t *origin = &collection.origins[i];
        first_origin |= origin->overlay_id == TEST_OVERLAY &&
                        !strcmp(origin->ruid, CONTENT_RUID) && origin->content_version == 7;
        second_origin |= origin->overlay_id == other_overlay &&
                         !strcmp(origin->ruid, other_ruid) && origin->content_version == 7;
    }
    assert(first_origin && second_origin);
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_SERVE);
    char *other_path = NULL;
    v3_native_cache_chapter_abort(&capture);
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, other_overlay,
                                            "renamed.opus", NULL, &capture,
                                            &other_path) == V3_NATIVE_CHAPTER_SERVE);
    assert(path != NULL && other_path != NULL && !strcmp(path, other_path));
    free(path);
    free(other_path);
    v3_native_cache_chapter_abort(&capture);
    assert(v3_native_library_collection_delete(TEST_LIBRARY, TEST_CACHE,
                                                collection.content_hash) == NO_ERROR);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    assert(!v3_native_cache_route_matches(other_overlay, other_ruid, 7, "renamed.opus"));
    v3_native_library_collection_free(&collection);
    free(first_source);
    free(second_source);
}

static void test_library_import_preserves_pinned_serve_path(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    v3_native_cache_chapter_capture_t serving;
    char *path = NULL;
    assert(prepare("original.opus", &serving, &path) == V3_NATIVE_CHAPTER_SERVE);
    assert(path != NULL && fsFileExists(path));
    char *source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY,
                                                  TEST_OVERLAY, CONTENT_RUID,
                                                  &source) == NO_ERROR);
    assert(fsFileExists(path));
    v3_native_cache_chapter_abort(&serving);
    assert(!fsFileExists(path));
    free(path);
    free(source);
}

static void test_import_requires_requested_active_version(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    char *source = NULL;
    assert(v3_native_cache_import_active_library_version(TEST_CACHE, TEST_LIBRARY,
                                                          TEST_OVERLAY, CONTENT_RUID,
                                                          8, &source) == ERROR_INVALID_FILE);
    assert(source == NULL);
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 CONTENT_RUID, 7, &source) != NO_ERROR);
    assert(v3_native_cache_import_active_library_version(TEST_CACHE, TEST_LIBRARY,
                                                          TEST_OVERLAY, CONTENT_RUID,
                                                          7, &source) == NO_ERROR);
    free(source);

    const char *game_ruid = "DC467027500304E0";
    const char *game_manifest = "{\"version\":9,\"contentType\":\"tonieplay\","
                                "\"content\":[{\"name\":\"game.json\",\"type\":\"json\","
                                "\"auth\":\"assigned-auth\","
                                "\"filename\":\"game.json\",\"fileSize\":4}]}";
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, game_ruid);
    v3_native_cache_meta_capture_response(&meta, 200);
    v3_native_cache_meta_capture_append(&meta, game_manifest, strlen(game_manifest));
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("game.json", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    assert(v3_native_cache_import_active_tonieplay_library_version(
               TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY, game_ruid, 10) == ERROR_INVALID_FILE);
    assert(v3_native_cache_import_active_tonieplay_library_version(
               TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY, game_ruid, 9) == NO_ERROR);
    assert(v3_native_cache_active_is_tonieplay(TEST_CACHE, TEST_LIBRARY,
                                                TEST_OVERLAY, game_ruid));
}

static void test_evicted_incomplete_generation_can_be_repaired(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_SERVE);
    v3_native_cache_chapter_abort(&capture);
    assert(fsDeleteFile(path) == NO_ERROR);
    free(path);
    for (unsigned i = 0; i < TEST_ROUTE_CAPACITY; i++)
        numbered_manifest(i);
    assert(!v3_native_cache_route_matches(TEST_OVERLAY, CONTENT_RUID, 7, "original.opus"));
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    assert(v3_native_cache_active_info(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                        CONTENT_RUID, NULL, NULL, NULL));
}

static void test_assigned_and_system_object_auth_disambiguation(void)
{
    test_import_requires_requested_active_version();
    const char *game_ruid = "DC467027500304E0";
    char *game_source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &game_source) == NO_ERROR);
    v3_native_cache_invalidate_routes(TEST_OVERLAY, game_ruid);
    uint8_t *manifest_data = NULL;
    size_t manifest_length = 0;
    assert(v3_tonieplay_library_activate(TEST_LIBRARY, game_source, TEST_OVERLAY,
                                          game_ruid, 9, &manifest_data,
                                          &manifest_length) == NO_ERROR);
    free(manifest_data);
    char *path = NULL, *source = NULL;
    char type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE], ruid[TB2_RUID_SIZE];
    bool_t ambiguous = FALSE;
    assert(v3_tonieplay_library_resolve_checked(TEST_OVERLAY, "game.json", NULL,
                                                 &path, type, ruid, &source, &ambiguous));
    assert(!ambiguous && !strcmp(ruid, game_ruid));
    free(path);
    free(source);
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_observe_init(&meta, TEST_OVERLAY, SYSTEM_RUID);
    meta_body_auth(&meta, 3, "game.json", NULL, "system-auth");
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_meta_capture_abort(&meta);
    assert(!v3_tonieplay_library_resolve_checked(TEST_OVERLAY, "game.json", "system-auth",
                                                  &path, type, ruid, &source, &ambiguous));
    assert(!ambiguous && path == NULL && source == NULL);
    v3_native_cache_chapter_capture_t capture;
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                            "game.json", "system-auth", &capture,
                                            &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(!strcmp(capture.ruid, SYSTEM_RUID));
    v3_native_cache_chapter_abort(&capture);
    assert(v3_tonieplay_library_resolve_checked(TEST_OVERLAY, "game.json", "assigned-auth",
                                                 &path, type, ruid, &source, &ambiguous));
    assert(!ambiguous && !strcmp(ruid, game_ruid) && !strcmp(source, game_source));
    free(path);
    free(source);
    const char *unknown[] = {NULL, "wrong-auth"};
    for (size_t i = 0; i < 2; i++)
    {
        assert(!v3_tonieplay_library_resolve_checked(TEST_OVERLAY, "game.json", unknown[i],
                                                      &path, type, ruid, &source, &ambiguous));
        assert(ambiguous && path == NULL && source == NULL);
    }
    free(game_source);
}

static void test_auth_refresh_reuses_persisted_staging(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, TRUE);
    v3_native_cache_invalidate_routes(TEST_OVERLAY, CONTENT_RUID);
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, CONTENT_RUID);
    const char *new_auth = "opaque-token.lastc-23-1791216979";
    meta_body_auth(&meta, 7, "original.opus", NULL, new_auth);
    assert(v3_native_cache_meta_capture_finish(&meta) == NO_ERROR);
    v3_native_cache_download_plan_t plan;
    assert(v3_native_cache_download_plan_from_meta(&meta, &plan) == NO_ERROR);
    assert(!strcmp(plan.objects[0].auth, new_auth));
    v3_native_cache_meta_capture_abort(&meta);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(prepare("original.opus", &capture, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&capture);
    v3_native_cache_download_plan_free(&plan);
    uint8_t *raw = NULL;
    size_t length = 0;
    uint32_t version = 0;
    assert(v3_native_cache_read_active_manifest(TEST_CACHE, TEST_LIBRARY,
                                                 TEST_OVERLAY, CONTENT_RUID,
                                                 &raw, &length, &version) == NO_ERROR);
    assert(strstr((char *)raw, new_auth) != NULL);
    free(raw);
}

static char *read_test_file(const char *path)
{
    uint32_t size = 0;
    assert(fsGetFileSize(path, &size) == NO_ERROR);
    char *data = calloc(size + 1U, 1);
    FILE *file = fopen(path, "rb");
    assert(data != NULL && file != NULL);
    assert(fread(data, 1, size, file) == size && fclose(file) == 0);
    return data;
}

static error_t refresh_audio_auth(const char *auth, const char *second_name, bool store)
{
    v3_native_cache_meta_capture_t meta;
    if (store)
        v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                           TEST_OVERLAY, CONTENT_RUID);
    else
        v3_native_cache_meta_observe_init(&meta, TEST_OVERLAY, CONTENT_RUID);
    meta_body_auth(&meta, 7, "original.opus", second_name, auth);
    error_t error = v3_native_cache_meta_capture_finish(&meta);
    v3_native_cache_meta_capture_abort(&meta);
    return error;
}

static void test_auth_refresh_keeps_capture_and_manual_plan(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", "second.opus", TRUE);
    v3_native_cache_download_plan_t old_plan, new_plan;
    assert(v3_native_cache_download_plan_get(TEST_OVERLAY, CONTENT_RUID, &old_plan) == NO_ERROR);
    v3_native_cache_chapter_capture_t first, second, duplicate;
    char *path = NULL;
    assert(prepare("original.opus", &first, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    finish_object(&first);
    assert(prepare("second.opus", &second, &path) == V3_NATIVE_CHAPTER_CAPTURE);
    v3_native_cache_chapter_append(&second, "Og", 2);
    assert(refresh_audio_auth("refreshed-auth", "second.opus", TRUE) == NO_ERROR);
    assert(v3_native_cache_download_plan_get(TEST_OVERLAY, CONTENT_RUID, &new_plan) == NO_ERROR);
    assert(old_plan.route_handle.serial == new_plan.route_handle.serial);
    assert(!strcmp(old_plan.objects[0].auth, "opaque-token"));
    assert(!strcmp(new_plan.objects[0].auth, "refreshed-auth"));
    assert(v3_native_cache_chapter_prepare_plan(TEST_CACHE, TEST_LIBRARY, &old_plan,
                                                 "original.opus", &first, &path) == V3_NATIVE_CHAPTER_STAGED);
    v3_native_cache_chapter_abort(&first);
    assert(prepare("second.opus", &duplicate, &path) == V3_NATIVE_CHAPTER_FORWARD);
    v3_native_cache_chapter_abort(&duplicate);
    assert(fsFileExists(second.temp_path));
    v3_native_cache_chapter_append(&second, "gS", 2);
    assert(v3_native_cache_chapter_finish(&second) == NO_ERROR);
    v3_native_cache_chapter_abort(&second);
    assert(v3_native_cache_active_info(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                        CONTENT_RUID, NULL, NULL, NULL));
    v3_native_cache_download_plan_free(&old_plan);
    v3_native_cache_download_plan_free(&new_plan);
}

static void test_auth_refresh_preserves_order_and_exact_resolution(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", NULL, FALSE);
    manifest(SYSTEM_RUID, 7, "original.opus", NULL, FALSE);
    v3_native_cache_meta_capture_t delayed;
    v3_native_cache_meta_observe_init(&delayed, TEST_OVERLAY, CONTENT_RUID);
    meta_body_auth(&delayed, 7, "original.opus", NULL, "late-old-auth");
    char auth[V3_NATIVE_CACHE_OBJECT_AUTH_SIZE];
    memset(auth, 'x', sizeof(auth) - 1U);
    auth[sizeof(auth) - 1U] = '\0';
    assert(refresh_audio_auth(auth, NULL, FALSE) == NO_ERROR);
    assert(v3_native_cache_meta_capture_finish(&delayed) == ERROR_ABORTED);
    v3_native_cache_meta_capture_abort(&delayed);
    v3_native_cache_chapter_capture_t capture;
    char *path = NULL;
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                            "original.opus", auth, &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(!strcmp(capture.ruid, CONTENT_RUID));
    v3_native_cache_chapter_abort(&capture);
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                            "original.opus", "opaque-token", &capture, &path) == V3_NATIVE_CHAPTER_FORWARD);
    assert(!strcmp(capture.ruid, SYSTEM_RUID));
    v3_native_cache_chapter_abort(&capture);
    assert(v3_native_cache_chapter_prepare(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                            "original.opus", "late-old-auth", &capture, &path) == V3_NATIVE_CHAPTER_REJECT);
    assert(!fsDirExists(TEST_CACHE));
}

static void test_auth_refresh_still_rejects_content_changes(void)
{
    manifest(CONTENT_RUID, 7, "original.opus", "second.opus", TRUE);
    const char *path = TEST_CACHE "/v3-native/staging/5/" CONTENT_RUID "/7/manifest.json";
    char *original = read_test_file(path);
    enum {CHANGE_NAME, CHANGE_SIZE, CHANGE_TYPE, CHANGE_FILENAME,
          CHANGE_CONTENT_TYPE, CHANGE_COUNT, CHANGE_ORDER, CHANGE_COUNT_TOTAL};
    for (unsigned change = 0; change < CHANGE_COUNT_TOTAL; change++)
    {
        cJSON *root = cJSON_Parse(original);
        assert(root != NULL);
        cJSON *content = cJSON_GetObjectItem(root, "content");
        cJSON *first = cJSON_GetArrayItem(content, 0);
        assert(cJSON_ReplaceItemInObject(first, "auth", cJSON_CreateString("fresh-auth")));
        switch (change)
        {
        case CHANGE_NAME:
            assert(cJSON_ReplaceItemInObject(first, "name", cJSON_CreateString("other.opus")));
            break;
        case CHANGE_SIZE:
            assert(cJSON_ReplaceItemInObject(first, "fileSize", cJSON_CreateNumber(5)));
            break;
        case CHANGE_TYPE:
            assert(cJSON_ReplaceItemInObject(first, "type", cJSON_CreateString("data")));
            break;
        case CHANGE_FILENAME:
            assert(cJSON_AddStringToObject(first, "filename", "changed.opus"));
            break;
        case CHANGE_CONTENT_TYPE:
            assert(cJSON_AddStringToObject(root, "contentType", "tonieplay"));
            break;
        case CHANGE_COUNT:
            cJSON_DeleteItemFromArray(content, 1);
            break;
        case CHANGE_ORDER:
            assert(cJSON_AddItemToArray(content, cJSON_DetachItemFromArray(content, 0)));
            break;
        }
        char *raw = cJSON_PrintUnformatted(root);
        assert(raw != NULL);
        v3_native_cache_meta_capture_t meta;
        v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                           TEST_OVERLAY, CONTENT_RUID);
        v3_native_cache_meta_capture_response(&meta, 200);
        v3_native_cache_meta_capture_append(&meta, raw, strlen(raw));
        assert(v3_native_cache_meta_capture_finish(&meta) == ERROR_INVALID_FILE);
        v3_native_cache_meta_capture_abort(&meta);
        char *unchanged = read_test_file(path);
        assert(!strcmp(original, unchanged));
        free(unchanged);
        cJSON_free(raw);
        cJSON_Delete(root);
    }
    v3_native_cache_download_plan_t plan;
    assert(v3_native_cache_download_plan_get(TEST_OVERLAY, CONTENT_RUID, &plan) == NO_ERROR);
    assert(!strcmp(plan.objects[0].auth, "opaque-token"));
    v3_native_cache_download_plan_free(&plan);
    free(original);
}

static void test_auth_refresh_retries_storage_without_changing_library(void)
{
    complete_generation(CONTENT_RUID, 7, "original.opus");
    char *source = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                  CONTENT_RUID, &source) == NO_ERROR);
    const char *manifest_path = TEST_CACHE "/v3-native/versions/5/" CONTENT_RUID "/7/manifest.json";
    const char *descriptor_path = TEST_CACHE "/v3-native/versions/5/" CONTENT_RUID "/7/descriptor.json";
    const char *marker_path = TEST_CACHE "/v3-native/active/5/" CONTENT_RUID ".json";
    const char *blocked_part = TEST_CACHE "/v3-native/versions/5/" CONTENT_RUID "/7/manifest.json.part";
    char *before = read_test_file(manifest_path);
    char *descriptor = read_test_file(descriptor_path);
    char *marker = read_test_file(marker_path);
    assert(fsCreateDir(blocked_part) == NO_ERROR);
    /* POSIX remove() can remove an empty directory: keep it nonempty so the
     * production atomic writer cannot delete its deliberately blocked path. */
    const char *blocker_path = TEST_CACHE "/v3-native/versions/5/" CONTENT_RUID "/7/manifest.json.part/blocker";
    FILE *blocker = fopen(blocker_path, "wb");
    assert(blocker != NULL);
    assert(fclose(blocker) == 0);
    assert(refresh_audio_auth("new-auth.lastc-23-1791216979", NULL, TRUE) != NO_ERROR);
    char *after = read_test_file(manifest_path);
    assert(!strcmp(before, after));
    free(after);
    /* Read-only status and actual local delivery must not replace live auth
     * with the older manifest left on disk by the failed atomic write. */
    assert(v3_native_cache_active_info(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                        CONTENT_RUID, NULL, NULL, NULL));
    v3_native_cache_route_handle_t handle;
    uint8_t *data = NULL;
    size_t length = 0;
    uint32_t version = 0;
    assert(v3_native_cache_open_active_manifest(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 CONTENT_RUID, &data, &length, &version, &handle) == NO_ERROR);
    free(data);
    v3_native_cache_route_release(&handle);
    v3_native_cache_download_plan_t plan;
    assert(v3_native_cache_download_plan_get(TEST_OVERLAY, CONTENT_RUID, &plan) == NO_ERROR);
    assert(!strcmp(plan.objects[0].auth, "new-auth.lastc-23-1791216979"));
    v3_native_cache_download_plan_free(&plan);
    assert(fsDeleteFile(blocker_path) == NO_ERROR);
    assert(fsRemoveDir(blocked_part) == NO_ERROR);
    assert(refresh_audio_auth("new-auth.lastc-23-1791216979", NULL, TRUE) == NO_ERROR);
    after = read_test_file(manifest_path);
    assert(strstr(after, "new-auth.lastc-23-1791216979") != NULL);
    free(after);
    after = read_test_file(descriptor_path);
    assert(!strcmp(descriptor, after));
    free(after);
    after = read_test_file(marker_path);
    assert(!strcmp(marker, after));
    free(after);
    char *reimported = NULL;
    assert(v3_native_cache_import_active_library(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                  CONTENT_RUID, &reimported) == NO_ERROR);
    assert(!strcmp(source, reimported));
    v3_native_cache_invalidate_routes(TEST_OVERLAY, CONTENT_RUID);
    assert(v3_native_cache_open_active_manifest(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 CONTENT_RUID, &data, &length, &version, &handle) == NO_ERROR);
    assert(strstr((char *)data, "new-auth.lastc-23-1791216979") != NULL);
    free(data);
    v3_native_cache_route_release(&handle);
    free(before);
    free(descriptor);
    free(marker);
    free(source);
    free(reimported);
}

static error_t refresh_game_manifest(const uint8_t *manifest_data, size_t length,
                                      const char *auth)
{
    cJSON *manifest_json = cJSON_ParseWithLength((const char *)manifest_data, length);
    assert(manifest_json != NULL);
    cJSON *object = cJSON_GetArrayItem(cJSON_GetObjectItem(manifest_json, "content"), 0);
    assert(cJSON_ReplaceItemInObject(object, "auth", cJSON_CreateString(auth)));
    char *raw = cJSON_PrintUnformatted(manifest_json);
    assert(raw != NULL);
    v3_native_cache_meta_capture_t meta;
    v3_native_cache_meta_capture_init(&meta, TEST_CACHE, TEST_LIBRARY,
                                       TEST_OVERLAY, "DC467027500304E0");
    v3_native_cache_meta_capture_response(&meta, 200);
    v3_native_cache_meta_capture_append(&meta, raw, strlen(raw));
    error_t error = v3_native_cache_meta_capture_finish(&meta);
    v3_native_cache_meta_capture_abort(&meta);
    cJSON_Delete(manifest_json);
    cJSON_free(raw);
    return error;
}

static void test_tonieplay_auth_refresh_imports_from_immutable_library(void)
{
    test_import_requires_requested_active_version();
    const char *game_ruid = "DC467027500304E0";
    char *original_source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &original_source) == NO_ERROR);
    v3_tonieplay_library_collection_t original, reloaded, refreshed;
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, original_source, TRUE, &original) == NO_ERROR);
    assert(!fsFileExists(TEST_CACHE "/v3-native/versions/5/DC467027500304E0/9/chapters/game.json"));
    assert(refresh_game_manifest(original.manifest, original.manifest_length, "new-game-auth") == NO_ERROR);
    assert(v3_native_cache_import_active_tonieplay_library_version(TEST_CACHE, TEST_LIBRARY,
                                                                    TEST_OVERLAY, game_ruid, 9) == NO_ERROR);
    char *new_source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &new_source) == NO_ERROR);
    assert(strcmp(new_source, original_source));
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, original_source, TRUE, &reloaded) == NO_ERROR);
    assert(original.manifest_length == reloaded.manifest_length &&
           !memcmp(original.manifest, reloaded.manifest, original.manifest_length));
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, new_source, TRUE, &refreshed) == NO_ERROR);
    assert(strstr((char *)refreshed.manifest, "new-game-auth") != NULL);
    assert(!strcmp(original.objects[0].sha256, refreshed.objects[0].sha256));
    v3_tonieplay_library_collection_free(&original);
    v3_tonieplay_library_collection_free(&reloaded);
    v3_tonieplay_library_collection_free(&refreshed);
    free(original_source);
    free(new_source);
}

static const uint8_t *racing_game_manifest;
static size_t racing_game_manifest_length;

static void refresh_game_before_library_link(void)
{
    assert(refresh_game_manifest(racing_game_manifest, racing_game_manifest_length,
                                    "third-game-auth") == NO_ERROR);
}

static void test_tonieplay_auth_refresh_checks_manifest_before_linking(void)
{
    test_import_requires_requested_active_version();
    const char *game_ruid = "DC467027500304E0";
    const char *descriptor_path = TEST_CACHE "/v3-native/versions/5/DC467027500304E0/9/descriptor.json";
    char *original_source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &original_source) == NO_ERROR);
    v3_tonieplay_library_collection_t original, latest;
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, original_source, TRUE, &original) == NO_ERROR);
    char *descriptor = read_test_file(descriptor_path);
    assert(refresh_game_manifest(original.manifest, original.manifest_length, "second-game-auth") == NO_ERROR);
    racing_game_manifest = original.manifest;
    racing_game_manifest_length = original.manifest_length;
    /* Deterministic interleaving after archive publication, before linking. */
    library_release_hook = refresh_game_before_library_link;
    assert(v3_native_cache_import_active_tonieplay_library_version(TEST_CACHE, TEST_LIBRARY,
                                                                    TEST_OVERLAY, game_ruid, 9) == ERROR_ABORTED);
    assert(library_release_hook == NULL);
    char *unchanged = read_test_file(descriptor_path);
    assert(!strcmp(descriptor, unchanged));
    free(unchanged);
    assert(v3_native_cache_import_active_tonieplay_library_version(TEST_CACHE, TEST_LIBRARY,
                                                                    TEST_OVERLAY, game_ruid, 9) == NO_ERROR);
    char *latest_source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &latest_source) == NO_ERROR);
    assert(strcmp(latest_source, original_source));
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, latest_source, TRUE, &latest) == NO_ERROR);
    assert(strstr((char *)latest.manifest, "third-game-auth") != NULL);
    v3_tonieplay_library_collection_free(&original);
    v3_tonieplay_library_collection_free(&latest);
    free(original_source);
    free(latest_source);
    free(descriptor);
}

static void test_tonieplay_auth_refresh_rejects_damaged_library_backing(void)
{
    test_import_requires_requested_active_version();
    const char *game_ruid = "DC467027500304E0";
    const char *descriptor_path = TEST_CACHE "/v3-native/versions/5/DC467027500304E0/9/descriptor.json";
    char *source = NULL;
    assert(v3_native_cache_active_library_source(TEST_CACHE, TEST_LIBRARY, TEST_OVERLAY,
                                                 game_ruid, 9, &source) == NO_ERROR);
    v3_tonieplay_library_collection_t original;
    assert(v3_tonieplay_library_collection_load(TEST_LIBRARY, source, TRUE, &original) == NO_ERROR);
    char *descriptor = read_test_file(descriptor_path);
    assert(refresh_game_manifest(original.manifest, original.manifest_length, "new-game-auth") == NO_ERROR);
    FILE *object = fopen(original.objects[0].path, "wb");
    assert(object != NULL);
    assert(fwrite("bad!", 1, 4, object) == 4 && fclose(object) == 0);
    assert(v3_native_cache_import_active_tonieplay_library_version(TEST_CACHE, TEST_LIBRARY,
                                                                    TEST_OVERLAY, game_ruid, 9) == ERROR_INVALID_FILE);
    char *unchanged = read_test_file(descriptor_path);
    assert(!strcmp(descriptor, unchanged));
    free(unchanged);
    free(descriptor);
    v3_tonieplay_library_collection_free(&original);
    free(source);
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*run)(void); } cases[] = {
        {"interleaved", test_interleaved_content_and_system_manifest},
        {"duplicate-abort", test_duplicate_abort_keeps_original_capture},
        {"stored-interleaving", test_stored_interleaving_completes_both_generations},
        {"readonly-info", test_active_info_does_not_rollback_live_route},
        {"meta-order", test_out_of_order_meta_is_not_published},
        {"invalidate-capture", test_invalidation_prevents_late_capture_activation},
        {"ambiguous-name", test_ambiguous_object_name_is_rejected},
        {"library-import", test_library_import_keeps_unrelated_route},
        {"incomplete-library", test_missing_library_object_never_uses_partial_backing},
        {"failed-meta", test_failed_meta_preserves_other_route},
        {"same-generation", test_identical_manifest_preserves_capture_owner},
        {"invalidate-meta", test_invalidated_meta_cannot_restore_route},
        {"plan-pin", test_manual_plan_pins_generation_until_free},
        {"pool-lru", test_unpinned_pool_uses_least_recent_route},
        {"bounded-reservations", test_meta_reservations_are_bounded_and_released},
        {"unknown-and-overlay", test_unknown_objects_and_other_overlay_do_not_route},
        {"auth-collision", test_auth_disambiguates_without_guessing_an_owner},
        {"manual-shared-name", test_manual_plan_resolves_shared_name_exactly},
        {"late-version", test_old_completion_cannot_rollback_active_version},
        {"stale-abort", test_stale_abort_does_not_remove_new_capture_file},
        {"handler-auth", test_handler_auth_is_strict_and_opaque},
        {"handler-authority", test_handler_preserves_source_and_nocloud_authority},
        {"storage-failure", test_storage_failure_keeps_only_observed_mapping},
        {"persisted-identity", test_evicted_staging_identity_cannot_be_replaced},
        {"partial-response", test_partial_response_does_not_activate_generation},
        {"sequential-versions", test_sequential_versions_do_not_exhaust_unpinned_pool},
        {"observe-to-store", test_observed_generation_can_enable_storage},
        {"storage-retry", test_repaired_storage_retries_identical_manifest},
        {"shared-library", test_shared_library_keeps_two_overlay_origins},
        {"pinned-serve-path", test_library_import_preserves_pinned_serve_path},
        {"import-version", test_import_requires_requested_active_version},
        {"repair-incomplete", test_evicted_incomplete_generation_can_be_repaired},
        {"assigned-system-auth", test_assigned_and_system_object_auth_disambiguation},
        {"auth-refresh-staging", test_auth_refresh_reuses_persisted_staging},
        {"auth-refresh-capture", test_auth_refresh_keeps_capture_and_manual_plan},
        {"auth-refresh-order", test_auth_refresh_preserves_order_and_exact_resolution},
        {"auth-refresh-identity", test_auth_refresh_still_rejects_content_changes},
        {"auth-refresh-storage", test_auth_refresh_retries_storage_without_changing_library},
        {"auth-refresh-tonieplay", test_tonieplay_auth_refresh_imports_from_immutable_library},
        {"auth-refresh-library-race", test_tonieplay_auth_refresh_checks_manifest_before_linking},
        {"auth-refresh-library-corrupt", test_tonieplay_auth_refresh_rejects_damaged_library_backing},
    };
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        if (!strcmp(argv[1], "--list"))
            puts(cases[i].name);
        else if (!strcmp(argv[1], cases[i].name))
        {
            cases[i].run();
            for (size_t j = 0; j < MUTEX_LAST; j++)
                assert(held[j] == 0);
            printf("PASS %s\n", cases[i].name);
            return 0;
        }
    }
    assert(!strcmp(argv[1], "--list"));
    return 0;
}
