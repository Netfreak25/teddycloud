/* Real metadata module, filesystem, JSON, hashes and Ogg/Opus parser.
 * Only the cache snapshot/catalog lookup and failure boundaries are controlled. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ogg/ogg.h>
#include "settings.h"
#include "mutex_manager.h"
#include "v3_local_content.h"
#undef TRACE_LEVEL
#include "../src/v3_playlist_metadata.c"

static settings_t fixture_settings;
static unsigned held[MUTEX_LAST];
static unsigned failure_stage, scans;
static bool_t snapshot_enabled, missing_last;
static bool_t observe_during_scan;
static uint32_t snapshot_version;
static size_t snapshot_count;
static uint32_t catalog_version;
static toniesJson_item_t catalog;
static const char ruid[] = "1234567890ABCDEF";
static char *manifest_fixture(uint32_t version, size_t count, const char *title);

void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *data) { free(data); }
void osSuspendAllTasks(void) {}
void osResumeAllTasks(void) {}
settings_t *get_settings(void) { return &fixture_settings; }
char *error2text(error_t error) { (void)error; return "fixture error"; }
void mutex_lock(mutex_id_t id) { assert(id < MUTEX_LAST && !held[id]); held[id]++; }
void mutex_unlock(mutex_id_t id) { assert(id < MUTEX_LAST && held[id] == 1); held[id]--; }

error_t __real_fsWriteFile(FsFile *file, void *data, size_t length);
error_t __wrap_fsWriteFile(FsFile *file, void *data, size_t length)
{ return failure_stage == 1 ? ERROR_WRITE_FAILED : __real_fsWriteFile(file, data, length); }
error_t __real_fsFlushFile(FsFile *file);
error_t __wrap_fsFlushFile(FsFile *file)
{ return failure_stage == 2 ? ERROR_WRITE_FAILED : __real_fsFlushFile(file); }
error_t __real_fsRenameFile(const char_t *from, const char_t *to);
error_t __wrap_fsRenameFile(const char_t *from, const char_t *to)
{ return failure_stage == 3 ? ERROR_WRITE_FAILED : __real_fsRenameFile(from, to); }
error_t __real_encoder_read_opus_metadata(const char *, double *, char *, size_t);
error_t __wrap_encoder_read_opus_metadata(const char *path, double *duration, char *title, size_t capacity)
{
    assert(!held[MUTEX_V3_PLAYLIST_METADATA] && !held[MUTEX_V3_NATIVE_LIBRARY]);
    scans++;
    if (observe_during_scan)
    {
        observe_during_scan = FALSE;
        char *raw = manifest_fixture(snapshot_version, snapshot_count, "Late manifest title");
        cJSON *manifest = cJSON_Parse(raw); free(raw);
        cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(manifest, "content"), 0);
        cJSON_AddStringToObject(first, "title", "Late chapter title");
        raw = cJSON_PrintUnformatted(manifest); cJSON_Delete(manifest);
        assert(v3_playlist_metadata_observe(&fixture_settings, ruid, (uint8_t *)raw, strlen(raw)) == NO_ERROR);
        free(raw);
    }
    return __real_encoder_read_opus_metadata(path, duration, title, capacity);
}

toniesJson_item_t *tonies_byAudioIdTrackCountUnique(uint32_t version, size_t count)
{ return version == catalog_version && count == catalog.tracks_count ? &catalog : NULL; }

error_t v3_local_content_generation_load(const char *root, uint8_t overlay,
    const char *tag, uint32_t version, v3_local_content_generation_t *generation)
{
    (void)root; (void)overlay; (void)tag; (void)version; (void)generation;
    assert(!"native metadata must not borrow an unrelated TAF/TAP generation");
    return ERROR_NOT_FOUND;
}
void v3_local_content_generation_free(v3_local_content_generation_t *generation)
{ assert(generation->chapters == NULL); }

static char *manifest_fixture(uint32_t version, size_t count, const char *title)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "version", version);
    if (title) cJSON_AddStringToObject(json, "title", title);
    cJSON_AddStringToObject(json, "auth", "must-not-be-persisted");
    cJSON *content = cJSON_AddArrayToObject(json, "content");
    for (size_t i = 0; i < count; i++)
    {
        char name[64]; snprintf(name, sizeof(name), "chapter-%zu.opus", i);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name);
        cJSON_AddStringToObject(entry, "type", "audio");
        cJSON_AddNumberToObject(entry, "fileSize", 1000);
        cJSON_AddStringToObject(entry, "auth", "chapter-secret");
        cJSON_AddItemToArray(content, entry);
    }
    char *result = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return result;
}

error_t __wrap_v3_native_cache_metadata_snapshot(const char *cache, const char *library,
    uint8_t overlay, const char *tag, uint32_t version, uint8_t **manifest,
    size_t *length, v3_native_library_collection_t *collection)
{
    (void)cache; (void)library; (void)overlay;
    assert(!held[MUTEX_V3_PLAYLIST_METADATA]);
    assert(!strcmp(tag, ruid));
    if (!snapshot_enabled || version != snapshot_version) return ERROR_NOT_FOUND;
    *manifest = (uint8_t *)manifest_fixture(version, snapshot_count, NULL);
    *length = strlen((char *)*manifest);
    collection->chapters = calloc(snapshot_count, sizeof(*collection->chapters));
    assert(collection->chapters);
    collection->chapter_count = snapshot_count;
    for (size_t i = 0; i < snapshot_count; i++)
    {
        v3_native_library_collection_chapter_t *chapter = &collection->chapters[i];
        snprintf(chapter->original_name, sizeof(chapter->original_name), "chapter-%zu.opus", i);
        chapter->index = i;
        chapter->file_size = 1000;
        if (!(missing_last && i == snapshot_count - 1)) chapter->path = strdup("valid.opus");
    }
    return NO_ERROR;
}

static void observe(uint32_t version, size_t count, const char *title)
{
    char *manifest = manifest_fixture(version, count, title);
    assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
        (uint8_t *)manifest, strlen(manifest)) == NO_ERROR);
    free(manifest);
}

static cJSON *playlist(uint32_t version)
{ return v3_playlist_metadata_json(&fixture_settings, ruid, version); }

static void check_revision(cJSON *playlist)
{
    char expected[65], actual[65];
    snprintf(actual, sizeof(actual), "%s", text(playlist, "metadataRevision"));
    cJSON *public_json = cJSON_Duplicate(playlist, TRUE);
    cJSON_DeleteItemFromObjectCaseSensitive(public_json, "metadataRevision");
    char *serialized = cJSON_PrintUnformatted(public_json);
    digest_text(serialized, strlen(serialized), expected);
    assert(!strcmp(expected, actual));
    free(serialized); cJSON_Delete(public_json);
}

static void test_structure(void)
{
    char *wrong_tracks[11];
    for (size_t i = 0; i < 11; i++) wrong_tracks[i] = "Old figurine chapter";
    catalog.tracks = wrong_tracks;
    observe(78, 78, NULL);
    observe(24, 24, NULL);
    for (uint32_t version = 24; version <= 78; version += 54)
    {
        catalog_version = version;
        catalog.tracks_count = version == 78 ? 10 : 11;
        cJSON *json = playlist(version);
        assert(cJSON_GetObjectItemCaseSensitive(json, "chapterCount")->valueint == (int)version);
        assert(cJSON_GetObjectItemCaseSensitive(json, "contentVersion")->valueint == (int)version);
        cJSON *tracks = cJSON_GetObjectItemCaseSensitive(json, "tracks");
        cJSON *durations = cJSON_GetObjectItemCaseSensitive(json, "durations");
        for (size_t i = 0; i < version; i++)
        {
            assert(!strcmp(cJSON_GetArrayItem(tracks, (int)i)->valuestring, ""));
            assert(cJSON_IsNull(cJSON_GetArrayItem(durations, (int)i)));
        }
        assert(!cJSON_HasObjectItem(json, "totalSeconds"));
        check_revision(json);
        cJSON_Delete(json);
    }
    cJSON *unknown = playlist(79);
    assert(!cJSON_HasObjectItem(unknown, "chapterCount"));
    assert(!cJSON_HasObjectItem(unknown, "contentVersion"));
    assert(!strcmp(text(unknown, "metadataState"), "unavailable"));
    cJSON_Delete(unknown);

    char *path = filename(&fixture_settings, ruid, 78, FALSE);
    cJSON *stored = read_json(path);
    char *serialized = cJSON_PrintUnformatted(stored);
    assert(strstr(serialized, "auth") == NULL && strstr(serialized, "secret") == NULL);
    free(path); free(serialized); cJSON_Delete(stored);
    fixture_settings.internal.overlayUniqueId = "different-box";
    unknown = playlist(78);
    assert(!cJSON_HasObjectItem(unknown, "chapterCount"));
    cJSON_Delete(unknown);
    fixture_settings.internal.overlayUniqueId = "box-one";

    /* Identical chapter counts do not make an unrelated catalogue version valid. */
    catalog.tracks_count = 2; catalog_version = 80;
    observe(81, 2, NULL);
    unknown = playlist(81);
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(unknown, "tracks"), 0)->valuestring, ""));
    cJSON_Delete(unknown);
    catalog_version = 81;
    unknown = playlist(81);
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(unknown, "tracks"), 0)->valuestring, "Old figurine chapter"));
    cJSON_Delete(unknown);
}

static void test_collection(void)
{
    v3_native_library_collection_chapter_t chapters[3] = {
        {.title = "First", .has_duration = TRUE, .duration_seconds = 1.625},
        {.title = "", .has_duration = FALSE},
        {.title = "Last", .has_duration = TRUE, .duration_seconds = 3.625},
    };
    v3_native_library_collection_t collection = {.chapters = chapters, .chapter_count = 3, .title = "Exact collection"};
    cJSON *json = v3_playlist_collection_json(&collection, TRUE, 300);
    assert(!strcmp(text(json, "metadataState"), "partial"));
    assert(cJSON_IsNull(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "durations"), 1)));
    assert(!cJSON_HasObjectItem(json, "totalSeconds"));
    char revision[65]; snprintf(revision, sizeof(revision), "%s", text(json, "metadataRevision"));
    cJSON_Delete(json);
    chapters[1].has_duration = TRUE; chapters[1].duration_seconds = 2.625;
    json = v3_playlist_collection_json(&collection, TRUE, 300);
    assert(!strcmp(text(json, "metadataState"), "ready"));
    assert(fabs(cJSON_GetObjectItemCaseSensitive(json, "totalSeconds")->valuedouble - 7.875) < 0.000001);
    assert(strcmp(revision, text(json, "metadataRevision")));
    cJSON_Delete(json);
    chapters[1].duration_seconds = NAN;
    json = v3_playlist_collection_json(&collection, FALSE, 300);
    assert(!cJSON_HasObjectItem(json, "contentVersion"));
    assert(!cJSON_HasObjectItem(json, "totalSeconds"));
    check_revision(json);
    cJSON_Delete(json);
}

static void test_atomic(void)
{
    observe(10, 2, NULL);
    char revision[65], current[65];
    v3_playlist_metadata_revision(&fixture_settings, ruid, 10, revision);
    observe(10, 2, NULL);
    v3_playlist_metadata_revision(&fixture_settings, ruid, 10, current);
    assert(!strcmp(revision, current));
    char *original = manifest_fixture(10, 2, NULL);
    char *whitespace = custom_asprintf("%s\n \r\t", original);
    assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
        (uint8_t *)whitespace, strlen(whitespace)) == NO_ERROR);
    free(whitespace);
    char *garbage = custom_asprintf("%s trailing", original);
    assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
        (uint8_t *)garbage, strlen(garbage)) == ERROR_INVALID_FILE);
    free(original); free(garbage);
    char *changed = manifest_fixture(10, 2, "New title");
    for (failure_stage = 1; failure_stage <= 3; failure_stage++)
    {
        assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
            (uint8_t *)changed, strlen(changed)) != NO_ERROR);
        v3_playlist_metadata_revision(&fixture_settings, ruid, 10, current);
        assert(!strcmp(revision, current));
    }
    failure_stage = 0;
    assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
        (uint8_t *)changed, strlen(changed)) == NO_ERROR);
    free(changed);
    v3_playlist_metadata_revision(&fixture_settings, ruid, 10, current);
    assert(strcmp(revision, current));
    changed = manifest_fixture(10, 3, NULL);
    assert(v3_playlist_metadata_observe(&fixture_settings, ruid,
        (uint8_t *)changed, strlen(changed)) == ERROR_INVALID_FILE);
    free(changed);
    char *path = filename(&fixture_settings, ruid, 10, FALSE);
    char *temporary = custom_asprintf("%s.tmp", path);
    assert(!fsFileExists(temporary));
    free(temporary); free(path);
}

static void write_packet(FILE *file, ogg_stream_state *stream, unsigned char *data,
    long length, long number, ogg_int64_t granule, int eos)
{
    ogg_packet packet = {.packet = data, .bytes = length, .b_o_s = number == 0,
        .e_o_s = eos, .granulepos = granule, .packetno = number};
    assert(!ogg_stream_packetin(stream, &packet));
    ogg_page page;
    while (ogg_stream_flush(stream, &page))
    {
        assert(fwrite(page.header, 1, page.header_len, file) == (size_t)page.header_len);
        assert(fwrite(page.body, 1, page.body_len, file) == (size_t)page.body_len);
    }
}

static void opus_fixture(void)
{
    FILE *file = fopen("valid.opus", "wb"); assert(file);
    ogg_stream_state stream; assert(!ogg_stream_init(&stream, 123));
    unsigned char header[19] = {'O','p','u','s','H','e','a','d',1,2,0x38,1,0x80,0xbb,0,0,0,0,0};
    unsigned char tags[] = {'O','p','u','s','T','a','g','s',0,0,0,0,1,0,0,0,10,0,0,0,'T','I','T','L','E','=','S','o','n','g'};
    unsigned char audio[] = {0xf8, 0xff, 0xfe};
    write_packet(file, &stream, header, sizeof(header), 0, 0, 0);
    write_packet(file, &stream, tags, sizeof(tags), 1, 0, 0);
    write_packet(file, &stream, audio, sizeof(audio), 2, 960, 0);
    write_packet(file, &stream, audio, sizeof(audio), 3, 1920, 1);
    ogg_stream_clear(&stream); fclose(file);
}

static void test_enrich(void)
{
    opus_fixture();
    snapshot_enabled = TRUE; snapshot_version = 20; snapshot_count = 2; missing_last = TRUE;
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 20) == NO_ERROR);
    assert(scans == 1);
    cJSON *json = playlist(20);
    assert(!strcmp(text(json, "metadataState"), "partial"));
    cJSON *durations = cJSON_GetObjectItemCaseSensitive(json, "durations");
    assert(fabs(cJSON_GetArrayItem(durations, 0)->valuedouble - (1920.0 - 312.0) / 48000.0) < 0.000001);
    assert(cJSON_IsNull(cJSON_GetArrayItem(durations, 1)));
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "tracks"), 0)->valuestring, "Song"));
    cJSON_Delete(json);
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 20) == NO_ERROR && scans == 1);
    missing_last = FALSE;
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 20) == NO_ERROR && scans == 2);
    json = playlist(20);
    assert(!strcmp(text(json, "metadataState"), "ready"));
    assert(fabs(cJSON_GetObjectItemCaseSensitive(json, "totalSeconds")->valuedouble - 0.067) < 0.000001);
    cJSON_Delete(json);
    assert(!v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 20));
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 21) != NO_ERROR);
    json = playlist(21); assert(!cJSON_HasObjectItem(json, "chapterCount")); cJSON_Delete(json);

    FILE *invalid = fopen("valid.opus", "wb"); assert(invalid);
    assert(fwrite("invalid", 1, 7, invalid) == 7); fclose(invalid);
    snapshot_version = 30;
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 30) == NO_ERROR && scans == 4);
    json = playlist(30);
    assert(!strcmp(text(json, "metadataState"), "partial"));
    assert(!cJSON_HasObjectItem(json, "totalSeconds"));
    assert(cJSON_IsNull(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "durations"), 0)));
    cJSON_Delete(json);
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 30) == NO_ERROR && scans == 4);
    opus_fixture();
    v3_playlist_metadata_reset_attempt(&fixture_settings, ruid);
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 30) == NO_ERROR && scans == 6);
    json = playlist(30);
    assert(!strcmp(text(json, "metadataState"), "ready"));
    cJSON_Delete(json);
}

static void test_concurrent_observe(void)
{
    opus_fixture();
    snapshot_enabled = TRUE; snapshot_version = 40; snapshot_count = 2;
    observe_during_scan = TRUE;
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 40) == NO_ERROR);
    cJSON *json = playlist(40);
    assert(!strcmp(text(json, "title"), "Late manifest title"));
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "tracks"), 0)->valuestring, "Late chapter title"));
    assert(!strcmp(text(json, "metadataState"), "ready"));
    cJSON_Delete(json);
}

static void test_corruption(void)
{
    observe(99, 2, NULL);
    char *path = filename(&fixture_settings, ruid, 99, FALSE);
    cJSON *doc = read_json(path);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(doc, "objects"), 0);
    set(first, "size", cJSON_CreateString("invalid"));
    assert(write_json(path, doc) == NO_ERROR);
    cJSON_Delete(doc); free(path);
    cJSON *json = playlist(99);
    assert(!cJSON_HasObjectItem(json, "chapterCount"));
    assert(!cJSON_HasObjectItem(json, "contentVersion"));
    cJSON_Delete(json);
}

static void test_assigned_collection(void)
{
    opus_fixture();
    assert(fsCreateDirEx("stage/chapters", TRUE) == NO_ERROR);
    assert(fsRenameFile("valid.opus", "stage/chapters/input.opus") == NO_ERROR);
    uint32_t size;
    assert(fsGetFileSize("stage/chapters/input.opus", &size) == NO_ERROR);
    v3_native_library_collection_chapter_t chapter = {
        .index = 0, .path = "stage/chapters/input.opus", .file_size = size,
        .title = "Custom chapter", .duration_seconds = 0.0335, .has_duration = TRUE,
    };
    v3_native_library_collection_t input = {.title = "Custom collection", .chapters = &chapter, .chapter_count = 1};
    char *source = NULL;
    bool_t reused = FALSE;
    assert(v3_native_library_publish_custom("library", "stage", &input, &source, &reused) == NO_ERROR);
    assert(source != NULL && !reused);
    assert(v3_playlist_metadata_bind_collection(&fixture_settings, ruid, 600, source) == NO_ERROR);
    assert(v3_playlist_metadata_enrich(&fixture_settings, ruid, 600) == NO_ERROR);
    assert(scans == 0); /* Existing file-intrinsic duration/title must not be scanned again. */
    cJSON *json = playlist(600);
    assert(cJSON_GetObjectItemCaseSensitive(json, "contentVersion")->valueint == 600);
    assert(cJSON_GetObjectItemCaseSensitive(json, "chapterCount")->valueint == 1);
    assert(!strcmp(text(json, "title"), "Custom collection"));
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "tracks"), 0)->valuestring, "Custom chapter"));
    assert(!strcmp(text(json, "metadataState"), "ready"));
    check_revision(json); cJSON_Delete(json);
    assert(!v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 600));
    json = playlist(601);
    assert(!cJSON_HasObjectItem(json, "chapterCount")); cJSON_Delete(json);
    free(source);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    fixture_settings.internal.datadirfull = ".";
    fixture_settings.internal.contentdirfull = "content";
    fixture_settings.internal.librarydirfull = "library";
    fixture_settings.internal.cachedirfull = "cache";
    fixture_settings.internal.overlayUniqueId = "box-one";
    if (!strcmp(argv[1], "structure")) test_structure();
    else if (!strcmp(argv[1], "collection")) test_collection();
    else if (!strcmp(argv[1], "atomic")) test_atomic();
    else if (!strcmp(argv[1], "enrich")) test_enrich();
    else if (!strcmp(argv[1], "concurrent-observe")) test_concurrent_observe();
    else if (!strcmp(argv[1], "corruption")) test_corruption();
    else if (!strcmp(argv[1], "assigned-collection")) test_assigned_collection();
    else if (!strcmp(argv[1], "attempt-write"))
    {
        observe(55, 2, NULL);
        assert(v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 55));
        assert(v3_playlist_metadata_begin_attempt(&fixture_settings, ruid, 55) == NO_ERROR);
        cJSON *json = playlist(55); assert(!strcmp(text(json, "metadataState"), "pending")); cJSON_Delete(json);
    }
    else if (!strcmp(argv[1], "attempt-restart"))
    {
        assert(!v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 55));
        assert(v3_playlist_metadata_begin_attempt(&fixture_settings, ruid, 55) == ERROR_ABORTED);
        cJSON *json = playlist(55); assert(strcmp(text(json, "metadataState"), "pending")); cJSON_Delete(json);
        v3_playlist_metadata_reset_attempt(&fixture_settings, ruid);
        assert(v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 55));
        assert(v3_playlist_metadata_begin_attempt(&fixture_settings, ruid, 55) == NO_ERROR);
        v3_playlist_metadata_finish_attempt(&fixture_settings, ruid, 55, "cloud_denied");
        assert(!v3_playlist_metadata_needs_fetch(&fixture_settings, ruid, 55));
    }
    else assert(!"unknown case");
    for (size_t i = 0; i < MUTEX_LAST; i++) assert(!held[i]);
    printf("PASS v3 playlist metadata: %s\n", argv[1]);
    return 0;
}
