#include "v3_playlist_metadata.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "encoder_api.h"
#include "fs_ext.h"
#include "hash/sha256.h"
#include "mutex_manager.h"
#include "server_helpers.h"
#include "toniesJson.h"
#include "v3_local_content.h"

#define V3_PLAYLIST_SCHEMA 1
#define V3_PLAYLIST_TITLE_BYTES 1024U
#define V3_PLAYLIST_DIRECTORY "content-metadata/v3-playlists"

/* The existing serial downloader owns at most one active metadata attempt. */
static char *active_attempt_path;
static bool_t title_valid(const char *title);

bool_t v3_playlist_metadata_is_local_taf(settings_t *settings, const char *ruid, uint32_t version)
{
    v3_local_content_generation_t generation = {0};
    bool_t found = settings != NULL && version != 0 &&
        v3_local_content_generation_load(settings->internal.cachedirfull,
            settings->internal.overlayNumber, ruid, version, &generation) == NO_ERROR &&
        generation.source_kind == V3_LOCAL_CONTENT_SOURCE_DIRECT_TAF;
    v3_local_content_generation_free(&generation);
    return found;
}

static bool_t json_end(const char *end, const char *limit)
{
    if (end == NULL) return FALSE;
    while (end < limit && isspace((unsigned char)*end)) end++;
    return end == limit;
}

static const char *text(const cJSON *json, const char *key)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
    return value != NULL ? value : "";
}

static void set(cJSON *json, const char *key, cJSON *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(json, key);
    cJSON_AddItemToObject(json, key, value);
}

static void set_text(cJSON *json, const char *key, const char *value)
{
    set(json, key, cJSON_CreateString(value != NULL ? value : ""));
}

static void digest_text(const void *data, size_t length, char output[65])
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256Compute(data, length, digest);
    for (size_t i = 0; i < sizeof(digest); i++)
        osSnprintf(output + i * 2U, 3, "%02x", digest[i]);
}

/* Include the stable overlay identity and actual content/library roots, never
 * only a runtime overlay slot that can later belong to another box. */
static char *directory(settings_t *settings, const char *ruid, bool_t create)
{
    char canonical[TB2_RUID_SIZE];
    if (settings == NULL || settings->internal.datadirfull == NULL ||
        settings->internal.contentdirfull == NULL || settings->internal.librarydirfull == NULL ||
        settings->internal.overlayUniqueId == NULL ||
        settings->internal.overlayUniqueId[0] == '\0' ||
        !tb2_ruid_canonicalize(ruid, canonical))
        return NULL;
    char *identity = custom_asprintf("%s\n%s\n%s", settings->internal.overlayUniqueId,
        settings->internal.contentdirfull, settings->internal.librarydirfull);
    if (identity == NULL) return NULL;
    for (char *p = identity; *p; p++)
        if (*p == '\\') *p = '/';
    char scope[65];
    digest_text(identity, strlen(identity), scope);
    osFreeMem(identity);
    char *result = custom_asprintf("%s/%s/%s/%s", settings->internal.datadirfull,
        V3_PLAYLIST_DIRECTORY, scope, canonical);
    if (result != NULL && create && fsCreateDirEx(result, TRUE) != NO_ERROR && !fsDirExists(result))
    {
        osFreeMem(result);
        return NULL;
    }
    return result;
}

static char *filename(settings_t *settings, const char *ruid, uint32_t version, bool_t create)
{
    char *dir = directory(settings, ruid, create);
    char *path = dir != NULL ? custom_asprintf("%s/%" PRIu32 ".json", dir, version) : NULL;
    osFreeMem(dir);
    return path;
}

static cJSON *read_json(const char *path)
{
    uint32_t length = 0;
    if (path == NULL || fsGetFileSize(path, &length) != NO_ERROR ||
        length == 0 || length > V3_PLAYLIST_METADATA_MAX_BYTES)
        return NULL;
    char *data = osAllocMem((size_t)length + 1U);
    FsFile *file = data != NULL ? fsOpenFile(path, FS_FILE_MODE_READ) : NULL;
    if (file == NULL) { osFreeMem(data); return NULL; }
    size_t received = 0;
    error_t error = fsReadFile(file, data, (size_t)length, &received);
    fsCloseFile(file);
    data[length] = '\0';
    const char *end = NULL;
    cJSON *json = error == NO_ERROR && received == length
        ? cJSON_ParseWithLengthOpts(data, (size_t)length, &end, FALSE) : NULL;
    if (json != NULL && !json_end(end, data + length)) { cJSON_Delete(json); json = NULL; }
    osFreeMem(data);
    return json;
}

static cJSON *load(settings_t *settings, const char *ruid, uint32_t version)
{
    char *path = filename(settings, ruid, version, FALSE);
    cJSON *json = read_json(path);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    cJSON *stored = cJSON_GetObjectItemCaseSensitive(json, "version");
    bool_t valid = TRUE;
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(json, "objects");
    if (objects != NULL)
    {
        valid = cJSON_IsArray(objects) && cJSON_GetArraySize(objects) > 0;
        cJSON *object;
        cJSON_ArrayForEach(object, objects)
        {
            cJSON *size = cJSON_GetObjectItemCaseSensitive(object, "size");
            cJSON *duration = cJSON_GetObjectItemCaseSensitive(object, "duration");
            valid = valid && cJSON_IsObject(object) &&
                v3_native_cache_chapter_name_is_safe(text(object, "name")) &&
                cJSON_IsNumber(size) && size->valuedouble >= 1 && size->valuedouble <= INT32_MAX &&
                size->valuedouble == floor(size->valuedouble) && title_valid(text(object, "title")) &&
                (cJSON_IsNull(duration) || (cJSON_IsNumber(duration) &&
                    isfinite(duration->valuedouble) && duration->valuedouble >= 0));
        }
    }
    if (!valid || !cJSON_IsObject(json) || !cJSON_IsNumber(schema) || schema->valuedouble != V3_PLAYLIST_SCHEMA ||
        !cJSON_IsNumber(stored) || stored->valuedouble != version)
    {
        cJSON_Delete(json);
        osFreeMem(path);
        return NULL;
    }
    if (!strcmp(text(json, "state"), "pending") &&
        (active_attempt_path == NULL || path == NULL || strcmp(active_attempt_path, path)))
    {
        set_text(json, "state", cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(json, "objects"))
            ? "partial" : "unavailable");
        set_text(json, "reason", "metadata_attempt_interrupted");
    }
    osFreeMem(path);
    return json;
}

/* Caller serializes publication, not slow audio scans, with the metadata lock. */
static error_t write_json(const char *path, cJSON *json)
{
    char *data = cJSON_PrintUnformatted(json);
    if (data == NULL) return ERROR_OUT_OF_MEMORY;
    size_t length = strlen(data);
    if (length > V3_PLAYLIST_METADATA_MAX_BYTES) { cJSON_free(data); return ERROR_INVALID_LENGTH; }
    char *temporary = path != NULL ? custom_asprintf("%s.tmp", path) : NULL;
    FsFile *file = temporary != NULL ? fsOpenFile(temporary, FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC) : NULL;
    error_t error = file != NULL ? fsWriteFile(file, data, length) : ERROR_FILE_OPENING_FAILED;
    if (file != NULL)
    {
        error_t flush = fsFlushFile(file);
        if (error == NO_ERROR) error = flush;
        fsCloseFile(file);
    }
    if (error == NO_ERROR) error = fsRenameFile(temporary, path);
    if (temporary != NULL && fsFileExists(temporary)) fsDeleteFile(temporary);
    osFreeMem(temporary);
    cJSON_free(data);
    return error;
}

static error_t save(settings_t *settings, const char *ruid, uint32_t version, cJSON *json)
{
    char *path = filename(settings, ruid, version, TRUE);
    error_t error = path != NULL ? write_json(path, json) : ERROR_FILE_OPENING_FAILED;
    osFreeMem(path);
    return error;
}

static bool_t title_valid(const char *title)
{
    if (title == NULL || strlen(title) > V3_PLAYLIST_TITLE_BYTES) return FALSE;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++)
        if (*p < 0x20) return FALSE;
    return TRUE;
}

static cJSON *empty_document(uint32_t version)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "schema", V3_PLAYLIST_SCHEMA);
    cJSON_AddNumberToObject(json, "version", version);
    cJSON_AddBoolToObject(json, "attempted", FALSE);
    cJSON_AddBoolToObject(json, "missingAudio", TRUE);
    cJSON_AddStringToObject(json, "state", "unavailable");
    cJSON_AddStringToObject(json, "reason", "metadata_not_available");
    cJSON_AddStringToObject(json, "title", "");
    return json;
}

static bool_t same_objects(const cJSON *left, const cJSON *right)
{
    if (!cJSON_IsArray(left) || !cJSON_IsArray(right) || cJSON_GetArraySize(left) != cJSON_GetArraySize(right))
        return FALSE;
    for (int i = 0; i < cJSON_GetArraySize(left); i++)
    {
        const cJSON *a = cJSON_GetArrayItem(left, i), *b = cJSON_GetArrayItem(right, i);
        const cJSON *as = cJSON_GetObjectItemCaseSensitive(a, "size"), *bs = cJSON_GetObjectItemCaseSensitive(b, "size");
        if (strcmp(text(a, "name"), text(b, "name")) || !cJSON_IsNumber(as) || !cJSON_IsNumber(bs) ||
            as->valuedouble != bs->valuedouble) return FALSE;
    }
    return TRUE;
}

error_t v3_playlist_metadata_observe(settings_t *settings, const char *ruid,
    const uint8_t *manifest, size_t length)
{
    if (manifest == NULL || length == 0 || length > V3_PLAYLIST_METADATA_MAX_BYTES)
        return ERROR_INVALID_LENGTH;
    const char *end = NULL;
    cJSON *raw = cJSON_ParseWithLengthOpts((const char *)manifest, length, &end, FALSE);
    cJSON *version_json = cJSON_GetObjectItemCaseSensitive(raw, "version");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(raw, "content");
    if (!json_end(end, (const char *)manifest + length) || !cJSON_IsNumber(version_json) ||
        version_json->valuedouble < 1 || version_json->valuedouble > UINT32_MAX ||
        version_json->valuedouble != (uint32_t)version_json->valuedouble ||
        !cJSON_IsArray(content) || cJSON_GetArraySize(content) == 0)
    { cJSON_Delete(raw); return ERROR_INVALID_FILE; }
    uint32_t version = (uint32_t)version_json->valuedouble;
    cJSON *doc = empty_document(version);
    cJSON *objects = cJSON_AddArrayToObject(doc, "objects");
    error_t error = NO_ERROR;
    cJSON *entry;
    cJSON_ArrayForEach(entry, content)
    {
        cJSON *size = cJSON_GetObjectItemCaseSensitive(entry, "fileSize");
        if (text(entry, "type")[0] && strcmp(text(entry, "type"), "audio"))
        { error = ERROR_NOT_IMPLEMENTED; break; }
        if (strcmp(text(entry, "type"), "audio") || !v3_native_cache_chapter_name_is_safe(text(entry, "name")) ||
            !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > INT32_MAX ||
            size->valuedouble != (uint32_t)size->valuedouble)
        { error = ERROR_INVALID_FILE; break; }
        cJSON *object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, "name", text(entry, "name"));
        cJSON_AddNumberToObject(object, "size", size->valuedouble);
        const char *title = text(entry, "title");
        cJSON_AddStringToObject(object, "title", title_valid(title) ? title : "");
        cJSON_AddStringToObject(object, "titleSource", title[0] ? "manifest" : "");
        cJSON_AddNullToObject(object, "duration");
        cJSON_AddBoolToObject(object, "scanned", FALSE);
        cJSON_AddItemToArray(objects, object);
    }
    const char *title = text(raw, "title");
    if (title_valid(title)) set_text(doc, "title", title);
    cJSON_Delete(raw);
    if (error != NO_ERROR || cJSON_GetArraySize(objects) == 0)
    { cJSON_Delete(doc); return error != NO_ERROR ? error : ERROR_OUT_OF_MEMORY; }
    set_text(doc, "state", "partial");
    set_text(doc, "reason", "durations_pending");
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *previous = load(settings, ruid, version);
    cJSON *old_objects = cJSON_GetObjectItemCaseSensitive(previous, "objects");
    if (cJSON_IsArray(old_objects) && !same_objects(old_objects, objects))
        error = ERROR_INVALID_FILE;
    else if (previous != NULL)
    {
        /* Identical manifests (including refreshed auth) never discard measured
         * durations, attempt state or content-specific names. */
        for (int i = 0; i < cJSON_GetArraySize(old_objects); i++)
        {
            cJSON *old = cJSON_GetArrayItem(old_objects, i), *object = cJSON_GetArrayItem(objects, i);
            if ((!text(old, "title")[0] || !strcmp(text(old, "titleSource"), "catalog") ||
                !strcmp(text(old, "titleSource"), "ogg")) && text(object, "title")[0] != '\0')
            {
                set_text(old, "title", text(object, "title"));
                set_text(old, "titleSource", "manifest");
            }
        }
        if (!cJSON_IsArray(old_objects))
            set(previous, "objects", cJSON_Duplicate(objects, TRUE));
        if (text(previous, "title")[0] == '\0') set_text(previous, "title", text(doc, "title"));
        error = save(settings, ruid, version, previous);
    }
    else error = save(settings, ruid, version, doc);
    cJSON_Delete(previous);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON_Delete(doc);
    return error;
}

/* File-intrinsic metadata can be shared. Manifest/catalog titles must never be
 * written into a content-hash collection used by another RUID. */
static error_t update_library(settings_t *settings,
    const v3_native_library_collection_t *collection, const cJSON *objects)
{
    if (strlen(collection->content_hash) != V3_NATIVE_LIBRARY_HASH_HEX_LENGTH) return NO_ERROR;
    char *path = custom_asprintf("%s/by/contentHash/%s/library-entry.json",
        settings->internal.librarydirfull, collection->content_hash);
    if (path == NULL) return ERROR_OUT_OF_MEMORY;
    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);
    cJSON *library = read_json(path);
    cJSON *chapters = cJSON_GetObjectItemCaseSensitive(library, "chapters");
    bool_t changed = FALSE;
    error_t error = NO_ERROR;
    if ((size_t)cJSON_GetArraySize(chapters) != collection->chapter_count) error = ERROR_INVALID_FILE;
    for (size_t i = 0; error == NO_ERROR && i < collection->chapter_count; i++)
    {
        cJSON *chapter = cJSON_GetArrayItem(chapters, (int)i);
        const cJSON *object = cJSON_GetArrayItem(objects, (int)i);
        if (strcmp(text(chapter, "sha256"), collection->chapters[i].sha256))
        { error = ERROR_INVALID_FILE; break; }
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(object, "duration");
        if (cJSON_IsNumber(duration) && !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(chapter, "durationSeconds")))
        { set(chapter, "durationSeconds", cJSON_Duplicate(duration, TRUE)); changed = TRUE; }
        if (!text(chapter, "title")[0] && !strcmp(text(object, "titleSource"), "ogg"))
        { set_text(chapter, "title", text(object, "title")); changed = TRUE; }
    }
    if (error == NO_ERROR && changed) error = write_json(path, library);
    cJSON_Delete(library);
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);
    osFreeMem(path);
    return error;
}

static int title_priority(const cJSON *object)
{
    if (!text(object, "title")[0]) return 0;
    const char *source = text(object, "titleSource");
    return !strcmp(source, "library") ? 4 : !strcmp(source, "manifest") ? 3 :
        !strcmp(source, "ogg") ? 2 : 1;
}

static cJSON *collection_document(const v3_native_library_collection_t *collection, uint32_t version)
{
    cJSON *doc = empty_document(version);
    cJSON *objects = cJSON_AddArrayToObject(doc, "objects");
    set_text(doc, "title", collection->title);
    toniesJson_item_t *catalog = NULL;
    for (size_t i = 0; i < collection->origin_count; i++)
    {
        toniesJson_item_t *candidate = tonies_byAudioIdTrackCountUnique(
            collection->origins[i].content_version, collection->chapter_count);
        if (candidate == NULL || candidate->tracks == NULL ||
            (size_t)candidate->tracks_count != collection->chapter_count) continue;
        if (catalog != NULL && candidate != catalog) { catalog = NULL; break; }
        catalog = candidate;
    }
    if (!text(doc, "title")[0] && catalog != NULL) set_text(doc, "title", catalog->title);
    bool_t complete = collection->chapter_count != 0;
    for (size_t i = 0; i < collection->chapter_count; i++)
    {
        const v3_native_library_collection_chapter_t *chapter = &collection->chapters[i];
        cJSON *object = cJSON_CreateObject();
        cJSON_AddStringToObject(object, "name", chapter->original_name);
        cJSON_AddNumberToObject(object, "size", chapter->file_size);
        const char *title = chapter->title;
        bool_t intrinsic = title != NULL && title[0];
        if (!intrinsic && catalog != NULL) title = catalog->tracks[i];
        cJSON_AddStringToObject(object, "title", title != NULL ? title : "");
        cJSON_AddStringToObject(object, "titleSource", intrinsic ? "library" : "catalog");
        bool_t known = chapter->has_duration && isfinite(chapter->duration_seconds) && chapter->duration_seconds >= 0;
        cJSON_AddItemToObject(object, "duration", known ? cJSON_CreateNumber(chapter->duration_seconds) : cJSON_CreateNull());
        cJSON_AddBoolToObject(object, "scanned", FALSE);
        complete = complete && known;
        cJSON_AddItemToArray(objects, object);
    }
    set(doc, "missingAudio", cJSON_CreateBool(FALSE));
    set_text(doc, "state", complete ? "ready" : "partial");
    set_text(doc, "reason", complete ? "" : "durations_unavailable");
    return doc;
}

error_t v3_playlist_metadata_bind_collection(settings_t *settings, const char *ruid,
    uint32_t version, const char *source)
{
    v3_native_library_collection_t collection = {0};
    error_t error = v3_native_library_collection_load(settings->internal.librarydirfull, source, FALSE, &collection);
    if (error != NO_ERROR) return error;
    cJSON *doc = collection_document(&collection, version);
    set_text(doc, "librarySource", source);
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *previous = load(settings, ruid, version);
    if (previous != NULL)
    {
        if (!cJSON_HasObjectItem(previous, "objects"))
        {
            set(doc, "attempted", cJSON_CreateBool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(previous, "attempted"))));
            error = save(settings, ruid, version, doc);
        }
        else if (strcmp(text(previous, "librarySource"), source)) error = ERROR_INVALID_FILE;
        /* Never discard completed scans or the persistent attempt latch. */
    }
    else error = save(settings, ruid, version, doc);
    cJSON_Delete(previous);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON_Delete(doc);
    v3_native_library_collection_free(&collection);
    return error;
}

error_t v3_playlist_metadata_enrich(settings_t *settings, const char *ruid, uint32_t version)
{
    if (settings == NULL || version == 0) return ERROR_INVALID_PARAMETER;
    uint8_t *manifest = NULL;
    size_t length = 0;
    v3_native_library_collection_t collection = {0};
    cJSON *local = NULL;
    error_t error = v3_native_cache_metadata_snapshot(settings->internal.cachedirfull,
        settings->internal.librarydirfull, settings->internal.overlayNumber,
        ruid, version, &manifest, &length, &collection);
    if (error == NO_ERROR)
        error = v3_playlist_metadata_observe(settings, ruid, manifest, length);
    else
    {
        mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
        local = load(settings, ruid, version);
        mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
        error = text(local, "librarySource")[0]
            ? v3_native_library_collection_load(settings->internal.librarydirfull,
                text(local, "librarySource"), FALSE, &collection) : ERROR_NOT_FOUND;
        if (error != NO_ERROR)
        {
            cJSON_Delete(local); local = NULL;
        }
    }
    osFreeMem(manifest);
    if (error != NO_ERROR)
    { v3_native_library_collection_free(&collection); return error; }
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = load(settings, ruid, version);
    if (doc == NULL) { doc = local; local = NULL; }
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON_Delete(local);
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(doc, "objects");
    if ((size_t)cJSON_GetArraySize(objects) != collection.chapter_count)
    { cJSON_Delete(doc); v3_native_library_collection_free(&collection); return ERROR_INVALID_FILE; }
    bool_t missing = FALSE, complete = collection.chapter_count != 0;
    if (collection.title != NULL && title_valid(collection.title) && collection.title[0])
        set_text(doc, "title", collection.title);
    for (size_t i = 0; i < collection.chapter_count; i++)
    {
        cJSON *object = cJSON_GetArrayItem(objects, (int)i);
        const v3_native_library_collection_chapter_t *chapter = &collection.chapters[i];
        if (strcmp(text(object, "name"), chapter->original_name))
        { error = ERROR_INVALID_FILE; break; }
        if (chapter->title != NULL && chapter->title[0] && title_valid(chapter->title))
        { set_text(object, "title", chapter->title); set_text(object, "titleSource", "library"); }
        if (chapter->has_duration)
            set(object, "duration", cJSON_CreateNumber(chapter->duration_seconds));
        if (chapter->path == NULL) missing = TRUE;
        else if ((!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, "duration")) ||
            title_priority(object) < 2) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "scanned")))
        {
            double duration = 0;
            char title[V3_PLAYLIST_TITLE_BYTES + 1U];
            /* No state or library lock while reading Ogg pages. */
            error_t scan = encoder_read_opus_metadata(chapter->path, &duration, title, sizeof(title));
            set(object, "scanned", cJSON_CreateBool(TRUE));
            if (scan == NO_ERROR)
            {
                set(object, "duration", cJSON_CreateNumber(duration));
                if (title_priority(object) < 2 && title_valid(title) && title[0])
                { set_text(object, "title", title); set_text(object, "titleSource", "ogg"); }
            }
        }
        complete = complete && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, "duration"));
    }
    if (error == NO_ERROR) error = update_library(settings, &collection, objects);
    if (error == NO_ERROR)
    {
        mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
        cJSON *latest = load(settings, ruid, version);
        cJSON *latest_objects = cJSON_GetObjectItemCaseSensitive(latest, "objects");
        if (latest_objects != NULL && !same_objects(latest_objects, objects)) error = ERROR_INVALID_FILE;
        else
        {
            if (latest != NULL)
            {
                set(doc, "attempted", cJSON_CreateBool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(latest, "attempted"))));
                set_text(doc, "state", text(latest, "state"));
                if ((collection.title == NULL || !collection.title[0]) && text(latest, "title")[0])
                    set_text(doc, "title", text(latest, "title"));
                for (int i = 0; i < cJSON_GetArraySize(latest_objects); i++)
                {
                    cJSON *newer = cJSON_GetArrayItem(latest_objects, i);
                    cJSON *scanned = cJSON_GetArrayItem(objects, i);
                    if (title_priority(newer) >= title_priority(scanned) && title_priority(newer) != 0)
                    {
                        set_text(scanned, "title", text(newer, "title"));
                        set_text(scanned, "titleSource", text(newer, "titleSource"));
                    }
                    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(scanned, "duration")) &&
                        cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(newer, "duration")))
                        set(scanned, "duration", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(newer, "duration"), TRUE));
                }
            }
            complete = collection.chapter_count != 0;
            cJSON *object;
            cJSON_ArrayForEach(object, objects)
                complete = complete && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, "duration"));
            set(doc, "missingAudio", cJSON_CreateBool(missing));
            if (strcmp(text(doc, "state"), "pending"))
            {
                set_text(doc, "state", complete ? "ready" : "partial");
                set_text(doc, "reason", complete ? "" : "durations_unavailable");
            }
            error = save(settings, ruid, version, doc);
        }
        cJSON_Delete(latest);
        mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    }
    cJSON_Delete(doc);
    v3_native_library_collection_free(&collection);
    return error;
}

static toniesJson_item_t *catalog_for(uint32_t version, size_t count)
{
    toniesJson_item_t *item = tonies_byAudioIdTrackCountUnique(version, count);
    return item != NULL && item->tracks != NULL && (size_t)item->tracks_count == count ? item : NULL;
}

static cJSON *playlist_from_document(const cJSON *doc)
{
    cJSON *playlist = cJSON_CreateObject();
    cJSON_AddStringToObject(playlist, "kind", "native_collection");
    cJSON_AddBoolToObject(playlist, "editable", FALSE);
    cJSON_AddStringToObject(playlist, "metadataState", doc ? text(doc, "state") : "unavailable");
    cJSON_AddStringToObject(playlist, "metadataReason", doc ? text(doc, "reason") : "metadata_not_available");
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(doc, "objects");
    size_t count = (size_t)cJSON_GetArraySize(objects);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version");
    toniesJson_item_t *catalog = cJSON_IsArray(objects) && cJSON_IsNumber(version)
        ? catalog_for((uint32_t)version->valuedouble, count) : NULL;
    const char *title = text(doc, "title");
    cJSON_AddStringToObject(playlist, "title", title[0] ? title : catalog && catalog->title ? catalog->title : "");
    if (cJSON_IsArray(objects))
    {
        cJSON_AddNumberToObject(playlist, "chapterCount", (double)count);
        if (cJSON_IsNumber(version)) cJSON_AddNumberToObject(playlist, "contentVersion", version->valuedouble);
    }
    cJSON *tracks = cJSON_AddArrayToObject(playlist, "tracks");
    cJSON *durations = cJSON_AddArrayToObject(playlist, "durations");
    double total = 0;
    bool_t complete = count != 0;
    for (size_t i = 0; i < count; i++)
    {
        cJSON *object = cJSON_GetArrayItem(objects, (int)i);
        const char *name = text(object, "title");
        if (!name[0] && catalog != NULL && catalog->tracks[i] != NULL) name = catalog->tracks[i];
        cJSON_AddItemToArray(tracks, cJSON_CreateString(name));
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(object, "duration");
        bool_t known = cJSON_IsNumber(duration) && isfinite(duration->valuedouble) && duration->valuedouble >= 0;
        cJSON_AddItemToArray(durations, known ? cJSON_CreateNumber(duration->valuedouble) : cJSON_CreateNull());
        if (known) total += duration->valuedouble;
        complete = complete && known;
    }
    if (complete) cJSON_AddNumberToObject(playlist, "totalSeconds", total);
    char *serialized = cJSON_PrintUnformatted(playlist);
    if (serialized != NULL)
    {
        char revision[65];
        digest_text(serialized, strlen(serialized), revision);
        cJSON_AddStringToObject(playlist, "metadataRevision", revision);
        cJSON_free(serialized);
    }
    return playlist;
}

cJSON *v3_playlist_metadata_json(settings_t *settings, const char *ruid, uint32_t version)
{
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = version != 0 ? load(settings, ruid, version) : NULL;
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    if (doc == NULL)
    {
        /* Structural fallback only: never scan audio or start cloud work from
         * tag-list/API polling. The exact-version snapshot is read-only. */
        uint8_t *manifest = NULL;
        size_t length = 0;
        v3_native_library_collection_t collection = {0};
        if (v3_native_cache_metadata_snapshot(settings->internal.cachedirfull,
            settings->internal.librarydirfull, settings->internal.overlayNumber,
            ruid, version, &manifest, &length, &collection) == NO_ERROR)
        {
            cJSON *raw = cJSON_ParseWithLength((const char *)manifest, length);
            cJSON *resolved = cJSON_GetObjectItemCaseSensitive(raw, "version");
            if (cJSON_IsNumber(resolved) && (version == 0 || resolved->valuedouble == version))
                doc = collection_document(&collection, (uint32_t)resolved->valuedouble);
            cJSON_Delete(raw);
        }
        osFreeMem(manifest);
        v3_native_library_collection_free(&collection);
    }
    cJSON *result = playlist_from_document(doc);
    cJSON_Delete(doc);
    return result;
}

void v3_playlist_metadata_revision(settings_t *settings, const char *ruid, uint32_t version, char revision[65])
{
    /* Polling reads only the small sidecar, not every audio path in a cache. */
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = load(settings, ruid, version);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *json = playlist_from_document(doc);
    osSnprintf(revision, V3_PLAYLIST_METADATA_REVISION_SIZE, "%s", text(json, "metadataRevision"));
    cJSON_Delete(json);
    cJSON_Delete(doc);
}

cJSON *v3_playlist_collection_json(const v3_native_library_collection_t *collection,
    bool_t version_valid, uint32_t version)
{
    cJSON *doc = collection != NULL ? collection_document(collection, version) : empty_document(version);
    if (!version_valid) cJSON_DeleteItemFromObjectCaseSensitive(doc, "version");
    cJSON *result = playlist_from_document(doc);
    cJSON_Delete(doc);
    return result;
}

bool_t v3_playlist_metadata_needs_fetch(settings_t *settings, const char *ruid, uint32_t version)
{
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = load(settings, ruid, version);
    bool_t result = doc == NULL || (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "attempted")) &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "missingAudio")));
    cJSON_Delete(doc);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    return result;
}

error_t v3_playlist_metadata_begin_attempt(settings_t *settings, const char *ruid, uint32_t version)
{
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = load(settings, ruid, version);
    if (doc == NULL) doc = empty_document(version);
    error_t error = ERROR_ABORTED;
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "attempted")))
    {
        set(doc, "attempted", cJSON_CreateBool(TRUE));
        set_text(doc, "state", "pending");
        set_text(doc, "reason", "");
        error = save(settings, ruid, version, doc);
        if (error == NO_ERROR)
        {
            osFreeMem(active_attempt_path);
            active_attempt_path = filename(settings, ruid, version, FALSE);
        }
    }
    cJSON_Delete(doc);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    return error;
}

void v3_playlist_metadata_finish_attempt(settings_t *settings, const char *ruid, uint32_t version, const char *reason)
{
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    cJSON *doc = load(settings, ruid, version);
    if (doc != NULL)
    {
        cJSON *objects = cJSON_GetObjectItemCaseSensitive(doc, "objects");
        bool_t complete = cJSON_IsArray(objects) && cJSON_GetArraySize(objects) > 0;
        cJSON *object;
        cJSON_ArrayForEach(object, objects)
            complete = complete && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, "duration"));
        set_text(doc, "state", complete ? "ready" : cJSON_IsArray(objects) ? "partial" : "unavailable");
        set_text(doc, "reason", complete ? "" : reason != NULL ? reason : "durations_unavailable");
        error_t error = save(settings, ruid, version, doc);
        if (error) TRACE_WARNING("V3 playlist result was not persisted: %s\r\n", error2text(error));
    }
    cJSON_Delete(doc);
    char *path = filename(settings, ruid, version, FALSE);
    if (path != NULL && active_attempt_path != NULL && !strcmp(path, active_attempt_path))
    { osFreeMem(active_attempt_path); active_attempt_path = NULL; }
    osFreeMem(path);
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
}

void v3_playlist_metadata_reset_attempt(settings_t *settings, const char *ruid)
{
    char *dir = directory(settings, ruid, FALSE);
    FsDir *listing = dir != NULL ? fsOpenDir(dir) : NULL;
    FsDirEntry entry;
    mutex_lock(MUTEX_V3_PLAYLIST_METADATA);
    while (listing != NULL && fsReadDir(listing, &entry) == NO_ERROR)
    {
        char *end;
        unsigned long version = strtoul(entry.name, &end, 10);
        if (end == entry.name || strcmp(end, ".json") || version == 0 || version > UINT32_MAX) continue;
        cJSON *doc = load(settings, ruid, (uint32_t)version);
        if (doc != NULL)
        {
            set(doc, "attempted", cJSON_CreateBool(FALSE));
            cJSON *object;
            cJSON_ArrayForEach(object, cJSON_GetObjectItemCaseSensitive(doc, "objects"))
                if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, "duration")))
                    set(object, "scanned", cJSON_CreateBool(FALSE));
            if (!strcmp(text(doc, "state"), "pending")) set_text(doc, "state", "partial");
            error_t error = save(settings, ruid, (uint32_t)version, doc);
            if (error) TRACE_WARNING("V3 playlist retry was not persisted: %s\r\n", error2text(error));
        }
        cJSON_Delete(doc);
    }
    mutex_unlock(MUTEX_V3_PLAYLIST_METADATA);
    if (listing != NULL) fsCloseDir(listing);
    osFreeMem(dir);
}
