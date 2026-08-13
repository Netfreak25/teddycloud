#define TRACE_LEVEL TRACE_LEVEL_DEBUG

#include "v3_native_cache.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug.h"
#include "fs_ext.h"
#include "hash/sha256.h"
#include "mutex_manager.h"
#include "settings.h"
#include "toniefile.h"

#define V3_NATIVE_CACHE_DIR "v3-native"
#define V3_NATIVE_CACHE_META_LIMIT (1024U * 1024U)
#define V3_NATIVE_CACHE_SCHEMA_LEGACY_AUDIO 1U
#define V3_NATIVE_CACHE_SCHEMA 2U
#define V3_NATIVE_RESERVED_LOCAL_PREFIX "teddycloud_"
#define V3_NATIVE_LIBRARY_SCHEMA 2U
#define V3_NATIVE_LIBRARY_HASH_BUFFER_SIZE 8192U
#define V3_NATIVE_LIBRARY_BY_DIR "by"
#define V3_NATIVE_LIBRARY_CONTENT_HASH_DIR "contentHash"
#define V3_NATIVE_LIBRARY_HASH_DOMAIN "TeddyCloud TB2 library collection v1"
#define V3_TONIEPLAY_LIBRARY_SCHEMA 3U
#define V3_TONIEPLAY_LIBRARY_HASH_DOMAIN "TeddyCloud TB2 Tonieplay collection v1"
#define V3_NATIVE_LIBRARY_SOURCE_PREFIX "lib://by/contentHash/"
#define V3_NATIVE_LIBRARY_SOURCE_SUFFIX "/library-entry.json"

typedef struct
{
    char name[V3_NATIVE_CACHE_OBJECT_NAME_SIZE];
    char auth[V3_NATIVE_CACHE_OBJECT_AUTH_SIZE];
    char type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    char filename[V3_NATIVE_CACHE_OBJECT_FILENAME_SIZE];
    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE];
    uint32_t file_size;
    bool_t capturing;
} v3_native_object_t;

typedef v3_native_object_t v3_native_chapter_t;

typedef struct
{
    char original_name[V3_NATIVE_CACHE_CHAPTER_NAME_SIZE];
    uint32_t file_size;
    uint8_t sha256[SHA256_DIGEST_SIZE];
    char sha256_hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
} v3_native_library_chapter_t;

typedef struct
{
    bool_t valid;
    bool_t active;
    bool_t capture_enabled;
    uint8_t overlay_id;
    char ruid[TB2_RUID_SIZE];
    uint32_t version;
    char content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    char *generation_dir;
    v3_native_object_t *chapters;
    size_t chapter_count;
    char *library_source;
    char **library_paths;
} v3_native_route_t;

typedef struct
{
    bool_t valid;
    char ruid[TB2_RUID_SIZE];
    uint32_t version;
    v3_tonieplay_library_collection_t collection;
} v3_tonieplay_assigned_route_t;

static v3_native_route_t routes[MAX_OVERLAYS];
static v3_tonieplay_assigned_route_t assigned_routes[MAX_OVERLAYS];

static bool_t v3_native_library_hash_is_canonical(const char *value);

static char *v3_native_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
    {
        va_end(args);
        return NULL;
    }
    char *result = osAllocMem((size_t)length + 1U);
    if (result != NULL)
    {
        vsnprintf(result, (size_t)length + 1U, format, args);
    }
    va_end(args);
    return result;
}

static void v3_native_route_clear(v3_native_route_t *route)
{
    if (route == NULL)
    {
        return;
    }
    if (route->library_paths != NULL)
    {
        for (size_t i = 0; i < route->chapter_count; i++)
        {
            osFreeMem(route->library_paths[i]);
        }
    }
    osFreeMem(route->library_paths);
    osFreeMem(route->library_source);
    osFreeMem(route->generation_dir);
    osFreeMem(route->chapters);
    osMemset(route, 0, sizeof(*route));
}

static void v3_tonieplay_assigned_route_clear(
    v3_tonieplay_assigned_route_t *route)
{
    if (route == NULL)
    {
        return;
    }
    v3_tonieplay_library_collection_free(&route->collection);
    osMemset(route, 0, sizeof(*route));
}

static error_t v3_native_ensure_dir(const char *path)
{
    if (path == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (fsDirExists(path))
    {
        return NO_ERROR;
    }
    error_t error = fsCreateDirEx(path, true);
    return error == NO_ERROR || fsDirExists(path) ? NO_ERROR : error;
}

static error_t v3_native_write_atomic(const char *path, const void *data, size_t length)
{
    char *temporary = v3_native_format("%s.part", path);
    if (temporary == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    fsDeleteFile(temporary);
    FsFile *file = fsOpenFile(temporary, FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
    error_t error = file == NULL ? ERROR_FILE_OPENING_FAILED : NO_ERROR;
    if (error == NO_ERROR)
    {
        error = fsWriteFile(file, (void *)data, length);
    }
    if (error == NO_ERROR)
    {
        error = fsFlushFile(file);
    }
    if (file != NULL)
    {
        fsCloseFile(file);
    }
    if (error == NO_ERROR)
    {
        error = fsRenameFile(temporary, path);
    }
    if (error != NO_ERROR)
    {
        fsDeleteFile(temporary);
    }
    osFreeMem(temporary);
    return error;
}

static error_t v3_native_read_file(const char *path, uint8_t **data, size_t *length)
{
    uint32_t file_size = 0;
    if (path == NULL || data == NULL || length == NULL ||
        fsGetFileSize(path, &file_size) != NO_ERROR || file_size == 0)
    {
        return ERROR_FILE_NOT_FOUND;
    }

    uint8_t *buffer = osAllocMem((size_t)file_size + 1U);
    FsFile *file = buffer != NULL ? fsOpenFile(path, FS_FILE_MODE_READ) : NULL;
    if (buffer == NULL || file == NULL)
    {
        osFreeMem(buffer);
        return buffer == NULL ? ERROR_OUT_OF_MEMORY : ERROR_FILE_OPENING_FAILED;
    }

    size_t position = 0;
    error_t error = NO_ERROR;
    while (position < file_size)
    {
        size_t received = 0;
        error = fsReadFile(file, buffer + position, file_size - position, &received);
        if (error != NO_ERROR || received == 0)
        {
            break;
        }
        position += received;
    }
    fsCloseFile(file);
    if (error != NO_ERROR || position != file_size)
    {
        osFreeMem(buffer);
        return error != NO_ERROR ? error : ERROR_READ_FAILED;
    }

    buffer[position] = '\0';
    *data = buffer;
    *length = position;
    return NO_ERROR;
}

static bool_t v3_native_reserved_windows_name(const char *name)
{
    char base[8] = {0};
    size_t length = 0;
    while (name[length] != '\0' && name[length] != '.' && length < sizeof(base) - 1U)
    {
        base[length] = (char)toupper((unsigned char)name[length]);
        length++;
    }
    base[length] = '\0';
    if (!osStrcmp(base, "CON") || !osStrcmp(base, "PRN") ||
        !osStrcmp(base, "AUX") || !osStrcmp(base, "NUL"))
    {
        return TRUE;
    }
    return length == 4U &&
           ((!osStrncmp(base, "COM", 3) || !osStrncmp(base, "LPT", 3)) &&
            base[3] >= '1' && base[3] <= '9');
}

bool_t v3_native_cache_chapter_name_is_safe(const char *name)
{
    if (name == NULL)
    {
        return FALSE;
    }
    size_t length = osStrlen(name);
    if (length == 0 || length >= V3_NATIVE_CACHE_CHAPTER_NAME_SIZE ||
        !osStrncasecmp(name, V3_NATIVE_RESERVED_LOCAL_PREFIX,
                       sizeof(V3_NATIVE_RESERVED_LOCAL_PREFIX) - 1U) ||
        name[0] == '.' || name[length - 1U] == '.' || name[length - 1U] == ' ' ||
        v3_native_reserved_windows_name(name))
    {
        return FALSE;
    }
    for (size_t i = 0; i < length; i++)
    {
        unsigned char value = (unsigned char)name[i];
        if (!(isalnum(value) || value == '_' || value == '-' || value == '.'))
        {
            return FALSE;
        }
        if (value == '.' && i + 1U < length && name[i + 1U] == '.')
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t v3_native_json_u32(const cJSON *item, uint32_t *value, bool_t allow_zero)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX)
    {
        return FALSE;
    }
    uint32_t converted = (uint32_t)item->valuedouble;
    if ((double)converted != item->valuedouble || (!allow_zero && converted == 0))
    {
        return FALSE;
    }
    *value = converted;
    return TRUE;
}

static bool_t v3_native_object_string_fits(const cJSON *item, size_t capacity)
{
    return item == NULL || cJSON_IsNull(item) ||
           (cJSON_IsString(item) && item->valuestring != NULL &&
            osStrlen(item->valuestring) < capacity);
}

static error_t v3_native_parse_manifest(const uint8_t *data,
                                        size_t length,
                                        uint32_t *version,
                                        char content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE],
                                        v3_native_object_t **objects,
                                        size_t *object_count)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, length, &end, 0);
    cJSON *content = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "content") : NULL;
    while (end != NULL && end < (const char *)data + length &&
           isspace((unsigned char)*end))
    {
        end++;
    }
    if (root == NULL || end != (const char *)data + length ||
        !v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(root, "version"), version, FALSE) ||
        !cJSON_IsArray(content))
    {
        cJSON_Delete(root);
        return ERROR_INVALID_FILE;
    }

    cJSON *manifest_type = cJSON_GetObjectItemCaseSensitive(root, "contentType");
    if (!v3_native_object_string_fits(manifest_type,
                                      V3_NATIVE_CACHE_OBJECT_TYPE_SIZE))
    {
        cJSON_Delete(root);
        return ERROR_INVALID_FILE;
    }
    osStrcpy(content_type,
             cJSON_IsString(manifest_type) ? manifest_type->valuestring : "");

    size_t count = (size_t)cJSON_GetArraySize(content);
    if (count == 0)
    {
        cJSON_Delete(root);
        return ERROR_INVALID_FILE;
    }

    v3_native_object_t *parsed = osAllocMem(count * sizeof(*parsed));
    if (parsed == NULL)
    {
        cJSON_Delete(root);
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(parsed, 0, count * sizeof(*parsed));
    size_t index = 0;
    error_t error = NO_ERROR;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *auth = cJSON_GetObjectItemCaseSensitive(item, "auth");
        cJSON *filename = cJSON_GetObjectItemCaseSensitive(item, "filename");
        cJSON *file_size = cJSON_GetObjectItemCaseSensitive(item, "fileSize");
        if (!cJSON_IsString(name) || name->valuestring == NULL ||
            !v3_native_cache_chapter_name_is_safe(name->valuestring) ||
            !v3_native_object_string_fits(auth,
                                          V3_NATIVE_CACHE_OBJECT_AUTH_SIZE) ||
            !v3_native_json_u32(file_size, &parsed[index].file_size, FALSE))
        {
            error = ERROR_INVALID_NAME;
            break;
        }
        for (size_t previous = 0; previous < index; previous++)
        {
            if (!osStrcasecmp(parsed[previous].name, name->valuestring))
            {
                error = ERROR_INVALID_NAME;
                break;
            }
        }
        if (error != NO_ERROR)
        {
            break;
        }
        osStrcpy(parsed[index].name, name->valuestring);
        if (cJSON_IsString(auth))
        {
            osStrcpy(parsed[index].auth, auth->valuestring);
        }
        if (cJSON_IsString(type) && type->valuestring != NULL &&
            osStrlen(type->valuestring) < sizeof(parsed[index].type))
        {
            osStrcpy(parsed[index].type, type->valuestring);
        }
        if (cJSON_IsString(filename) && filename->valuestring != NULL &&
            osStrlen(filename->valuestring) < sizeof(parsed[index].filename))
        {
            osStrcpy(parsed[index].filename, filename->valuestring);
        }
        osStrcpy(parsed[index].content_type,
                 !osStrcmp(parsed[index].type, "audio")
                     ? "audio/ogg"
                     : "application/octet-stream");
        index++;
    }
    cJSON_Delete(root);
    if (error != NO_ERROR)
    {
        osFreeMem(parsed);
        return error;
    }
    *objects = parsed;
    *object_count = count;
    return NO_ERROR;
}

static bool_t v3_native_objects_all_audio(const v3_native_object_t *objects,
                                          size_t object_count)
{
    if (objects == NULL || object_count == 0)
    {
        return FALSE;
    }
    for (size_t i = 0; i < object_count; i++)
    {
        if (osStrcmp(objects[i].type, "audio"))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t v3_native_route_is_tonieplay(const v3_native_route_t *route)
{
    return route != NULL && route->valid &&
           (!osStrcmp(route->content_type, "tonieplay") ||
            !v3_native_objects_all_audio(route->chapters,
                                         route->chapter_count));
}

static char *v3_native_generation_dir(const char *cache_root,
                                      const char *state,
                                      uint8_t overlay_id,
                                      const char *ruid,
                                      uint32_t version)
{
    return v3_native_format("%s%c%s%c%s%c%u%c%s%c%" PRIu32,
                            cache_root, PATH_SEPARATOR, V3_NATIVE_CACHE_DIR,
                            PATH_SEPARATOR, state, PATH_SEPARATOR,
                            (unsigned)overlay_id, PATH_SEPARATOR, ruid,
                            PATH_SEPARATOR, version);
}

static char *v3_native_active_marker_path(const char *cache_root,
                                          uint8_t overlay_id,
                                          const char *ruid)
{
    return v3_native_format("%s%c%s%cactive%c%u%c%s.json", cache_root,
                            PATH_SEPARATOR, V3_NATIVE_CACHE_DIR,
                            PATH_SEPARATOR, PATH_SEPARATOR,
                            (unsigned)overlay_id, PATH_SEPARATOR, ruid);
}

static error_t v3_native_write_descriptor(const char *stage_dir,
                                          uint8_t overlay_id,
                                          const char *ruid,
                                          uint32_t version,
                                          const char *content_type,
                                          const v3_native_chapter_t *chapters,
                                          size_t chapter_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = NULL;
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "schemaVersion", V3_NATIVE_CACHE_SCHEMA) == NULL ||
        cJSON_AddNumberToObject(root, "overlay", overlay_id) == NULL ||
        cJSON_AddStringToObject(root, "ruid", ruid) == NULL ||
        cJSON_AddNumberToObject(root, "version", version) == NULL ||
        cJSON_AddStringToObject(root, "contentType",
                               content_type != NULL ? content_type : "") == NULL ||
        (array = cJSON_AddArrayToObject(root, "objects")) == NULL)
    {
        cJSON_Delete(root);
        return ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < chapter_count; i++)
    {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddStringToObject(entry, "name", chapters[i].name) == NULL ||
            cJSON_AddStringToObject(entry, "type", chapters[i].type) == NULL ||
            cJSON_AddStringToObject(entry, "filename", chapters[i].filename) == NULL ||
            cJSON_AddNumberToObject(entry, "fileSize", chapters[i].file_size) == NULL ||
            cJSON_AddStringToObject(entry, "contentType",
                                   chapters[i].content_type) == NULL ||
            !cJSON_AddItemToArray(array, entry))
        {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return ERROR_OUT_OF_MEMORY;
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    char *path = v3_native_format("%s%cdescriptor.json", stage_dir, PATH_SEPARATOR);
    error_t error = path == NULL ? ERROR_OUT_OF_MEMORY
                                : v3_native_write_atomic(path, json, osStrlen(json));
    cJSON_free(json);
    osFreeMem(path);
    return error;
}

static void v3_native_load_descriptor(
    const char *generation_dir,
    v3_native_route_t *route,
    char **library_source)
{
    if (library_source != NULL)
    {
        *library_source = NULL;
    }
    char *path = generation_dir != NULL
                     ? v3_native_format("%s%cdescriptor.json", generation_dir,
                                        PATH_SEPARATOR)
                     : NULL;
    uint8_t *data = NULL;
    size_t length = 0;
    if (path == NULL || v3_native_read_file(path, &data, &length) != NO_ERROR)
    {
        osFreeMem(path);
        return;
    }
    osFreeMem(path);

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, length, &end, 0);
    cJSON *objects = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "objects")
                         : NULL;
    cJSON *content_type = root != NULL
                              ? cJSON_GetObjectItemCaseSensitive(root,
                                                                 "contentType")
                              : NULL;
    cJSON *source = root != NULL
                        ? cJSON_GetObjectItemCaseSensitive(root,
                                                           "librarySource")
                        : NULL;
    cJSON *stored_ruid = root != NULL
                             ? cJSON_GetObjectItemCaseSensitive(root, "ruid")
                             : NULL;
    uint32_t stored_overlay = 0;
    uint32_t stored_version = 0;
    bool_t descriptor_valid =
        root != NULL && end == (const char *)data + length &&
        cJSON_IsArray(objects) &&
        cJSON_GetArraySize(objects) == (int)route->chapter_count &&
        cJSON_IsString(stored_ruid) && stored_ruid->valuestring != NULL &&
        !osStrcasecmp(stored_ruid->valuestring, route->ruid) &&
        v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(root, "overlay"),
                           &stored_overlay, TRUE) &&
        stored_overlay == route->overlay_id &&
        v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(root, "version"),
                           &stored_version, FALSE) &&
        stored_version == route->version;
    if (descriptor_valid)
    {
        if (cJSON_IsString(content_type) && content_type->valuestring != NULL &&
            osStrlen(content_type->valuestring) < sizeof(route->content_type))
        {
            osStrcpy(route->content_type, content_type->valuestring);
        }
        for (size_t i = 0; i < route->chapter_count; i++)
        {
            cJSON *entry = cJSON_GetArrayItem(objects, (int)i);
            cJSON *name = entry != NULL
                              ? cJSON_GetObjectItemCaseSensitive(entry, "name")
                              : NULL;
            cJSON *mime = entry != NULL
                              ? cJSON_GetObjectItemCaseSensitive(entry,
                                                                 "contentType")
                              : NULL;
            if (cJSON_IsString(name) && name->valuestring != NULL &&
                !osStrcmp(name->valuestring, route->chapters[i].name) &&
                cJSON_IsString(mime) && mime->valuestring != NULL &&
                osStrlen(mime->valuestring) <
                    sizeof(route->chapters[i].content_type))
            {
                osStrcpy(route->chapters[i].content_type, mime->valuestring);
            }
        }
    }
    if (descriptor_valid && library_source != NULL && cJSON_IsString(source) &&
        source->valuestring != NULL &&
        v3_native_library_source_is_candidate(source->valuestring))
    {
        *library_source = strdup(source->valuestring);
    }
    cJSON_Delete(root);
    osFreeMem(data);
}

static bool_t v3_native_cache_files_complete(const v3_native_route_t *route)
{
    for (size_t i = 0; i < route->chapter_count; i++)
    {
        char *path = v3_native_format("%s%cchapters%c%s", route->generation_dir,
                                      PATH_SEPARATOR, PATH_SEPARATOR,
                                      route->chapters[i].name);
        uint32_t size = 0;
        bool_t complete = path != NULL &&
                          fsGetFileSize(path, &size) == NO_ERROR &&
                          size == route->chapters[i].file_size;
        osFreeMem(path);
        if (!complete)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t v3_native_origin_list_has(
    const v3_native_library_origin_t *origins,
    size_t origin_count,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version)
{
    for (size_t i = 0; i < origin_count; i++)
    {
        if (origins[i].overlay_id == overlay_id &&
            origins[i].content_version == version &&
            !osStrcasecmp(origins[i].ruid, ruid))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static bool_t v3_native_library_paths_complete(const v3_native_route_t *route)
{
    if (route == NULL || route->library_source == NULL ||
        route->library_paths == NULL)
    {
        return FALSE;
    }
    for (size_t i = 0; i < route->chapter_count; i++)
    {
        uint32_t size = 0;
        if (route->library_paths[i] == NULL ||
            fsGetFileSize(route->library_paths[i], &size) != NO_ERROR ||
            size != route->chapters[i].file_size)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static void v3_native_route_clear_library_backing(v3_native_route_t *route)
{
    if (route == NULL)
    {
        return;
    }
    if (route->library_paths != NULL)
    {
        for (size_t i = 0; i < route->chapter_count; i++)
        {
            osFreeMem(route->library_paths[i]);
        }
    }
    osFreeMem(route->library_paths);
    osFreeMem(route->library_source);
    route->library_paths = NULL;
    route->library_source = NULL;
}

static bool_t v3_native_route_use_library(
    const char *library_root,
    const char *library_source,
    v3_native_route_t *route)
{
    if (library_root == NULL || library_source == NULL || route == NULL ||
        route->chapter_count == 0)
    {
        return FALSE;
    }

    char **paths = osAllocMem(route->chapter_count * sizeof(*paths));
    if (paths == NULL)
    {
        return FALSE;
    }
    osMemset(paths, 0, route->chapter_count * sizeof(*paths));
    bool_t valid = TRUE;

    if (v3_native_route_is_tonieplay(route))
    {
        v3_tonieplay_library_collection_t collection;
        osMemset(&collection, 0, sizeof(collection));
        error_t error = v3_tonieplay_library_collection_load(
            library_root, library_source, FALSE, &collection);
        valid = error == NO_ERROR &&
                collection.content_version == route->version &&
                collection.object_count == route->chapter_count &&
                v3_native_origin_list_has(
                    collection.origins, collection.origin_count,
                    route->overlay_id, route->ruid, route->version);
        for (size_t i = 0; valid && i < route->chapter_count; i++)
        {
            valid = collection.objects[i].index == i &&
                    collection.objects[i].file_size ==
                        route->chapters[i].file_size &&
                    !osStrcmp(collection.objects[i].name,
                              route->chapters[i].name);
            if (valid)
            {
                paths[i] = strdup(collection.objects[i].path);
                valid = paths[i] != NULL;
            }
        }
        v3_tonieplay_library_collection_free(&collection);
    }
    else
    {
        v3_native_library_collection_t collection;
        osMemset(&collection, 0, sizeof(collection));
        error_t error = v3_native_library_collection_load(
            library_root, library_source, FALSE, &collection);
        valid = error == NO_ERROR &&
                collection.chapter_count == route->chapter_count &&
                v3_native_origin_list_has(
                    collection.origins, collection.origin_count,
                    route->overlay_id, route->ruid, route->version);
        for (size_t i = 0; valid && i < route->chapter_count; i++)
        {
            valid = collection.chapters[i].index == i &&
                    collection.chapters[i].file_size ==
                        route->chapters[i].file_size;
            if (valid)
            {
                paths[i] = strdup(collection.chapters[i].path);
                valid = paths[i] != NULL;
            }
        }
        v3_native_library_collection_free(&collection);
    }
    if (valid)
    {
        route->library_source = strdup(library_source);
        valid = route->library_source != NULL;
    }
    if (valid)
    {
        route->library_paths = paths;
        return TRUE;
    }

    for (size_t i = 0; i < route->chapter_count; i++)
    {
        osFreeMem(paths[i]);
    }
    osFreeMem(paths);
    osFreeMem(route->library_source);
    route->library_source = NULL;
    return FALSE;
}

static void v3_native_compact_cache_files(v3_native_route_t *route)
{
    if (!v3_native_library_paths_complete(route))
    {
        return;
    }
    size_t removed = 0;
    for (size_t i = 0; i < route->chapter_count; i++)
    {
        char *path = v3_native_format("%s%cchapters%c%s",
                                      route->generation_dir,
                                      PATH_SEPARATOR, PATH_SEPARATOR,
                                      route->chapters[i].name);
        if (path != NULL && fsFileExists(path))
        {
            if (fsDeleteFile(path) == NO_ERROR)
            {
                removed++;
            }
            else
            {
                TRACE_WARNING("Could not remove duplicate TB2 V3 cache object overlay=%u rUID=%s version=%" PRIu32 " name=%s\r\n",
                              (unsigned)route->overlay_id, route->ruid,
                              route->version, route->chapters[i].name);
            }
        }
        osFreeMem(path);
    }
    if (removed > 0)
    {
        TRACE_INFO("Compacted TB2 V3 cache to library backing overlay=%u rUID=%s version=%" PRIu32 " objects=%" PRIuSIZE "\r\n",
                   (unsigned)route->overlay_id, route->ruid, route->version,
                   removed);
    }
}

static error_t v3_native_read_active_marker(const char *cache_root,
                                            uint8_t overlay_id,
                                            const char *canonical_ruid,
                                            uint32_t *version,
                                            uint32_t *schema_version)
{
    char *marker_path = v3_native_active_marker_path(cache_root, overlay_id,
                                                      canonical_ruid);
    uint8_t *marker_data = NULL;
    size_t marker_length = 0;
    error_t error = marker_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(marker_path, &marker_data,
                                              &marker_length);
    osFreeMem(marker_path);
    if (error != NO_ERROR)
    {
        return error;
    }

    const char *end = NULL;
    cJSON *marker = cJSON_ParseWithLengthOpts((const char *)marker_data,
                                               marker_length, &end, 0);
    uint32_t schema = 0;
    uint32_t marker_overlay = 0;
    uint32_t marker_version = 0;
    cJSON *marker_ruid = marker != NULL
                            ? cJSON_GetObjectItemCaseSensitive(marker, "ruid")
                            : NULL;
    bool_t valid = marker != NULL && end == (const char *)marker_data + marker_length &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(marker, "schemaVersion"),
                                      &schema, FALSE) &&
                   (schema == V3_NATIVE_CACHE_SCHEMA_LEGACY_AUDIO ||
                    schema == V3_NATIVE_CACHE_SCHEMA) &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(marker, "overlay"),
                                      &marker_overlay, TRUE) &&
                   marker_overlay == overlay_id &&
                   cJSON_IsString(marker_ruid) && marker_ruid->valuestring != NULL &&
                   !osStrcasecmp(marker_ruid->valuestring, canonical_ruid) &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(marker, "version"),
                                      &marker_version, FALSE);
    cJSON_Delete(marker);
    osFreeMem(marker_data);
    if (!valid)
    {
        return ERROR_INVALID_FILE;
    }
    *version = marker_version;
    if (schema_version != NULL)
    {
        *schema_version = schema;
    }
    return NO_ERROR;
}

error_t v3_native_cache_read_active_manifest(const char *cache_root,
                                             const char *library_root,
                                             uint8_t overlay_id,
                                             const char *ruid,
                                             uint8_t **data,
                                             size_t *length,
                                             uint32_t *version)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (cache_root == NULL || data == NULL || length == NULL || version == NULL ||
        overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }
    *data = NULL;
    *length = 0;
    *version = 0;

    uint32_t marker_version = 0;
    uint32_t marker_schema = 0;
    error_t error = v3_native_read_active_marker(cache_root, overlay_id,
                                                  canonical_ruid,
                                                  &marker_version,
                                                  &marker_schema);
    if (error != NO_ERROR)
    {
        return error;
    }

    char *generation_dir = v3_native_generation_dir(cache_root, "versions",
                                                     overlay_id, canonical_ruid,
                                                     marker_version);
    char *manifest_path = generation_dir != NULL
                              ? v3_native_format("%s%cmanifest.json", generation_dir,
                                                 PATH_SEPARATOR)
                              : NULL;
    uint8_t *manifest_data = NULL;
    size_t manifest_length = 0;
    error = manifest_path == NULL
                ? ERROR_OUT_OF_MEMORY
                : v3_native_read_file(manifest_path, &manifest_data,
                                      &manifest_length);
    osFreeMem(manifest_path);
    if (error != NO_ERROR)
    {
        osFreeMem(generation_dir);
        return error;
    }

    uint32_t manifest_version = 0;
    char manifest_content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE] = {0};
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error = v3_native_parse_manifest(manifest_data, manifest_length,
                                     &manifest_version, manifest_content_type,
                                     &chapters,
                                     &chapter_count);
    if (error == NO_ERROR &&
        marker_schema == V3_NATIVE_CACHE_SCHEMA_LEGACY_AUDIO &&
        !v3_native_objects_all_audio(chapters, chapter_count))
    {
        error = ERROR_INVALID_FILE;
    }
    v3_native_route_t loaded;
    osMemset(&loaded, 0, sizeof(loaded));
    loaded.valid = error == NO_ERROR && manifest_version == marker_version;
    loaded.active = loaded.valid;
    loaded.capture_enabled = TRUE;
    loaded.overlay_id = overlay_id;
    loaded.version = marker_version;
    osStrcpy(loaded.content_type, manifest_content_type);
    loaded.generation_dir = generation_dir;
    loaded.chapters = chapters;
    loaded.chapter_count = chapter_count;
    osStrcpy(loaded.ruid, canonical_ruid);
    char *library_source = NULL;
    v3_native_load_descriptor(generation_dir, &loaded, &library_source);
    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);
    bool_t library_complete = loaded.valid &&
                              v3_native_route_use_library(
                                  library_root, library_source, &loaded);
    bool_t cache_complete = loaded.valid &&
                            v3_native_cache_files_complete(&loaded);
    osFreeMem(library_source);
    if (!loaded.valid || (!library_complete && !cache_complete))
    {
        mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);
        v3_native_route_clear(&loaded);
        osFreeMem(manifest_data);
        return error != NO_ERROR ? error : ERROR_INVALID_FILE;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_clear(&routes[overlay_id]);
    routes[overlay_id] = loaded;
    if (library_complete)
    {
        v3_native_compact_cache_files(&routes[overlay_id]);
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);

    *data = manifest_data;
    *length = manifest_length;
    *version = marker_version;
    return NO_ERROR;
}

bool_t v3_native_cache_active_version(const char *cache_root,
                                      uint8_t overlay_id,
                                      const char *ruid,
                                      uint32_t *version)
{
    char canonical_ruid[TB2_RUID_SIZE];
    uint32_t active_version = 0;
    if (cache_root == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid) ||
        v3_native_read_active_marker(cache_root, overlay_id, canonical_ruid,
                                     &active_version, NULL) != NO_ERROR)
    {
        return FALSE;
    }
    if (version != NULL)
    {
        *version = active_version;
    }
    return TRUE;
}

bool_t v3_native_cache_active_is_tonieplay(const char *cache_root,
                                           const char *library_root,
                                           uint8_t overlay_id,
                                           const char *ruid)
{
    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t version = 0;
    if (v3_native_cache_read_active_manifest(cache_root, library_root,
                                             overlay_id, ruid,
                                             &manifest, &manifest_length,
                                             &version) != NO_ERROR)
    {
        return FALSE;
    }
    osFreeMem(manifest);
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    bool_t tonieplay = v3_native_route_is_tonieplay(&routes[overlay_id]);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return tonieplay;
}

bool_t v3_native_cache_active_info(const char *cache_root,
                                   const char *library_root,
                                   uint8_t overlay_id,
                                   const char *ruid,
                                   uint32_t *version,
                                   size_t *object_count,
                                   bool_t *tonieplay)
{
    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t active_version = 0;
    if (v3_native_cache_read_active_manifest(cache_root, library_root,
                                             overlay_id, ruid,
                                             &manifest, &manifest_length,
                                             &active_version) != NO_ERROR)
    {
        return FALSE;
    }
    osFreeMem(manifest);

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *route = &routes[overlay_id];
    bool_t valid = route->valid && route->active &&
                   route->version == active_version &&
                   route->chapter_count > 0;
    if (valid)
    {
        if (version != NULL)
        {
            *version = active_version;
        }
        if (object_count != NULL)
        {
            *object_count = route->chapter_count;
        }
        if (tonieplay != NULL)
        {
            *tonieplay = v3_native_route_is_tonieplay(route);
        }
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return valid;
}

static error_t v3_native_remove_tree(const char *path)
{
    if (path == NULL || !fsDirExists(path))
    {
        return NO_ERROR;
    }
    FsDir *directory = fsOpenDir(path);
    if (directory == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }
    error_t error = NO_ERROR;
    while (error == NO_ERROR)
    {
        FsDirEntry entry;
        if (fsReadDir(directory, &entry) != NO_ERROR)
        {
            break;
        }
        if (!osStrcmp(entry.name, ".") || !osStrcmp(entry.name, ".."))
        {
            continue;
        }
        char *child = v3_native_format("%s%c%s", path, PATH_SEPARATOR,
                                       entry.name);
        if (child == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
            break;
        }
        error = (entry.attributes & FS_FILE_ATTR_DIRECTORY)
                    ? v3_native_remove_tree(child)
                    : fsDeleteFile(child);
        osFreeMem(child);
    }
    fsCloseDir(directory);
    return error == NO_ERROR ? fsRemoveDir(path) : error;
}

static void v3_native_library_digest_to_hex(
    const uint8_t digest[SHA256_DIGEST_SIZE],
    char hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        hex[i * 2U] = digits[digest[i] >> 4];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0FU];
    }
    hex[SHA256_DIGEST_SIZE * 2U] = '\0';
}

static bool_t v3_native_library_hex_to_digest(
    const char hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE],
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    if (!v3_native_library_hash_is_canonical(hex))
    {
        return FALSE;
    }
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        unsigned char high = (unsigned char)hex[i * 2U];
        unsigned char low = (unsigned char)hex[i * 2U + 1U];
        high = (unsigned char)(isdigit(high) ? high - '0' : high - 'a' + 10);
        low = (unsigned char)(isdigit(low) ? low - '0' : low - 'a' + 10);
        digest[i] = (uint8_t)((high << 4) | low);
    }
    return TRUE;
}

static void v3_native_library_hash_u32(Sha256Context *context, uint32_t value)
{
    uint8_t encoded[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value,
    };
    sha256Update(context, encoded, sizeof(encoded));
}

static error_t v3_native_library_hash_file(
    const char *path,
    uint8_t digest[SHA256_DIGEST_SIZE],
    uint32_t *file_size)
{
    uint32_t size = 0;
    if (path == NULL || digest == NULL || file_size == NULL ||
        fsGetFileSize(path, &size) != NO_ERROR)
    {
        return ERROR_FILE_NOT_FOUND;
    }

    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }

    uint8_t *buffer = osAllocMem(V3_NATIVE_LIBRARY_HASH_BUFFER_SIZE);
    if (buffer == NULL)
    {
        fsCloseFile(file);
        return ERROR_OUT_OF_MEMORY;
    }

    Sha256Context context;
    uint32_t remaining = size;
    error_t error = NO_ERROR;
    sha256Init(&context);
    while (remaining > 0)
    {
        size_t requested = MIN((size_t)remaining,
                               (size_t)V3_NATIVE_LIBRARY_HASH_BUFFER_SIZE);
        size_t received = 0;
        error = fsReadFile(file, buffer, requested, &received);
        if (error != NO_ERROR || received == 0)
        {
            error = error != NO_ERROR ? error : ERROR_READ_FAILED;
            break;
        }
        sha256Update(&context, buffer, received);
        remaining -= (uint32_t)received;
    }
    fsCloseFile(file);
    osFreeMem(buffer);
    if (error != NO_ERROR)
    {
        return error;
    }
    sha256Final(&context, digest);
    *file_size = size;
    return NO_ERROR;
}

static char *v3_native_library_chapter_name(
    const v3_native_library_chapter_t *chapter,
    size_t index)
{
    return chapter != NULL
               ? v3_native_format("teddycloud_%s_%02" PRIuSIZE ".opus",
                                  chapter->sha256_hex, index)
               : NULL;
}

static error_t v3_native_library_prepare_chapters(
    const char *source_dir,
    const v3_native_chapter_t *source_chapters,
    size_t chapter_count,
    v3_native_library_chapter_t **prepared_chapters,
    char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE])
{
    if (source_dir == NULL || source_chapters == NULL || chapter_count == 0 ||
        chapter_count > INT_MAX || prepared_chapters == NULL ||
        content_hash == NULL ||
        chapter_count > SIZE_MAX / sizeof(v3_native_library_chapter_t))
    {
        return ERROR_INVALID_PARAMETER;
    }

    v3_native_library_chapter_t *prepared =
        osAllocMem(chapter_count * sizeof(*prepared));
    if (prepared == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(prepared, 0, chapter_count * sizeof(*prepared));

    error_t error = NO_ERROR;
    for (size_t i = 0; error == NO_ERROR && i < chapter_count; i++)
    {
        char *source = v3_native_format("%s%cchapters%c%s", source_dir,
                                        PATH_SEPARATOR, PATH_SEPARATOR,
                                        source_chapters[i].name);
        uint32_t actual_size = 0;
        error = source == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_library_hash_file(source, prepared[i].sha256,
                                                  &actual_size);
        osFreeMem(source);
        if (error == NO_ERROR && actual_size != source_chapters[i].file_size)
        {
            error = ERROR_INVALID_FILE;
        }
        if (error == NO_ERROR)
        {
            osStrcpy(prepared[i].original_name, source_chapters[i].name);
            prepared[i].file_size = actual_size;
            v3_native_library_digest_to_hex(prepared[i].sha256,
                                            prepared[i].sha256_hex);
        }
    }

    if (error == NO_ERROR)
    {
        Sha256Context context;
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256Init(&context);
        sha256Update(&context, V3_NATIVE_LIBRARY_HASH_DOMAIN,
                     sizeof(V3_NATIVE_LIBRARY_HASH_DOMAIN) - 1U);
        v3_native_library_hash_u32(&context, (uint32_t)chapter_count);
        for (size_t i = 0; i < chapter_count; i++)
        {
            v3_native_library_hash_u32(&context, (uint32_t)i);
            v3_native_library_hash_u32(&context, prepared[i].file_size);
            sha256Update(&context, prepared[i].sha256, SHA256_DIGEST_SIZE);
        }
        sha256Final(&context, digest);
        v3_native_library_digest_to_hex(digest, content_hash);
    }

    if (error != NO_ERROR)
    {
        osFreeMem(prepared);
        return error;
    }
    *prepared_chapters = prepared;
    return NO_ERROR;
}

static cJSON *v3_native_library_origin_json(uint8_t overlay_id,
                                            const char *ruid,
                                            uint32_t version)
{
    cJSON *origin = cJSON_CreateObject();
    if (origin == NULL ||
        cJSON_AddNumberToObject(origin, "overlay", overlay_id) == NULL ||
        cJSON_AddStringToObject(origin, "ruid", ruid) == NULL ||
        cJSON_AddNumberToObject(origin, "contentVersion", version) == NULL)
    {
        cJSON_Delete(origin);
        return NULL;
    }
    return origin;
}

static char *v3_native_library_entry_json(
    const char *content_hash,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const v3_native_library_chapter_t *chapters,
    size_t chapter_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = NULL;
    cJSON *origins = NULL;
    cJSON *origin = NULL;
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "schemaVersion", V3_NATIVE_LIBRARY_SCHEMA) == NULL ||
        cJSON_AddStringToObject(root, "origin", "tonies") == NULL ||
        cJSON_AddStringToObject(root, "boxGeneration", "tb2") == NULL ||
        cJSON_AddStringToObject(root, "format", "ogg-opus") == NULL ||
        cJSON_AddStringToObject(root, "contentHash", content_hash) == NULL ||
        (array = cJSON_AddArrayToObject(root, "chapters")) == NULL ||
        (origins = cJSON_AddArrayToObject(root, "origins")) == NULL ||
        (origin = v3_native_library_origin_json(overlay_id, ruid, version)) == NULL ||
        !cJSON_AddItemToArray(origins, origin))
    {
        cJSON_Delete(origin);
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < chapter_count; i++)
    {
        cJSON *entry = cJSON_CreateObject();
        char *name = v3_native_library_chapter_name(&chapters[i], i);
        char *relative_path = name != NULL
                                  ? v3_native_format("chapters/%s", name)
                                  : NULL;
        if (entry == NULL || name == NULL || relative_path == NULL ||
            cJSON_AddNumberToObject(entry, "index", i) == NULL ||
            cJSON_AddStringToObject(entry, "originalName",
                                    chapters[i].original_name) == NULL ||
            cJSON_AddStringToObject(entry, "sha256",
                                    chapters[i].sha256_hex) == NULL ||
            cJSON_AddNumberToObject(entry, "fileSize",
                                    chapters[i].file_size) == NULL ||
            cJSON_AddStringToObject(entry, "path", relative_path) == NULL ||
            !cJSON_AddItemToArray(array, entry))
        {
            osFreeMem(relative_path);
            osFreeMem(name);
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return NULL;
        }
        osFreeMem(relative_path);
        osFreeMem(name);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static error_t v3_native_library_write_entry(
    const char *collection_dir,
    const char *content_hash,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const v3_native_library_chapter_t *chapters,
    size_t chapter_count)
{
    char *json = v3_native_library_entry_json(content_hash, overlay_id, ruid,
                                              version, chapters,
                                              chapter_count);
    char *path = collection_dir != NULL
                     ? v3_native_format("%s%clibrary-entry.json", collection_dir,
                                        PATH_SEPARATOR)
                     : NULL;
    error_t error = json == NULL || path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_write_atomic(path, json, osStrlen(json));
    cJSON_free(json);
    osFreeMem(path);
    return error;
}

static bool_t v3_native_library_json_string_equals(const cJSON *item,
                                                   const char *expected)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           !osStrcmp(item->valuestring, expected);
}

static bool_t v3_native_library_entry_matches(
    const char *collection_dir,
    const char *source_dir,
    const char *content_hash,
    const v3_native_library_chapter_t *chapters,
    size_t chapter_count)
{
    char *entry_path = v3_native_format("%s%clibrary-entry.json",
                                        collection_dir, PATH_SEPARATOR);
    uint8_t *entry_data = NULL;
    size_t entry_length = 0;
    error_t error = entry_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(entry_path, &entry_data,
                                              &entry_length);
    osFreeMem(entry_path);
    if (error != NO_ERROR)
    {
        osFreeMem(entry_data);
        return FALSE;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)entry_data,
                                            entry_length, &end, 0);
    uint32_t schema = 0;
    cJSON *entries = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "chapters")
                         : NULL;
    cJSON *origins = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "origins")
                         : NULL;
    bool_t valid = root != NULL &&
                   end == (const char *)entry_data + entry_length &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(root,
                                                                       "schemaVersion"),
                                      &schema, FALSE) &&
                   schema == V3_NATIVE_LIBRARY_SCHEMA &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "origin"),
                       "tonies") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "boxGeneration"),
                       "tb2") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "format"),
                       "ogg-opus") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "contentHash"),
                       content_hash) &&
                   cJSON_IsArray(entries) &&
                   cJSON_GetArraySize(entries) == (int)chapter_count &&
                   cJSON_IsArray(origins);

    for (size_t i = 0; valid && i < chapter_count; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(entries, (int)i);
        uint32_t index = 0;
        uint32_t file_size = 0;
        char *name = v3_native_library_chapter_name(&chapters[i], i);
        char *relative_path = name != NULL
                                  ? v3_native_format("chapters/%s", name)
                                  : NULL;
        valid = entry != NULL && name != NULL && relative_path != NULL &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(entry,
                                                                    "index"),
                                   &index, TRUE) &&
                index == i &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(entry,
                                                                    "fileSize"),
                                   &file_size, FALSE) &&
                file_size == chapters[i].file_size &&
                v3_native_library_json_string_equals(
                    cJSON_GetObjectItemCaseSensitive(entry, "sha256"),
                    chapters[i].sha256_hex) &&
                v3_native_library_json_string_equals(
                    cJSON_GetObjectItemCaseSensitive(entry, "path"),
                    relative_path);
        if (valid)
        {
            char *source = v3_native_format("%s%cchapters%c%s", source_dir,
                                            PATH_SEPARATOR, PATH_SEPARATOR,
                                            chapters[i].original_name);
            char *target = v3_native_format("%s%cchapters%c%s", collection_dir,
                                            PATH_SEPARATOR, PATH_SEPARATOR,
                                            name);
            valid = source != NULL && target != NULL &&
                    fsCompareFiles(source, target, NULL) == NO_ERROR;
            osFreeMem(source);
            osFreeMem(target);
        }
        osFreeMem(relative_path);
        osFreeMem(name);
    }

    cJSON_Delete(root);
    osFreeMem(entry_data);
    return valid;
}

static bool_t v3_native_library_origin_matches(const cJSON *origin,
                                               uint8_t overlay_id,
                                               const char *ruid,
                                               uint32_t version)
{
    uint32_t stored_overlay = 0;
    uint32_t stored_version = 0;
    cJSON *stored_ruid = origin != NULL
                             ? cJSON_GetObjectItemCaseSensitive(origin, "ruid")
                             : NULL;
    return cJSON_IsObject(origin) &&
           v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(origin,
                                                               "overlay"),
                              &stored_overlay, TRUE) &&
           stored_overlay == overlay_id &&
           cJSON_IsString(stored_ruid) && stored_ruid->valuestring != NULL &&
           !osStrcasecmp(stored_ruid->valuestring, ruid) &&
           v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(origin,
                                                               "contentVersion"),
                              &stored_version, FALSE) &&
           stored_version == version;
}

static error_t v3_native_library_add_origin(const char *collection_dir,
                                             uint8_t overlay_id,
                                             const char *ruid,
                                             uint32_t version)
{
    char *entry_path = v3_native_format("%s%clibrary-entry.json",
                                        collection_dir, PATH_SEPARATOR);
    uint8_t *entry_data = NULL;
    size_t entry_length = 0;
    error_t error = entry_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(entry_path, &entry_data,
                                              &entry_length);
    if (error != NO_ERROR)
    {
        osFreeMem(entry_path);
        osFreeMem(entry_data);
        return error;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)entry_data,
                                            entry_length, &end, 0);
    bool_t parsed_completely = root != NULL &&
                               end == (const char *)entry_data + entry_length;
    osFreeMem(entry_data);
    cJSON *origins = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "origins")
                         : NULL;
    if (!parsed_completely || !cJSON_IsArray(origins))
    {
        cJSON_Delete(root);
        osFreeMem(entry_path);
        return ERROR_INVALID_FILE;
    }

    int origin_count = cJSON_GetArraySize(origins);
    for (int i = 0; i < origin_count; i++)
    {
        if (v3_native_library_origin_matches(cJSON_GetArrayItem(origins, i),
                                             overlay_id, ruid, version))
        {
            cJSON_Delete(root);
            osFreeMem(entry_path);
            return NO_ERROR;
        }
    }

    cJSON *origin = v3_native_library_origin_json(overlay_id, ruid, version);
    if (origin == NULL || !cJSON_AddItemToArray(origins, origin))
    {
        cJSON_Delete(origin);
        cJSON_Delete(root);
        osFreeMem(entry_path);
        return ERROR_OUT_OF_MEMORY;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    error = json == NULL
                ? ERROR_OUT_OF_MEMORY
                : v3_native_write_atomic(entry_path, json, osStrlen(json));
    cJSON_free(json);
    osFreeMem(entry_path);
    return error;
}

static error_t v3_native_cache_descriptor_set_library_source_locked(
    const char *cache_root,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const char *library_source)
{
    char *generation_dir = v3_native_generation_dir(
        cache_root, "versions", overlay_id, ruid, version);
    char *descriptor_path = generation_dir != NULL
                                ? v3_native_format("%s%cdescriptor.json",
                                                   generation_dir,
                                                   PATH_SEPARATOR)
                                : NULL;
    osFreeMem(generation_dir);
    uint8_t *data = NULL;
    size_t length = 0;
    error_t error = descriptor_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(descriptor_path, &data, &length);
    if (error != NO_ERROR)
    {
        osFreeMem(descriptor_path);
        return error;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, length, &end, 0);
    bool_t parsed_completely = root != NULL &&
                               end == (const char *)data + length;
    osFreeMem(data);
    uint32_t descriptor_overlay = 0;
    uint32_t descriptor_version = 0;
    cJSON *descriptor_ruid = root != NULL
                                 ? cJSON_GetObjectItemCaseSensitive(root, "ruid")
                                 : NULL;
    bool_t valid = parsed_completely &&
                   cJSON_IsString(descriptor_ruid) &&
                   descriptor_ruid->valuestring != NULL &&
                   !osStrcasecmp(descriptor_ruid->valuestring, ruid) &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "overlay"),
                                      &descriptor_overlay, TRUE) &&
                   descriptor_overlay == overlay_id &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "version"),
                                      &descriptor_version, FALSE) &&
                   descriptor_version == version &&
                   v3_native_library_source_is_candidate(library_source);
    if (!valid)
    {
        cJSON_Delete(root);
        osFreeMem(descriptor_path);
        return ERROR_INVALID_FILE;
    }

    cJSON *source = cJSON_CreateString(library_source);
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, "librarySource");
    bool_t stored = source != NULL &&
                    (existing != NULL
                         ? cJSON_ReplaceItemInObjectCaseSensitive(
                               root, "librarySource", source)
                         : cJSON_AddItemToObject(root, "librarySource", source));
    if (!stored)
    {
        cJSON_Delete(source);
        cJSON_Delete(root);
        osFreeMem(descriptor_path);
        return ERROR_OUT_OF_MEMORY;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    error = json == NULL
                ? ERROR_OUT_OF_MEMORY
                : v3_native_write_atomic(descriptor_path, json, osStrlen(json));
    cJSON_free(json);
    osFreeMem(descriptor_path);
    return error;
}

static error_t v3_native_cache_descriptor_set_library_source(
    const char *cache_root,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const char *library_source)
{
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    error_t error = v3_native_cache_descriptor_set_library_source_locked(
        cache_root, overlay_id, ruid, version, library_source);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return error;
}

error_t v3_native_cache_active_library_source(const char *cache_root,
                                              const char *library_root,
                                              uint8_t overlay_id,
                                              const char *ruid,
                                              uint32_t version,
                                              char **library_source)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (cache_root == NULL || library_root == NULL || library_source == NULL ||
        overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }
    *library_source = NULL;

    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t active_version = 0;
    error_t error = v3_native_cache_read_active_manifest(
        cache_root, library_root, overlay_id, canonical_ruid, &manifest,
        &manifest_length, &active_version);
    osFreeMem(manifest);
    if (error != NO_ERROR || (version != 0 && active_version != version))
    {
        return error != NO_ERROR ? error : ERROR_FILE_NOT_FOUND;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *route = &routes[overlay_id];
    bool_t valid = route->valid && route->active &&
                   route->version == active_version &&
                   !osStrcasecmp(route->ruid, canonical_ruid) &&
                   v3_native_library_paths_complete(route);
    if (valid)
    {
        *library_source = strdup(route->library_source);
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return !valid ? ERROR_FILE_NOT_FOUND
                  : (*library_source != NULL ? NO_ERROR
                                             : ERROR_OUT_OF_MEMORY);
}

static error_t v3_native_cache_link_library_source(
    const char *cache_root,
    const char *library_root,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const char *library_source)
{
    error_t error = v3_native_cache_descriptor_set_library_source(
        cache_root, overlay_id, ruid, version, library_source);
    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t active_version = 0;
    if (error == NO_ERROR)
    {
        error = v3_native_cache_read_active_manifest(
            cache_root, library_root, overlay_id, ruid, &manifest,
            &manifest_length, &active_version);
    }
    osFreeMem(manifest);
    if (error == NO_ERROR && active_version != version)
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        const v3_native_route_t *route = &routes[overlay_id];
        bool_t linked = route->valid && route->active &&
                        route->version == version &&
                        !osStrcasecmp(route->ruid, ruid) &&
                        route->library_source != NULL &&
                        !osStrcmp(route->library_source, library_source) &&
                        v3_native_library_paths_complete(route);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        if (!linked)
        {
            error = ERROR_INVALID_FILE;
        }
    }
    return error;
}

error_t v3_native_cache_import_active_library(const char *cache_root,
                                               const char *library_root,
                                               uint8_t overlay_id,
                                               const char *ruid,
                                               char **library_source)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (library_source != NULL)
    {
        *library_source = NULL;
    }
    if (cache_root == NULL || library_root == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t version = 0;
    error_t error = v3_native_cache_read_active_manifest(
        cache_root, library_root, overlay_id, canonical_ruid, &manifest,
        &manifest_length, &version);
    if (error != NO_ERROR)
    {
        return error;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *loaded_route = &routes[overlay_id];
    bool_t already_linked = loaded_route->valid && loaded_route->active &&
                            loaded_route->version == version &&
                            !osStrcasecmp(loaded_route->ruid, canonical_ruid) &&
                            v3_native_library_paths_complete(loaded_route);
    char *linked_source = already_linked
                              ? strdup(loaded_route->library_source)
                              : NULL;
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    if (already_linked)
    {
        osFreeMem(manifest);
        if (linked_source == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        if (library_source != NULL)
        {
            *library_source = linked_source;
        }
        else
        {
            osFreeMem(linked_source);
        }
        return NO_ERROR;
    }
    uint32_t parsed_version = 0;
    char manifest_content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE] = {0};
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error = v3_native_parse_manifest(manifest, manifest_length, &parsed_version,
                                     manifest_content_type, &chapters,
                                     &chapter_count);
    if (error == NO_ERROR &&
        !v3_native_objects_all_audio(chapters, chapter_count))
    {
        error = ERROR_INVALID_FILE;
    }
    if (error != NO_ERROR || parsed_version != version)
    {
        osFreeMem(chapters);
        osFreeMem(manifest);
        return error != NO_ERROR ? error : ERROR_INVALID_FILE;
    }

    char *source_dir = v3_native_generation_dir(cache_root, "versions",
                                                overlay_id, canonical_ruid,
                                                version);
    v3_native_library_chapter_t *library_chapters = NULL;
    char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE] = {0};
    error = source_dir == NULL
                ? ERROR_OUT_OF_MEMORY
                : v3_native_library_prepare_chapters(
                      source_dir, chapters, chapter_count, &library_chapters,
                      content_hash);
    if (error != NO_ERROR)
    {
        osFreeMem(source_dir);
        osFreeMem(chapters);
        osFreeMem(manifest);
        return error;
    }

    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);

    char *final_parent = v3_native_format(
        "%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_BY_DIR, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_CONTENT_HASH_DIR);
    char *final_dir = final_parent != NULL
                          ? v3_native_format("%s%c%s", final_parent,
                                             PATH_SEPARATOR, content_hash)
                          : NULL;
    char *stage_dir = v3_native_format(
        "%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_STAGING_DIR, PATH_SEPARATOR, content_hash);
    char *stage_chapters = stage_dir != NULL
                               ? v3_native_format("%s%cchapters", stage_dir,
                                                  PATH_SEPARATOR)
                               : NULL;
    bool_t final_published = FALSE;
    if (final_parent == NULL || final_dir == NULL || stage_dir == NULL ||
        stage_chapters == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (fsDirExists(final_dir))
    {
        if (!v3_native_library_entry_matches(final_dir, source_dir,
                                             content_hash, library_chapters,
                                             chapter_count))
        {
            error = ERROR_INVALID_FILE;
            goto cleanup;
        }
        error = v3_native_library_add_origin(final_dir, overlay_id,
                                             canonical_ruid, version);
        goto success;
    }

    error = v3_native_remove_tree(stage_dir);
    if (error == NO_ERROR)
    {
        error = v3_native_ensure_dir(stage_chapters);
    }
    for (size_t i = 0; error == NO_ERROR && i < chapter_count; i++)
    {
        char *name = v3_native_library_chapter_name(&library_chapters[i], i);
        char *source = v3_native_format("%s%cchapters%c%s", source_dir,
                                        PATH_SEPARATOR, PATH_SEPARATOR,
                                        library_chapters[i].original_name);
        char *target = name != NULL
                           ? v3_native_format("%s%c%s", stage_chapters,
                                              PATH_SEPARATOR, name)
                           : NULL;
        error = source == NULL || target == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : fsCopyFile(source, target, TRUE);
        if (error == NO_ERROR &&
            fsCompareFiles(source, target, NULL) != NO_ERROR)
        {
            error = ERROR_INVALID_FILE;
        }
        osFreeMem(name);
        osFreeMem(source);
        osFreeMem(target);
    }
    if (error == NO_ERROR)
    {
        error = v3_native_library_write_entry(stage_dir, content_hash,
                                              overlay_id, canonical_ruid,
                                              version, library_chapters,
                                              chapter_count);
    }
    if (error == NO_ERROR &&
        !v3_native_library_entry_matches(stage_dir, source_dir, content_hash,
                                         library_chapters, chapter_count))
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR)
    {
        error = v3_native_ensure_dir(final_parent);
    }
    if (error == NO_ERROR && fsDirExists(final_dir))
    {
        error = v3_native_library_entry_matches(
                    final_dir, source_dir, content_hash, library_chapters,
                    chapter_count)
                    ? v3_native_library_add_origin(final_dir, overlay_id,
                                                   canonical_ruid, version)
                    : ERROR_INVALID_FILE;
        v3_native_remove_tree(stage_dir);
    }
    else if (error == NO_ERROR)
    {
        error = fsRenameFile(stage_dir, final_dir);
        final_published = error == NO_ERROR;
    }
    if (error == NO_ERROR &&
        !v3_native_library_entry_matches(final_dir, source_dir, content_hash,
                                         library_chapters, chapter_count))
    {
        error = ERROR_INVALID_FILE;
        if (final_published)
        {
            v3_native_remove_tree(final_dir);
        }
    }

success:
    if (error == NO_ERROR)
    {
        TRACE_INFO("Imported TB2 V3 library contentHash=%s overlay=%u rUID=%s version=%" PRIu32 " chapters=%" PRIuSIZE "\r\n",
                   content_hash, (unsigned)overlay_id, canonical_ruid, version,
                   chapter_count);
    }
    else
    {
        v3_native_remove_tree(stage_dir);
    }

cleanup:
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);
    osFreeMem(stage_chapters);
    osFreeMem(stage_dir);
    osFreeMem(final_dir);
    osFreeMem(final_parent);
    osFreeMem(library_chapters);
    osFreeMem(source_dir);
    osFreeMem(chapters);
    osFreeMem(manifest);
    if (error == NO_ERROR)
    {
        char *source = v3_native_format(
            "lib://by/contentHash/%s/library-entry.json", content_hash);
        if (source == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            error = v3_native_cache_link_library_source(
                cache_root, library_root, overlay_id, canonical_ruid, version,
                source);
            if (error == NO_ERROR && library_source != NULL)
            {
                *library_source = source;
                source = NULL;
            }
            osFreeMem(source);
        }
    }
    return error;
}

static bool_t v3_native_library_hash_is_canonical(const char *value)
{
    if (value == NULL || osStrlen(value) != V3_NATIVE_LIBRARY_HASH_HEX_LENGTH)
    {
        return FALSE;
    }
    for (size_t i = 0; i < V3_NATIVE_LIBRARY_HASH_HEX_LENGTH; i++)
    {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
        {
            return FALSE;
        }
    }
    return TRUE;
}

bool_t v3_native_library_source_is_candidate(const char *source)
{
    size_t prefix_length = sizeof(V3_NATIVE_LIBRARY_SOURCE_PREFIX) - 1U;
    size_t suffix_length = sizeof(V3_NATIVE_LIBRARY_SOURCE_SUFFIX) - 1U;
    size_t expected_length = prefix_length + V3_NATIVE_LIBRARY_HASH_HEX_LENGTH +
                             suffix_length;
    if (source == NULL || osStrlen(source) != expected_length ||
        osStrncmp(source, V3_NATIVE_LIBRARY_SOURCE_PREFIX, prefix_length) ||
        osStrcmp(source + prefix_length + V3_NATIVE_LIBRARY_HASH_HEX_LENGTH,
                 V3_NATIVE_LIBRARY_SOURCE_SUFFIX))
    {
        return FALSE;
    }
    for (size_t i = 0; i < V3_NATIVE_LIBRARY_HASH_HEX_LENGTH; i++)
    {
        char value = source[prefix_length + i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f')))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t v3_native_descriptor_references_library(
    const char *cache_root,
    const char *generation_dir,
    const char *library_source,
    uint8_t *overlay_id,
    char ruid[TB2_RUID_SIZE],
    uint32_t *version)
{
    char *descriptor_path = v3_native_format("%s%cdescriptor.json",
                                              generation_dir,
                                              PATH_SEPARATOR);
    uint8_t *data = NULL;
    size_t length = 0;
    if (descriptor_path == NULL ||
        v3_native_read_file(descriptor_path, &data, &length) != NO_ERROR)
    {
        osFreeMem(descriptor_path);
        return FALSE;
    }
    osFreeMem(descriptor_path);

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, length, &end, 0);
    cJSON *source = root != NULL
                        ? cJSON_GetObjectItemCaseSensitive(root, "librarySource")
                        : NULL;
    cJSON *stored_ruid = root != NULL
                             ? cJSON_GetObjectItemCaseSensitive(root, "ruid")
                             : NULL;
    uint32_t stored_overlay = 0;
    uint32_t stored_version = 0;
    char canonical_ruid[TB2_RUID_SIZE];
    bool_t valid = root != NULL && end == (const char *)data + length &&
                   cJSON_IsString(source) && source->valuestring != NULL &&
                   !osStrcmp(source->valuestring, library_source) &&
                   cJSON_IsString(stored_ruid) &&
                   stored_ruid->valuestring != NULL &&
                   tb2_ruid_canonicalize(stored_ruid->valuestring,
                                         canonical_ruid) &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "overlay"),
                                      &stored_overlay, TRUE) &&
                   stored_overlay < MAX_OVERLAYS &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "version"),
                                      &stored_version, FALSE);
    char *expected_dir = valid
                             ? v3_native_generation_dir(
                                   cache_root, "versions",
                                   (uint8_t)stored_overlay, canonical_ruid,
                                   stored_version)
                             : NULL;
    valid = valid && expected_dir != NULL &&
            !osStrcmp(expected_dir, generation_dir);
    osFreeMem(expected_dir);
    cJSON_Delete(root);
    osFreeMem(data);
    if (!valid)
    {
        return FALSE;
    }
    *overlay_id = (uint8_t)stored_overlay;
    osStrcpy(ruid, canonical_ruid);
    *version = stored_version;
    return TRUE;
}

static error_t v3_native_invalidate_library_source_recursive(
    const char *cache_root,
    const char *directory,
    const char *library_source,
    unsigned depth)
{
    if (!fsDirExists(directory))
    {
        return NO_ERROR;
    }
    if (depth == 3U)
    {
        uint8_t overlay_id = 0;
        char ruid[TB2_RUID_SIZE];
        uint32_t version = 0;
        if (!v3_native_descriptor_references_library(
                cache_root, directory, library_source, &overlay_id, ruid,
                &version))
        {
            return NO_ERROR;
        }

        uint32_t active_version = 0;
        if (v3_native_read_active_marker(cache_root, overlay_id, ruid,
                                         &active_version, NULL) == NO_ERROR &&
            active_version == version)
        {
            char *marker = v3_native_active_marker_path(cache_root, overlay_id,
                                                         ruid);
            error_t error = marker == NULL
                                ? ERROR_OUT_OF_MEMORY
                                : fsDeleteFile(marker);
            osFreeMem(marker);
            if (error != NO_ERROR)
            {
                return error;
            }
        }
        v3_native_route_t *route = &routes[overlay_id];
        if (route->valid && route->version == version &&
            !osStrcasecmp(route->ruid, ruid) &&
            route->library_source != NULL &&
            !osStrcmp(route->library_source, library_source))
        {
            v3_native_route_clear(route);
        }
        TRACE_INFO("Invalidating TB2 V3 cache generation for deleted library source overlay=%u rUID=%s version=%" PRIu32 "\r\n",
                   (unsigned)overlay_id, ruid, version);
        return v3_native_remove_tree(directory);
    }

    FsDir *dir = fsOpenDir(directory);
    if (dir == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }
    error_t error = NO_ERROR;
    FsDirEntry entry;
    while (error == NO_ERROR && fsReadDir(dir, &entry) == NO_ERROR)
    {
        if (!osStrcmp(entry.name, ".") || !osStrcmp(entry.name, "..") ||
            !(entry.attributes & FS_FILE_ATTR_DIRECTORY))
        {
            continue;
        }
        char *child = v3_native_format("%s%c%s", directory, PATH_SEPARATOR,
                                       entry.name);
        error = child == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_invalidate_library_source_recursive(
                          cache_root, child, library_source, depth + 1U);
        osFreeMem(child);
    }
    fsCloseDir(dir);
    return error;
}

static error_t v3_native_cache_invalidate_library_source(
    const char *cache_root,
    const char *library_source)
{
    char *versions_root = v3_native_format("%s%c%s%cversions", cache_root,
                                           PATH_SEPARATOR,
                                           V3_NATIVE_CACHE_DIR,
                                           PATH_SEPARATOR);
    if (versions_root == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    error_t error = v3_native_invalidate_library_source_recursive(
        cache_root, versions_root, library_source, 0U);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    osFreeMem(versions_root);
    return error;
}

void v3_native_library_collection_free(
    v3_native_library_collection_t *collection)
{
    if (collection == NULL)
    {
        return;
    }
    if (collection->chapters != NULL)
    {
        for (size_t i = 0; i < collection->chapter_count; i++)
        {
            osFreeMem(collection->chapters[i].path);
        }
    }
    osFreeMem(collection->chapters);
    osFreeMem(collection->origins);
    osMemset(collection, 0, sizeof(*collection));
}

error_t v3_native_library_collection_delete(const char *library_root,
                                             const char *cache_root,
                                             const char *content_hash)
{
    if (library_root == NULL || cache_root == NULL ||
        !v3_native_library_hash_is_canonical(content_hash))
    {
        return ERROR_INVALID_PARAMETER;
    }

    char *collection_dir = v3_native_format(
        "%s%c%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_BY_DIR, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_CONTENT_HASH_DIR, PATH_SEPARATOR, content_hash);
    char *derived_taf = v3_native_format(
        "%s%ctb1-native-library%c%s.taf", cache_root, PATH_SEPARATOR,
        PATH_SEPARATOR, content_hash);
    char *library_source = v3_native_format(
        "lib://by/contentHash/%s/library-entry.json", content_hash);
    if (collection_dir == NULL || derived_taf == NULL || library_source == NULL)
    {
        osFreeMem(collection_dir);
        osFreeMem(derived_taf);
        osFreeMem(library_source);
        return ERROR_OUT_OF_MEMORY;
    }
    if (!fsDirExists(collection_dir))
    {
        osFreeMem(collection_dir);
        osFreeMem(derived_taf);
        osFreeMem(library_source);
        return ERROR_FILE_NOT_FOUND;
    }

    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);
    error_t error = v3_native_cache_invalidate_library_source(
        cache_root, library_source);
    if (error == NO_ERROR)
    {
        error = v3_native_remove_tree(collection_dir);
    }
    if (error == NO_ERROR && fsFileExists(derived_taf))
    {
        error = fsDeleteFile(derived_taf);
    }
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);

    osFreeMem(collection_dir);
    osFreeMem(derived_taf);
    osFreeMem(library_source);
    return error;
}

static uint32_t v3_native_library_audio_id(const char *content_hash)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 8U; i++)
    {
        char digit = content_hash[i];
        value = (value << 4) |
                (uint32_t)(digit <= '9' ? digit - '0' : digit - 'a' + 10);
    }
    return value == 0 ? 1U : value;
}

bool_t v3_native_library_source_audio_id(const char *source,
                                         uint32_t *audio_id)
{
    if (audio_id == NULL || !v3_native_library_source_is_candidate(source))
    {
        return FALSE;
    }
    *audio_id = v3_native_library_audio_id(
        source + sizeof(V3_NATIVE_LIBRARY_SOURCE_PREFIX) - 1U);
    return TRUE;
}

error_t v3_native_library_collection_load(
    const char *library_root,
    const char *source,
    bool_t verify_hashes,
    v3_native_library_collection_t *collection)
{
    if (library_root == NULL || collection == NULL ||
        !v3_native_library_source_is_candidate(source))
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(collection, 0, sizeof(*collection));

    size_t prefix_length = sizeof(V3_NATIVE_LIBRARY_SOURCE_PREFIX) - 1U;
    osMemcpy(collection->content_hash, source + prefix_length,
             V3_NATIVE_LIBRARY_HASH_HEX_LENGTH);
    collection->content_hash[V3_NATIVE_LIBRARY_HASH_HEX_LENGTH] = '\0';
    collection->audio_id = v3_native_library_audio_id(collection->content_hash);

    char *collection_dir = v3_native_format(
        "%s%c%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_BY_DIR, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_CONTENT_HASH_DIR, PATH_SEPARATOR,
        collection->content_hash);
    char *entry_path = collection_dir != NULL
                           ? v3_native_format("%s%clibrary-entry.json",
                                              collection_dir, PATH_SEPARATOR)
                           : NULL;
    uint8_t *entry_data = NULL;
    size_t entry_length = 0;
    error_t error = collection_dir == NULL || entry_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(entry_path, &entry_data,
                                              &entry_length);
    if (error != NO_ERROR)
    {
        goto cleanup;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)entry_data,
                                            entry_length, &end, 0);
    cJSON *chapters = root != NULL
                          ? cJSON_GetObjectItemCaseSensitive(root, "chapters")
                          : NULL;
    cJSON *origins = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "origins")
                         : NULL;
    uint32_t schema = 0;
    int chapter_count = cJSON_IsArray(chapters) ? cJSON_GetArraySize(chapters) : 0;
    int origin_count = cJSON_IsArray(origins) ? cJSON_GetArraySize(origins) : -1;
    bool_t valid = root != NULL &&
                   end == (const char *)entry_data + entry_length &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "schemaVersion"),
                                      &schema, FALSE) &&
                   schema == V3_NATIVE_LIBRARY_SCHEMA &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "boxGeneration"),
                       "tb2") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "format"),
                       "ogg-opus") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "contentHash"),
                       collection->content_hash) &&
                   chapter_count > 0 && chapter_count <= TONIEFILE_MAX_SOURCES &&
                   origin_count >= 0;
    if (!valid)
    {
        cJSON_Delete(root);
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }

    collection->chapters = osAllocMem(
        sizeof(*collection->chapters) * (size_t)chapter_count);
    if (collection->chapters == NULL)
    {
        cJSON_Delete(root);
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }
    osMemset(collection->chapters, 0,
             sizeof(*collection->chapters) * (size_t)chapter_count);
    collection->chapter_count = (size_t)chapter_count;

    if (origin_count > 0)
    {
        collection->origins = osAllocMem(
            sizeof(*collection->origins) * (size_t)origin_count);
        if (collection->origins == NULL)
        {
            cJSON_Delete(root);
            error = ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }
        osMemset(collection->origins, 0,
                 sizeof(*collection->origins) * (size_t)origin_count);
        collection->origin_count = (size_t)origin_count;

        for (size_t i = 0; i < collection->origin_count; i++)
        {
            cJSON *origin = cJSON_GetArrayItem(origins, (int)i);
            cJSON *ruid = origin != NULL
                              ? cJSON_GetObjectItemCaseSensitive(origin, "ruid")
                              : NULL;
            uint32_t overlay_id = 0;
            uint32_t content_version = 0;
            valid = cJSON_IsObject(origin) && cJSON_IsString(ruid) &&
                    ruid->valuestring != NULL &&
                    v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                           origin, "overlay"),
                                       &overlay_id, TRUE) &&
                    overlay_id < MAX_OVERLAYS &&
                    v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                           origin, "contentVersion"),
                                       &content_version, FALSE) &&
                    tb2_ruid_canonicalize(
                        ruid->valuestring, collection->origins[i].ruid);
            if (!valid)
            {
                error = ERROR_INVALID_FILE;
                break;
            }
            collection->origins[i].overlay_id = (uint8_t)overlay_id;
            collection->origins[i].content_version = content_version;
        }
        if (!valid)
        {
            cJSON_Delete(root);
            goto cleanup;
        }
    }

    for (size_t i = 0; i < collection->chapter_count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(chapters, (int)i);
        cJSON *original_name = item != NULL
                                   ? cJSON_GetObjectItemCaseSensitive(
                                         item, "originalName")
                                   : NULL;
        cJSON *sha256 = item != NULL
                            ? cJSON_GetObjectItemCaseSensitive(item, "sha256")
                            : NULL;
        cJSON *stored_path = item != NULL
                                 ? cJSON_GetObjectItemCaseSensitive(item, "path")
                                 : NULL;
        uint32_t stored_index = 0;
        uint32_t file_size = 0;
        valid = cJSON_IsString(original_name) &&
                original_name->valuestring != NULL &&
                osStrlen(original_name->valuestring) <
                    V3_NATIVE_CACHE_CHAPTER_NAME_SIZE &&
                cJSON_IsString(sha256) && sha256->valuestring != NULL &&
                v3_native_library_hash_is_canonical(sha256->valuestring) &&
                cJSON_IsString(stored_path) && stored_path->valuestring != NULL &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(item, "index"),
                                   &stored_index, TRUE) &&
                stored_index == i &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(item, "fileSize"),
                                   &file_size, FALSE);
        if (!valid)
        {
            error = ERROR_INVALID_FILE;
            break;
        }

        char *expected_name = v3_native_format(
            "teddycloud_%s_%02" PRIuSIZE ".opus", sha256->valuestring, i);
        char *expected_relative = expected_name != NULL
                                      ? v3_native_format("chapters/%s",
                                                         expected_name)
                                      : NULL;
        char *chapter_path = expected_name != NULL
                                 ? v3_native_format("%s%cchapters%c%s",
                                                    collection_dir,
                                                    PATH_SEPARATOR,
                                                    PATH_SEPARATOR,
                                                    expected_name)
                                 : NULL;
        uint32_t actual_size = 0;
        valid = expected_name != NULL && expected_relative != NULL &&
                chapter_path != NULL &&
                !osStrcmp(stored_path->valuestring, expected_relative) &&
                fsGetFileSize(chapter_path, &actual_size) == NO_ERROR &&
                actual_size == file_size;
        if (valid && verify_hashes)
        {
            uint8_t digest[SHA256_DIGEST_SIZE];
            char digest_hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
            uint32_t hashed_size = 0;
            valid = v3_native_library_hash_file(chapter_path, digest,
                                                &hashed_size) == NO_ERROR &&
                    hashed_size == file_size;
            if (valid)
            {
                v3_native_library_digest_to_hex(digest, digest_hex);
                valid = !osStrcmp(digest_hex, sha256->valuestring);
            }
        }
        osFreeMem(expected_relative);
        osFreeMem(expected_name);
        if (!valid)
        {
            osFreeMem(chapter_path);
            error = ERROR_INVALID_FILE;
            break;
        }

        collection->chapters[i].index = i;
        collection->chapters[i].file_size = file_size;
        osStrcpy(collection->chapters[i].original_name,
                 original_name->valuestring);
        osStrcpy(collection->chapters[i].sha256, sha256->valuestring);
        collection->chapters[i].path = chapter_path;
    }
    cJSON_Delete(root);

cleanup:
    osFreeMem(entry_data);
    osFreeMem(entry_path);
    osFreeMem(collection_dir);
    if (error != NO_ERROR)
    {
        v3_native_library_collection_free(collection);
    }
    return error;
}

void v3_tonieplay_library_collection_free(
    v3_tonieplay_library_collection_t *collection)
{
    if (collection == NULL)
    {
        return;
    }
    for (size_t i = 0; i < collection->object_count; i++)
    {
        osFreeMem(collection->objects[i].path);
    }
    osFreeMem(collection->objects);
    osFreeMem(collection->origins);
    osFreeMem(collection->manifest_path);
    osFreeMem(collection->manifest);
    osMemset(collection, 0, sizeof(*collection));
}

static char *v3_tonieplay_object_relative_path(
    size_t index,
    const char sha256[V3_NATIVE_LIBRARY_HASH_HEX_SIZE])
{
    return v3_native_format("objects/%" PRIuSIZE "-%s.bin", index, sha256);
}

static bool_t v3_tonieplay_optional_string(const cJSON *item,
                                           char *target,
                                           size_t capacity)
{
    if (item == NULL || cJSON_IsNull(item))
    {
        target[0] = '\0';
        return TRUE;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        osStrlen(item->valuestring) >= capacity)
    {
        return FALSE;
    }
    osStrcpy(target, item->valuestring);
    return TRUE;
}

error_t v3_tonieplay_library_collection_load(
    const char *library_root,
    const char *source,
    bool_t verify_hashes,
    v3_tonieplay_library_collection_t *collection)
{
    if (library_root == NULL || collection == NULL ||
        !v3_native_library_source_is_candidate(source))
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(collection, 0, sizeof(*collection));
    size_t prefix_length = sizeof(V3_NATIVE_LIBRARY_SOURCE_PREFIX) - 1U;
    osMemcpy(collection->content_hash, source + prefix_length,
             V3_NATIVE_LIBRARY_HASH_HEX_LENGTH);
    collection->content_hash[V3_NATIVE_LIBRARY_HASH_HEX_LENGTH] = '\0';

    char *collection_dir = v3_native_format(
        "%s%c%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_BY_DIR, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_CONTENT_HASH_DIR, PATH_SEPARATOR,
        collection->content_hash);
    char *entry_path = collection_dir != NULL
                           ? v3_native_format("%s%clibrary-entry.json",
                                              collection_dir, PATH_SEPARATOR)
                           : NULL;
    uint8_t *entry_data = NULL;
    size_t entry_length = 0;
    error_t error = collection_dir == NULL || entry_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_native_read_file(entry_path, &entry_data,
                                              &entry_length);
    if (error != NO_ERROR)
    {
        goto cleanup;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)entry_data,
                                            entry_length, &end, 0);
    cJSON *manifest = root != NULL
                          ? cJSON_GetObjectItemCaseSensitive(root, "manifest")
                          : NULL;
    cJSON *objects = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "objects")
                         : NULL;
    cJSON *origins = root != NULL
                         ? cJSON_GetObjectItemCaseSensitive(root, "origins")
                         : NULL;
    uint32_t schema = 0;
    uint32_t manifest_size = 0;
    uint32_t version = 0;
    cJSON *manifest_sha = manifest != NULL
                             ? cJSON_GetObjectItemCaseSensitive(manifest,
                                                                "sha256")
                             : NULL;
    cJSON *manifest_path = manifest != NULL
                              ? cJSON_GetObjectItemCaseSensitive(manifest,
                                                                 "path")
                              : NULL;
    int count = cJSON_IsArray(objects) ? cJSON_GetArraySize(objects) : 0;
    int origin_count = cJSON_IsArray(origins) ? cJSON_GetArraySize(origins) : -1;
    bool_t valid = root != NULL && end == (const char *)entry_data + entry_length &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "schemaVersion"),
                                      &schema, FALSE) &&
                   schema == V3_TONIEPLAY_LIBRARY_SCHEMA &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "boxGeneration"),
                       "tb2") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "format"),
                       "tonieplay-v3") &&
                   v3_native_library_json_string_equals(
                       cJSON_GetObjectItemCaseSensitive(root, "contentHash"),
                       collection->content_hash) &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          root, "contentVersion"),
                                      &version, FALSE) &&
                   cJSON_IsString(manifest_sha) &&
                   v3_native_library_hash_is_canonical(
                       manifest_sha->valuestring) &&
                   v3_native_library_json_string_equals(manifest_path,
                                                        "content-meta.json") &&
                   v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                          manifest, "fileSize"),
                                      &manifest_size, FALSE) &&
                   count > 0 && origin_count >= 0;
    if (!valid)
    {
        cJSON_Delete(root);
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }

    cJSON *content_type = cJSON_GetObjectItemCaseSensitive(root, "contentType");
    if (!v3_tonieplay_optional_string(content_type, collection->content_type,
                                      sizeof(collection->content_type)))
    {
        cJSON_Delete(root);
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }
    collection->content_version = version;
    if (origin_count > 0)
    {
        collection->origins = osAllocMem(
            sizeof(*collection->origins) * (size_t)origin_count);
        if (collection->origins == NULL)
        {
            cJSON_Delete(root);
            error = ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }
        osMemset(collection->origins, 0,
                 sizeof(*collection->origins) * (size_t)origin_count);
        collection->origin_count = (size_t)origin_count;
        for (size_t i = 0; i < collection->origin_count; i++)
        {
            cJSON *origin = cJSON_GetArrayItem(origins, (int)i);
            cJSON *ruid = origin != NULL
                              ? cJSON_GetObjectItemCaseSensitive(origin, "ruid")
                              : NULL;
            uint32_t overlay_id = 0;
            uint32_t content_version = 0;
            valid = cJSON_IsObject(origin) && cJSON_IsString(ruid) &&
                    ruid->valuestring != NULL &&
                    v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                           origin, "overlay"),
                                       &overlay_id, TRUE) &&
                    overlay_id < MAX_OVERLAYS &&
                    v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(
                                           origin, "contentVersion"),
                                       &content_version, FALSE) &&
                    tb2_ruid_canonicalize(
                        ruid->valuestring, collection->origins[i].ruid);
            if (!valid)
            {
                cJSON_Delete(root);
                error = ERROR_INVALID_FILE;
                goto cleanup;
            }
            collection->origins[i].overlay_id = (uint8_t)overlay_id;
            collection->origins[i].content_version = content_version;
        }
    }
    collection->manifest_path = v3_native_format("%s%ccontent-meta.json",
                                                  collection_dir,
                                                  PATH_SEPARATOR);
    error = collection->manifest_path == NULL
                ? ERROR_OUT_OF_MEMORY
                : v3_native_read_file(collection->manifest_path,
                                      &collection->manifest,
                                      &collection->manifest_length);
    if (error == NO_ERROR && collection->manifest_length != manifest_size)
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR && verify_hashes)
    {
        uint8_t digest[SHA256_DIGEST_SIZE];
        char digest_hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
        sha256Compute(collection->manifest, collection->manifest_length,
                      digest);
        v3_native_library_digest_to_hex(digest, digest_hex);
        if (osStrcmp(digest_hex, manifest_sha->valuestring))
        {
            error = ERROR_INVALID_FILE;
        }
    }
    if (error != NO_ERROR)
    {
        cJSON_Delete(root);
        goto cleanup;
    }

    collection->objects = osAllocMem((size_t)count * sizeof(*collection->objects));
    if (collection->objects == NULL)
    {
        cJSON_Delete(root);
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }
    osMemset(collection->objects, 0,
             (size_t)count * sizeof(*collection->objects));
    collection->object_count = (size_t)count;
    for (size_t i = 0; i < collection->object_count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(objects, (int)i);
        cJSON *name = item != NULL
                          ? cJSON_GetObjectItemCaseSensitive(item, "name")
                          : NULL;
        cJSON *sha = item != NULL
                         ? cJSON_GetObjectItemCaseSensitive(item, "sha256")
                         : NULL;
        cJSON *stored_path = item != NULL
                                 ? cJSON_GetObjectItemCaseSensitive(item, "path")
                                 : NULL;
        uint32_t index = 0;
        uint32_t size = 0;
        v3_tonieplay_library_object_t *object = &collection->objects[i];
        valid = cJSON_IsString(name) && name->valuestring != NULL &&
                v3_native_cache_chapter_name_is_safe(name->valuestring) &&
                cJSON_IsString(sha) && sha->valuestring != NULL &&
                v3_native_library_hash_is_canonical(sha->valuestring) &&
                cJSON_IsString(stored_path) && stored_path->valuestring != NULL &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(item, "index"),
                                   &index, TRUE) && index == i &&
                v3_native_json_u32(cJSON_GetObjectItemCaseSensitive(item,
                                                                    "fileSize"),
                                   &size, FALSE) &&
                v3_tonieplay_optional_string(
                    cJSON_GetObjectItemCaseSensitive(item, "type"),
                    object->type, sizeof(object->type)) &&
                v3_tonieplay_optional_string(
                    cJSON_GetObjectItemCaseSensitive(item, "filename"),
                    object->filename, sizeof(object->filename)) &&
                v3_tonieplay_optional_string(
                    cJSON_GetObjectItemCaseSensitive(item, "contentType"),
                    object->content_type, sizeof(object->content_type));
        char *expected_relative = valid
                                      ? v3_tonieplay_object_relative_path(
                                            i, sha->valuestring)
                                      : NULL;
        char *object_path = expected_relative != NULL
                                ? v3_native_format("%s%c%s", collection_dir,
                                                   PATH_SEPARATOR,
                                                   expected_relative)
                                : NULL;
        uint32_t actual_size = 0;
        valid = valid && expected_relative != NULL && object_path != NULL &&
                !osStrcmp(stored_path->valuestring, expected_relative) &&
                fsGetFileSize(object_path, &actual_size) == NO_ERROR &&
                actual_size == size;
        if (valid && verify_hashes)
        {
            uint8_t digest[SHA256_DIGEST_SIZE];
            char digest_hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
            uint32_t hashed_size = 0;
            valid = v3_native_library_hash_file(object_path, digest,
                                                &hashed_size) == NO_ERROR &&
                    hashed_size == size;
            if (valid)
            {
                v3_native_library_digest_to_hex(digest, digest_hex);
                valid = !osStrcmp(digest_hex, sha->valuestring);
            }
        }
        osFreeMem(expected_relative);
        if (!valid)
        {
            osFreeMem(object_path);
            error = ERROR_INVALID_FILE;
            break;
        }
        object->index = i;
        object->file_size = size;
        osStrcpy(object->name, name->valuestring);
        osStrcpy(object->sha256, sha->valuestring);
        if (object->content_type[0] == '\0')
        {
            osStrcpy(object->content_type,
                     !osStrcmp(object->type, "audio")
                         ? "audio/ogg"
                         : "application/octet-stream");
        }
        object->path = object_path;
    }
    if (error == NO_ERROR && verify_hashes)
    {
        Sha256Context context;
        uint8_t digest[SHA256_DIGEST_SIZE];
        char digest_hex[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
        sha256Init(&context);
        sha256Update(&context, V3_TONIEPLAY_LIBRARY_HASH_DOMAIN,
                     sizeof(V3_TONIEPLAY_LIBRARY_HASH_DOMAIN) - 1U);
        v3_native_library_hash_u32(&context,
                                   (uint32_t)collection->manifest_length);
        sha256Update(&context, collection->manifest,
                     collection->manifest_length);
        v3_native_library_hash_u32(&context,
                                   (uint32_t)collection->object_count);
        for (size_t i = 0; i < collection->object_count; i++)
        {
            uint8_t object_digest[SHA256_DIGEST_SIZE];
            if (!v3_native_library_hex_to_digest(
                    collection->objects[i].sha256, object_digest))
            {
                error = ERROR_INVALID_FILE;
                break;
            }
            v3_native_library_hash_u32(&context, (uint32_t)i);
            v3_native_library_hash_u32(&context,
                                       collection->objects[i].file_size);
            sha256Update(&context, object_digest, sizeof(object_digest));
        }
        if (error == NO_ERROR)
        {
            sha256Final(&context, digest);
            v3_native_library_digest_to_hex(digest, digest_hex);
            if (osStrcmp(digest_hex, collection->content_hash))
            {
                error = ERROR_INVALID_FILE;
            }
        }
    }
    cJSON_Delete(root);

cleanup:
    osFreeMem(entry_data);
    osFreeMem(entry_path);
    osFreeMem(collection_dir);
    if (error != NO_ERROR)
    {
        v3_tonieplay_library_collection_free(collection);
    }
    return error;
}

bool_t v3_tonieplay_library_source_version(const char *library_root,
                                            const char *source,
                                            uint32_t *version)
{
    v3_tonieplay_library_collection_t collection;
    if (version == NULL ||
        v3_tonieplay_library_collection_load(library_root, source, FALSE,
                                             &collection) != NO_ERROR)
    {
        return FALSE;
    }
    *version = collection.content_version;
    v3_tonieplay_library_collection_free(&collection);
    return TRUE;
}

static char *v3_tonieplay_library_entry_json(
    const char *content_hash,
    const char *manifest_hash,
    size_t manifest_size,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t version,
    const char *content_type,
    const uint8_t *manifest_data,
    size_t manifest_length,
    const v3_tonieplay_library_object_t *objects,
    size_t object_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *manifest = NULL;
    cJSON *array = NULL;
    cJSON *origins = NULL;
    cJSON *origin = NULL;
    cJSON *metadata = NULL;
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "schemaVersion",
                               V3_TONIEPLAY_LIBRARY_SCHEMA) == NULL ||
        cJSON_AddStringToObject(root, "origin", "tonies") == NULL ||
        cJSON_AddStringToObject(root, "boxGeneration", "tb2") == NULL ||
        cJSON_AddStringToObject(root, "format", "tonieplay-v3") == NULL ||
        cJSON_AddStringToObject(root, "contentHash", content_hash) == NULL ||
        cJSON_AddStringToObject(root, "contentType",
                               content_type != NULL ? content_type : "") == NULL ||
        cJSON_AddNumberToObject(root, "contentVersion", version) == NULL ||
        (manifest = cJSON_AddObjectToObject(root, "manifest")) == NULL ||
        cJSON_AddStringToObject(manifest, "path", "content-meta.json") == NULL ||
        cJSON_AddStringToObject(manifest, "sha256", manifest_hash) == NULL ||
        cJSON_AddNumberToObject(manifest, "fileSize", manifest_size) == NULL ||
        (array = cJSON_AddArrayToObject(root, "objects")) == NULL ||
        (origins = cJSON_AddArrayToObject(root, "origins")) == NULL ||
        (origin = v3_native_library_origin_json(overlay_id, ruid, version)) == NULL ||
        !cJSON_AddItemToArray(origins, origin))
    {
        cJSON_Delete(origin);
        cJSON_Delete(root);
        return NULL;
    }

    const char *end = NULL;
    cJSON *raw = cJSON_ParseWithLengthOpts((const char *)manifest_data,
                                           manifest_length, &end, 0);
    if (raw != NULL && end == (const char *)manifest_data + manifest_length)
    {
        static const char *keys[] = {
            "gameId", "tonieplayEngineVersion", "language",
            "tonieSalesId", "title", "name",
        };
        metadata = cJSON_AddObjectToObject(root, "metadata");
        for (size_t i = 0; metadata != NULL && i < sizeof(keys) / sizeof(keys[0]); i++)
        {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(raw, keys[i]);
            cJSON *copy = value != NULL ? cJSON_Duplicate(value, TRUE) : NULL;
            if (copy != NULL)
            {
                cJSON_AddItemToObject(metadata, keys[i], copy);
            }
        }
    }
    cJSON_Delete(raw);

    for (size_t i = 0; i < object_count; i++)
    {
        cJSON *entry = cJSON_CreateObject();
        char *relative = v3_tonieplay_object_relative_path(i,
                                                            objects[i].sha256);
        if (entry == NULL || relative == NULL ||
            cJSON_AddNumberToObject(entry, "index", i) == NULL ||
            cJSON_AddStringToObject(entry, "name", objects[i].name) == NULL ||
            (objects[i].type[0] != '\0' &&
             cJSON_AddStringToObject(entry, "type", objects[i].type) == NULL) ||
            (objects[i].filename[0] != '\0' &&
             cJSON_AddStringToObject(entry, "filename",
                                    objects[i].filename) == NULL) ||
            cJSON_AddStringToObject(entry, "sha256", objects[i].sha256) == NULL ||
            cJSON_AddNumberToObject(entry, "fileSize",
                                    objects[i].file_size) == NULL ||
            cJSON_AddStringToObject(entry, "contentType",
                                    objects[i].content_type) == NULL ||
            cJSON_AddStringToObject(entry, "path", relative) == NULL ||
            !cJSON_AddItemToArray(array, entry))
        {
            osFreeMem(relative);
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return NULL;
        }
        osFreeMem(relative);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

error_t v3_native_cache_import_active_tonieplay_library(
    const char *cache_root,
    const char *library_root,
    uint8_t overlay_id,
    const char *ruid)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (cache_root == NULL || library_root == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint8_t *manifest = NULL;
    size_t manifest_length = 0;
    uint32_t version = 0;
    error_t error = v3_native_cache_read_active_manifest(
        cache_root, library_root, overlay_id, canonical_ruid, &manifest,
        &manifest_length, &version);
    if (error != NO_ERROR)
    {
        return error;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *loaded_route = &routes[overlay_id];
    bool_t already_linked = loaded_route->valid && loaded_route->active &&
                            loaded_route->version == version &&
                            !osStrcasecmp(loaded_route->ruid, canonical_ruid) &&
                            v3_native_library_paths_complete(loaded_route);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    if (already_linked)
    {
        osFreeMem(manifest);
        return NO_ERROR;
    }

    char manifest_content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE] = {0};
    v3_native_object_t *parsed = NULL;
    size_t object_count = 0;
    uint32_t parsed_version = 0;
    error = v3_native_parse_manifest(manifest, manifest_length, &parsed_version,
                                     manifest_content_type, &parsed,
                                     &object_count);
    if (error != NO_ERROR || parsed_version != version ||
        (osStrcmp(manifest_content_type, "tonieplay") &&
         v3_native_objects_all_audio(parsed, object_count)))
    {
        osFreeMem(parsed);
        osFreeMem(manifest);
        return error != NO_ERROR ? error : ERROR_INVALID_FILE;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *active_route = &routes[overlay_id];
    if (active_route->valid && active_route->active &&
        active_route->version == version &&
        !osStrcasecmp(active_route->ruid, canonical_ruid) &&
        active_route->chapter_count == object_count)
    {
        for (size_t i = 0; i < object_count; i++)
        {
            if (!osStrcmp(active_route->chapters[i].name, parsed[i].name))
            {
                osStrcpy(parsed[i].content_type,
                         active_route->chapters[i].content_type);
            }
        }
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);

    char *source_dir = v3_native_generation_dir(cache_root, "versions",
                                                overlay_id, canonical_ruid,
                                                version);
    v3_tonieplay_library_object_t *objects =
        osAllocMem(object_count * sizeof(*objects));
    if (source_dir == NULL || objects == NULL)
    {
        osFreeMem(objects);
        osFreeMem(source_dir);
        osFreeMem(parsed);
        osFreeMem(manifest);
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(objects, 0, object_count * sizeof(*objects));
    for (size_t i = 0; error == NO_ERROR && i < object_count; i++)
    {
        char *source_path = v3_native_format("%s%cchapters%c%s", source_dir,
                                             PATH_SEPARATOR, PATH_SEPARATOR,
                                             parsed[i].name);
        uint8_t digest[SHA256_DIGEST_SIZE];
        uint32_t size = 0;
        error = source_path == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_library_hash_file(source_path, digest, &size);
        osFreeMem(source_path);
        if (error == NO_ERROR && size != parsed[i].file_size)
        {
            error = ERROR_INVALID_FILE;
        }
        if (error == NO_ERROR)
        {
            objects[i].index = i;
            objects[i].file_size = size;
            osStrcpy(objects[i].name, parsed[i].name);
            osStrcpy(objects[i].type, parsed[i].type);
            osStrcpy(objects[i].filename, parsed[i].filename);
            osStrcpy(objects[i].content_type, parsed[i].content_type);
            v3_native_library_digest_to_hex(digest, objects[i].sha256);
        }
    }

    uint8_t manifest_digest[SHA256_DIGEST_SIZE];
    char manifest_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE] = {0};
    char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE] = {0};
    if (error == NO_ERROR)
    {
        sha256Compute(manifest, manifest_length, manifest_digest);
        v3_native_library_digest_to_hex(manifest_digest, manifest_hash);
        Sha256Context context;
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256Init(&context);
        sha256Update(&context, V3_TONIEPLAY_LIBRARY_HASH_DOMAIN,
                     sizeof(V3_TONIEPLAY_LIBRARY_HASH_DOMAIN) - 1U);
        v3_native_library_hash_u32(&context, (uint32_t)manifest_length);
        sha256Update(&context, manifest, manifest_length);
        v3_native_library_hash_u32(&context, (uint32_t)object_count);
        for (size_t i = 0; i < object_count; i++)
        {
            uint8_t object_digest[SHA256_DIGEST_SIZE];
            for (size_t n = 0; n < SHA256_DIGEST_SIZE; n++)
            {
                unsigned high = (unsigned)(isdigit((unsigned char)objects[i].sha256[n * 2U])
                                               ? objects[i].sha256[n * 2U] - '0'
                                               : objects[i].sha256[n * 2U] - 'a' + 10);
                unsigned low = (unsigned)(isdigit((unsigned char)objects[i].sha256[n * 2U + 1U])
                                              ? objects[i].sha256[n * 2U + 1U] - '0'
                                              : objects[i].sha256[n * 2U + 1U] - 'a' + 10);
                object_digest[n] = (uint8_t)((high << 4) | low);
            }
            v3_native_library_hash_u32(&context, (uint32_t)i);
            v3_native_library_hash_u32(&context, objects[i].file_size);
            sha256Update(&context, object_digest, sizeof(object_digest));
        }
        sha256Final(&context, digest);
        v3_native_library_digest_to_hex(digest, content_hash);
    }

    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);
    char *final_parent = v3_native_format(
        "%s%c%s%c%s", library_root, PATH_SEPARATOR, V3_NATIVE_LIBRARY_BY_DIR,
        PATH_SEPARATOR, V3_NATIVE_LIBRARY_CONTENT_HASH_DIR);
    char *final_dir = final_parent != NULL
                          ? v3_native_format("%s%c%s", final_parent,
                                             PATH_SEPARATOR, content_hash)
                          : NULL;
    char *stage_dir = v3_native_format(
        "%s%c%s%c%s", library_root, PATH_SEPARATOR,
        V3_NATIVE_LIBRARY_STAGING_DIR, PATH_SEPARATOR, content_hash);
    char *stage_objects = stage_dir != NULL
                              ? v3_native_format("%s%cobjects", stage_dir,
                                                 PATH_SEPARATOR)
                              : NULL;
    if (error == NO_ERROR &&
        (final_parent == NULL || final_dir == NULL || stage_dir == NULL ||
         stage_objects == NULL))
    {
        error = ERROR_OUT_OF_MEMORY;
    }
    if (error == NO_ERROR && fsDirExists(final_dir))
    {
        char *source = v3_native_format(
            "lib://by/contentHash/%s/library-entry.json", content_hash);
        v3_tonieplay_library_collection_t existing;
        osMemset(&existing, 0, sizeof(existing));
        error = source != NULL
                    ? v3_tonieplay_library_collection_load(
                          library_root, source, TRUE, &existing)
                    : ERROR_OUT_OF_MEMORY;
        if (error == NO_ERROR)
        {
            v3_tonieplay_library_collection_free(&existing);
            error = v3_native_library_add_origin(
                final_dir, overlay_id, canonical_ruid, version);
        }
        osFreeMem(source);
        goto tonieplay_cleanup;
    }
    if (error == NO_ERROR)
    {
        error = v3_native_remove_tree(stage_dir);
    }
    if (error == NO_ERROR)
    {
        error = v3_native_ensure_dir(stage_objects);
    }
    char *stage_manifest = stage_dir != NULL
                               ? v3_native_format("%s%ccontent-meta.json",
                                                  stage_dir, PATH_SEPARATOR)
                               : NULL;
    if (error == NO_ERROR)
    {
        error = stage_manifest == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_write_atomic(stage_manifest, manifest,
                                             manifest_length);
    }
    for (size_t i = 0; error == NO_ERROR && i < object_count; i++)
    {
        char *relative = v3_tonieplay_object_relative_path(i,
                                                            objects[i].sha256);
        const char *filename = relative != NULL
                                   ? strrchr(relative, '/')
                                   : NULL;
        char *source_path = v3_native_format("%s%cchapters%c%s", source_dir,
                                             PATH_SEPARATOR, PATH_SEPARATOR,
                                             objects[i].name);
        char *target_path = filename != NULL
                                ? v3_native_format("%s%c%s", stage_objects,
                                                   PATH_SEPARATOR,
                                                   filename + 1)
                                : NULL;
        error = source_path == NULL || target_path == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : fsCopyFile(source_path, target_path, TRUE);
        if (error == NO_ERROR &&
            fsCompareFiles(source_path, target_path, NULL) != NO_ERROR)
        {
            error = ERROR_INVALID_FILE;
        }
        osFreeMem(target_path);
        osFreeMem(source_path);
        osFreeMem(relative);
    }
    char *entry_json = error == NO_ERROR
                           ? v3_tonieplay_library_entry_json(
                                 content_hash, manifest_hash, manifest_length,
                                 overlay_id, canonical_ruid, version,
                                 manifest_content_type, manifest,
                                 manifest_length, objects, object_count)
                           : NULL;
    char *entry_path = stage_dir != NULL
                           ? v3_native_format("%s%clibrary-entry.json",
                                              stage_dir, PATH_SEPARATOR)
                           : NULL;
    if (error == NO_ERROR)
    {
        error = entry_json == NULL || entry_path == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_write_atomic(entry_path, entry_json,
                                             osStrlen(entry_json));
    }
    if (error == NO_ERROR)
    {
        error = v3_native_ensure_dir(final_parent);
    }
    if (error == NO_ERROR)
    {
        error = fsRenameFile(stage_dir, final_dir);
    }
    if (error == NO_ERROR)
    {
        char *source = v3_native_format(
            "lib://by/contentHash/%s/library-entry.json", content_hash);
        v3_tonieplay_library_collection_t published;
        osMemset(&published, 0, sizeof(published));
        error = source != NULL
                    ? v3_tonieplay_library_collection_load(
                          library_root, source, TRUE, &published)
                    : ERROR_OUT_OF_MEMORY;
        if (error == NO_ERROR)
        {
            v3_tonieplay_library_collection_free(&published);
        }
        osFreeMem(source);
        if (error != NO_ERROR)
        {
            v3_native_remove_tree(final_dir);
        }
    }
    if (error == NO_ERROR)
    {
        TRACE_INFO("Imported TB2 Tonieplay library contentHash=%s overlay=%u rUID=%s version=%" PRIu32 " objects=%" PRIuSIZE "\r\n",
                   content_hash, (unsigned)overlay_id, canonical_ruid, version,
                   object_count);
    }

    cJSON_free(entry_json);
    osFreeMem(entry_path);
    osFreeMem(stage_manifest);
tonieplay_cleanup:
    if (error != NO_ERROR)
    {
        v3_native_remove_tree(stage_dir);
    }
    osFreeMem(stage_objects);
    osFreeMem(stage_dir);
    osFreeMem(final_dir);
    osFreeMem(final_parent);
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);
    osFreeMem(objects);
    osFreeMem(source_dir);
    osFreeMem(parsed);
    osFreeMem(manifest);
    if (error == NO_ERROR)
    {
        char *source = v3_native_format(
            "lib://by/contentHash/%s/library-entry.json", content_hash);
        error = source == NULL
                    ? ERROR_OUT_OF_MEMORY
                    : v3_native_cache_link_library_source(
                          cache_root, library_root, overlay_id,
                          canonical_ruid, version, source);
        osFreeMem(source);
    }
    return error;
}

error_t v3_tonieplay_library_activate(
    const char *library_root,
    const char *source,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t effective_version,
    uint8_t **manifest,
    size_t *manifest_length)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (library_root == NULL || source == NULL || manifest == NULL ||
        manifest_length == NULL || overlay_id >= MAX_OVERLAYS ||
        effective_version == 0 ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }
    *manifest = NULL;
    *manifest_length = 0;

    v3_tonieplay_library_collection_t collection;
    error_t error = v3_tonieplay_library_collection_load(
        library_root, source, TRUE, &collection);
    if (error != NO_ERROR)
    {
        return error;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)collection.manifest,
                                            collection.manifest_length, &end, 0);
    cJSON *version = cJSON_CreateNumber(effective_version);
    if (root == NULL || end != (const char *)collection.manifest +
                                   collection.manifest_length ||
        version == NULL ||
        !cJSON_ReplaceItemInObjectCaseSensitive(root, "version", version))
    {
        cJSON_Delete(version);
        cJSON_Delete(root);
        v3_tonieplay_library_collection_free(&collection);
        return ERROR_INVALID_FILE;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        v3_tonieplay_library_collection_free(&collection);
        return ERROR_OUT_OF_MEMORY;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_tonieplay_assigned_route_t *route = &assigned_routes[overlay_id];
    v3_tonieplay_assigned_route_clear(route);
    route->valid = TRUE;
    route->version = effective_version;
    osStrcpy(route->ruid, canonical_ruid);
    route->collection = collection;
    size_t object_count = route->collection.object_count;
    osMemset(&collection, 0, sizeof(collection));
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);

    *manifest = (uint8_t *)json;
    *manifest_length = osStrlen(json);
    TRACE_INFO("Activated local TB2 Tonieplay collection overlay=%u rUID=%s version=%" PRIu32 " objects=%" PRIuSIZE "\r\n",
               (unsigned)overlay_id, canonical_ruid, effective_version,
               object_count);
    return NO_ERROR;
}

bool_t v3_tonieplay_library_resolve(
    uint8_t overlay_id,
    const char *name,
    char **path,
    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE])
{
    if (overlay_id >= MAX_OVERLAYS || name == NULL || path == NULL ||
        content_type == NULL)
    {
        return FALSE;
    }
    *path = NULL;
    content_type[0] = '\0';
    bool_t found = FALSE;
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_tonieplay_assigned_route_t *route = &assigned_routes[overlay_id];
    if (route->valid)
    {
        for (size_t i = 0; i < route->collection.object_count; i++)
        {
            const v3_tonieplay_library_object_t *object =
                &route->collection.objects[i];
            if (!osStrcmp(object->name, name))
            {
                *path = strdup(object->path);
                osStrcpy(content_type,
                         object->content_type[0] != '\0'
                             ? object->content_type
                             : "application/octet-stream");
                found = *path != NULL;
                break;
            }
        }
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return found;
}

bool_t v3_tonieplay_library_route_active(uint8_t overlay_id,
                                         const char *ruid)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return FALSE;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    bool_t active = assigned_routes[overlay_id].valid &&
                    !osStrcasecmp(assigned_routes[overlay_id].ruid,
                                  canonical_ruid);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return active;
}

bool_t v3_tonieplay_library_route_assigned(uint8_t overlay_id)
{
    if (overlay_id >= MAX_OVERLAYS)
    {
        return FALSE;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    bool_t active = assigned_routes[overlay_id].valid;
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return active;
}

void v3_tonieplay_library_deactivate(uint8_t overlay_id)
{
    if (overlay_id >= MAX_OVERLAYS)
    {
        return;
    }
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_tonieplay_assigned_route_clear(&assigned_routes[overlay_id]);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
}

void v3_native_cache_invalidate(const char *cache_root,
                                uint8_t overlay_id,
                                const char *ruid)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (cache_root == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_t *route = &routes[overlay_id];
    if (route->valid && !osStrcasecmp(route->ruid, canonical_ruid))
    {
        v3_native_route_clear(route);
    }
    if (assigned_routes[overlay_id].valid &&
        !osStrcasecmp(assigned_routes[overlay_id].ruid, canonical_ruid))
    {
        v3_tonieplay_assigned_route_clear(&assigned_routes[overlay_id]);
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);

    char *marker_path = v3_native_active_marker_path(cache_root, overlay_id,
                                                      canonical_ruid);
    if (marker_path != NULL)
    {
        fsDeleteFile(marker_path);
        osFreeMem(marker_path);
    }
    TRACE_INFO("Invalidated TB2 V3 original cache route overlay=%u rUID=%s\r\n",
               (unsigned)overlay_id, canonical_ruid);
}

static error_t v3_native_write_active_marker(const char *cache_root,
                                             const v3_native_route_t *route)
{
    char *directory = v3_native_format("%s%c%s%cactive%c%u", cache_root,
                                       PATH_SEPARATOR, V3_NATIVE_CACHE_DIR,
                                       PATH_SEPARATOR, PATH_SEPARATOR,
                                       (unsigned)route->overlay_id);
    char *path = directory != NULL
                     ? v3_native_format("%s%c%s.json", directory, PATH_SEPARATOR, route->ruid)
                     : NULL;
    error_t error = directory == NULL || path == NULL ? ERROR_OUT_OF_MEMORY
                                                       : v3_native_ensure_dir(directory);
    char marker[160];
    int length = osSnprintf(marker, sizeof(marker),
                            "{\"schemaVersion\":%d,\"overlay\":%u,\"ruid\":\"%s\",\"version\":%" PRIu32 "}",
                            V3_NATIVE_CACHE_SCHEMA, (unsigned)route->overlay_id,
                            route->ruid, route->version);
    if (error == NO_ERROR && (length <= 0 || (size_t)length >= sizeof(marker)))
    {
        error = ERROR_FAILURE;
    }
    if (error == NO_ERROR)
    {
        error = v3_native_write_atomic(path, marker, (size_t)length);
    }
    osFreeMem(path);
    osFreeMem(directory);
    return error;
}

static error_t v3_native_activate_route(v3_native_route_t *route, const char *cache_root)
{
    if (!v3_native_cache_files_complete(route))
    {
        return ERROR_IN_PROGRESS;
    }
    char *version_dir = v3_native_generation_dir(cache_root, "versions",
                                                 route->overlay_id, route->ruid,
                                                 route->version);
    if (version_dir == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    char *parent = strdup(version_dir);
    if (parent == NULL)
    {
        osFreeMem(version_dir);
        return ERROR_OUT_OF_MEMORY;
    }
    char *separator = strrchr(parent, PATH_SEPARATOR);
    if (separator != NULL)
    {
        *separator = '\0';
    }
    error_t error = v3_native_ensure_dir(parent);
    if (error == NO_ERROR && !fsDirExists(version_dir))
    {
        error = fsRenameFile(route->generation_dir, version_dir);
    }
    if (error == NO_ERROR)
    {
        osFreeMem(route->generation_dir);
        route->generation_dir = version_dir;
        version_dir = NULL;
        route->active = TRUE;
        error = v3_native_write_active_marker(cache_root, route);
    }
    if (error == NO_ERROR)
    {
        TRACE_INFO("Activated TB2 V3 cache overlay=%u rUID=%s version=%" PRIu32 " chapters=%" PRIuSIZE "\r\n",
                   (unsigned)route->overlay_id, route->ruid, route->version,
                   route->chapter_count);
    }
    else
    {
        route->active = FALSE;
    }
    osFreeMem(parent);
    osFreeMem(version_dir);
    return error;
}

void v3_native_cache_meta_capture_init(v3_native_cache_meta_capture_t *capture,
                                       const char *cache_root,
                                       uint8_t overlay_id,
                                       const char *ruid)
{
    osMemset(capture, 0, sizeof(*capture));
    capture->cache_root = cache_root != NULL ? strdup(cache_root) : NULL;
    capture->store = TRUE;
    capture->overlay_id = overlay_id;
    if (capture->cache_root == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, capture->ruid))
    {
        capture->failed = TRUE;
    }

    if (!capture->failed)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_clear(&routes[overlay_id]);
        v3_tonieplay_assigned_route_clear(&assigned_routes[overlay_id]);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    }
}

void v3_native_cache_meta_observe_init(v3_native_cache_meta_capture_t *capture,
                                       uint8_t overlay_id,
                                       const char *ruid)
{
    osMemset(capture, 0, sizeof(*capture));
    capture->overlay_id = overlay_id;
    if (overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, capture->ruid))
    {
        capture->failed = TRUE;
        return;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_clear(&routes[overlay_id]);
    v3_tonieplay_assigned_route_clear(&assigned_routes[overlay_id]);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
}

void v3_native_cache_meta_capture_response(v3_native_cache_meta_capture_t *capture,
                                           uint32_t status_code)
{
    capture->status_code = status_code;
}

void v3_native_cache_meta_capture_append(v3_native_cache_meta_capture_t *capture,
                                         const void *data,
                                         size_t length)
{
    if (capture == NULL || capture->failed || length == 0)
    {
        return;
    }
    if (capture->length > V3_NATIVE_CACHE_META_LIMIT - length)
    {
        capture->failed = TRUE;
        return;
    }
    size_t required = capture->length + length;
    if (required > capture->capacity)
    {
        size_t capacity = capture->capacity == 0 ? 4096U : capture->capacity;
        while (capacity < required)
        {
            capacity *= 2U;
        }
        uint8_t *buffer = osAllocMem(capacity);
        if (buffer == NULL)
        {
            capture->failed = TRUE;
            return;
        }
        if (capture->length > 0)
        {
            osMemcpy(buffer, capture->data, capture->length);
        }
        osFreeMem(capture->data);
        capture->data = buffer;
        capture->capacity = capacity;
    }
    osMemcpy(capture->data + capture->length, data, length);
    capture->length += length;
}

error_t v3_native_cache_meta_capture_finish(v3_native_cache_meta_capture_t *capture)
{
    if (capture == NULL || capture->failed || capture->status_code != 200 ||
        capture->length == 0)
    {
        return ERROR_INVALID_RESPONSE;
    }
    uint32_t version = 0;
    char manifest_content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE] = {0};
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error_t error = v3_native_parse_manifest(capture->data, capture->length,
                                             &version, manifest_content_type,
                                             &chapters, &chapter_count);
    if (error == NO_ERROR && !capture->store)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_t *route = &routes[capture->overlay_id];
        v3_native_route_clear(route);
        route->valid = TRUE;
        route->overlay_id = capture->overlay_id;
        route->version = version;
        osStrcpy(route->content_type, manifest_content_type);
        route->chapters = chapters;
        route->chapter_count = chapter_count;
        osStrcpy(route->ruid, capture->ruid);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        TRACE_DEBUG("Observed TB2 V3 route overlay=%u rUID=%s version=%" PRIu32 " chapters=%" PRIuSIZE " without caching\r\n",
                    (unsigned)capture->overlay_id, capture->ruid, version,
                    chapter_count);
        return NO_ERROR;
    }
    char *stage_dir = error == NO_ERROR
                          ? v3_native_generation_dir(capture->cache_root, "staging",
                                                     capture->overlay_id, capture->ruid,
                                                     version)
                          : NULL;
    char *chapter_dir = stage_dir != NULL
                            ? v3_native_format("%s%cchapters", stage_dir, PATH_SEPARATOR)
                            : NULL;
    if (error == NO_ERROR && (stage_dir == NULL || chapter_dir == NULL))
    {
        error = ERROR_OUT_OF_MEMORY;
    }
    if (error == NO_ERROR)
    {
        error = v3_native_ensure_dir(chapter_dir);
    }
    char *manifest_path = stage_dir != NULL
                              ? v3_native_format("%s%cmanifest.json", stage_dir, PATH_SEPARATOR)
                              : NULL;
    if (error == NO_ERROR && manifest_path == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
    }
    if (error == NO_ERROR)
    {
        error = v3_native_write_atomic(manifest_path, capture->data, capture->length);
    }
    if (error == NO_ERROR)
    {
        v3_native_route_t staged;
        osMemset(&staged, 0, sizeof(staged));
        staged.valid = TRUE;
        staged.generation_dir = stage_dir;
        staged.chapters = chapters;
        staged.chapter_count = chapter_count;
        osStrcpy(staged.content_type, manifest_content_type);
        v3_native_load_descriptor(stage_dir, &staged, NULL);
        osStrcpy(manifest_content_type, staged.content_type);
        error = v3_native_write_descriptor(stage_dir, capture->overlay_id,
                                           capture->ruid, version,
                                           manifest_content_type, chapters,
                                           chapter_count);
    }
    if (error == NO_ERROR)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_t *route = &routes[capture->overlay_id];
        v3_native_route_clear(route);
        route->valid = TRUE;
        route->capture_enabled = TRUE;
        route->overlay_id = capture->overlay_id;
        route->version = version;
        osStrcpy(route->content_type, manifest_content_type);
        route->generation_dir = stage_dir;
        route->chapters = chapters;
        route->chapter_count = chapter_count;
        osStrcpy(route->ruid, capture->ruid);
        stage_dir = NULL;
        chapters = NULL;

        char *version_dir = v3_native_generation_dir(capture->cache_root, "versions",
                                                     route->overlay_id, route->ruid,
                                                     route->version);
        if (version_dir != NULL && fsDirExists(version_dir))
        {
            osFreeMem(route->generation_dir);
            route->generation_dir = version_dir;
            v3_native_load_descriptor(route->generation_dir, route, NULL);
            route->active = v3_native_cache_files_complete(route);
            if (!route->active)
            {
                error = ERROR_INVALID_FILE;
            }
        }
        else
        {
            osFreeMem(version_dir);
        }
        if (error == NO_ERROR && !route->active &&
            v3_native_cache_files_complete(route))
        {
            error = v3_native_activate_route(route, capture->cache_root);
        }
        else if (error == NO_ERROR && route->active)
        {
            error = v3_native_write_active_marker(capture->cache_root, route);
        }
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    }
    if (error == NO_ERROR)
    {
        TRACE_INFO("Staged TB2 V3 cache overlay=%u rUID=%s version=%" PRIu32 " chapters=%" PRIuSIZE "\r\n",
                   (unsigned)capture->overlay_id, capture->ruid, version,
                   chapter_count);
    }
    osFreeMem(manifest_path);
    osFreeMem(chapter_dir);
    osFreeMem(stage_dir);
    osFreeMem(chapters);
    return error;
}

void v3_native_cache_meta_capture_abort(v3_native_cache_meta_capture_t *capture)
{
    if (capture == NULL)
    {
        return;
    }
    osFreeMem(capture->cache_root);
    osFreeMem(capture->data);
    osMemset(capture, 0, sizeof(*capture));
}

error_t v3_native_cache_download_plan_get(uint8_t overlay_id,
                                          const char *ruid,
                                          v3_native_cache_download_plan_t *plan)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (plan == NULL || overlay_id >= MAX_OVERLAYS ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(plan, 0, sizeof(*plan));

    error_t error = NO_ERROR;
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    const v3_native_route_t *route = &routes[overlay_id];
    if (!route->valid || osStrcasecmp(route->ruid, canonical_ruid) ||
        route->chapter_count == 0)
    {
        error = ERROR_NOT_FOUND;
    }
    else
    {
        plan->objects = osAllocMem(route->chapter_count * sizeof(*plan->objects));
        if (plan->objects == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            osMemset(plan->objects, 0,
                     route->chapter_count * sizeof(*plan->objects));
            for (size_t i = 0; i < route->chapter_count; i++)
            {
                osStrcpy(plan->objects[i].name, route->chapters[i].name);
                osStrcpy(plan->objects[i].auth, route->chapters[i].auth);
                osStrcpy(plan->objects[i].type, route->chapters[i].type);
                osStrcpy(plan->objects[i].filename,
                         route->chapters[i].filename);
                osStrcpy(plan->objects[i].content_type,
                         route->chapters[i].content_type);
                plan->objects[i].file_size = route->chapters[i].file_size;
            }
            if (error == NO_ERROR)
            {
                osStrcpy(plan->ruid, route->ruid);
                plan->version = route->version;
                osStrcpy(plan->content_type, route->content_type);
                plan->object_count = route->chapter_count;
                plan->chapters = plan->objects;
                plan->chapter_count = plan->object_count;
            }
        }
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);

    if (error != NO_ERROR)
    {
        v3_native_cache_download_plan_free(plan);
    }
    return error;
}

void v3_native_cache_download_plan_free(v3_native_cache_download_plan_t *plan)
{
    if (plan == NULL)
    {
        return;
    }
    osFreeMem(plan->objects);
    osMemset(plan, 0, sizeof(*plan));
}

static void v3_native_capture_paths_free(
    v3_native_cache_chapter_capture_t *capture,
    bool_t delete_temporary)
{
    if (capture->file != NULL)
    {
        fsCloseFile(capture->file);
        capture->file = NULL;
    }
    if (delete_temporary && capture->temp_path != NULL)
    {
        fsDeleteFile(capture->temp_path);
    }
    osFreeMem(capture->cache_root);
    osFreeMem(capture->stage_dir);
    osFreeMem(capture->temp_path);
    osFreeMem(capture->final_path);
    capture->cache_root = NULL;
    capture->stage_dir = NULL;
    capture->temp_path = NULL;
    capture->final_path = NULL;
}

v3_native_cache_chapter_action_t v3_native_cache_chapter_prepare(
    const char *cache_root,
    const char *library_root,
    uint8_t overlay_id,
    const char *name,
    v3_native_cache_chapter_capture_t *capture,
    char **serve_path)
{
    if (capture == NULL || serve_path == NULL)
    {
        return V3_NATIVE_CHAPTER_REJECT;
    }
    osMemset(capture, 0, sizeof(*capture));
    *serve_path = NULL;
    if (cache_root == NULL || library_root == NULL ||
        overlay_id >= MAX_OVERLAYS ||
        !v3_native_cache_chapter_name_is_safe(name))
    {
        return V3_NATIVE_CHAPTER_BYPASS;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_t *route = &routes[overlay_id];
    size_t index = route->chapter_count;
    if (route->valid)
    {
        for (size_t i = 0; i < route->chapter_count; i++)
        {
            if (!osStrcmp(route->chapters[i].name, name))
            {
                index = i;
                break;
            }
        }
    }
    if (!route->valid || index == route->chapter_count)
    {
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_BYPASS;
    }
    capture->overlay_id = overlay_id;
    capture->version = route->version;
    capture->object_index = index;
    osStrcpy(capture->ruid, route->ruid);
    osStrcpy(capture->name, name);
    osStrcpy(capture->content_type, route->chapters[index].content_type);
    if (!route->capture_enabled)
    {
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }

    if (route->active && route->library_source != NULL &&
        !v3_native_library_paths_complete(route))
    {
        if (v3_native_cache_files_complete(route))
        {
            TRACE_WARNING("TB2 V3 library backing unavailable; using complete cache copy overlay=%u rUID=%s version=%" PRIu32 "\r\n",
                          (unsigned)overlay_id, route->ruid, route->version);
            v3_native_route_clear_library_backing(route);
        }
        else
        {
            mutex_unlock(MUTEX_V3_NATIVE_CACHE);
            return V3_NATIVE_CHAPTER_FORWARD;
        }
    }

    char *path = route->active && route->library_source != NULL
                     ? strdup(route->library_paths[index])
                     : v3_native_format("%s%cchapters%c%s",
                                        route->generation_dir,
                                        PATH_SEPARATOR, PATH_SEPARATOR, name);
    uint32_t existing_size = 0;
    bool_t complete_file = path != NULL &&
                           fsGetFileSize(path, &existing_size) == NO_ERROR &&
                           existing_size == route->chapters[index].file_size;
    if (route->active)
    {
        if (complete_file)
        {
            *serve_path = path;
            mutex_unlock(MUTEX_V3_NATIVE_CACHE);
            return V3_NATIVE_CHAPTER_SERVE;
        }
        osFreeMem(path);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }

    if (complete_file)
    {
        osFreeMem(path);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_STAGED;
    }
    if (route->chapters[index].capturing)
    {
        osFreeMem(path);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }

    capture->cache_root = strdup(cache_root);
    capture->stage_dir = strdup(route->generation_dir);
    capture->final_path = path;
    capture->temp_path = path != NULL ? v3_native_format("%s.part", path) : NULL;
    capture->expected_size = route->chapters[index].file_size;
    if (capture->cache_root == NULL || capture->stage_dir == NULL ||
        capture->final_path == NULL || capture->temp_path == NULL)
    {
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        v3_native_capture_paths_free(capture, TRUE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }
    fsDeleteFile(capture->temp_path);
    capture->file = fsOpenFile(capture->temp_path,
                               FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
    if (capture->file != NULL)
    {
        route->chapters[index].capturing = TRUE;
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    if (capture->file == NULL)
    {
        v3_native_capture_paths_free(capture, TRUE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }
    return V3_NATIVE_CHAPTER_CAPTURE;
}

void v3_native_cache_object_content_type(
    v3_native_cache_chapter_capture_t *capture,
    const char *content_type)
{
    if (capture == NULL || content_type == NULL || content_type[0] == '\0' ||
        osStrlen(content_type) >= sizeof(capture->content_type))
    {
        return;
    }
    osStrcpy(capture->content_type, content_type);
}

bool_t v3_native_cache_route_matches(uint8_t overlay_id,
                                     const char *ruid,
                                     uint32_t version,
                                     const char *name)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (overlay_id >= MAX_OVERLAYS || name == NULL ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return FALSE;
    }

    bool_t matches = FALSE;
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_t *route = &routes[overlay_id];
    if (route->valid && route->version == version &&
        !osStrcasecmp(route->ruid, canonical_ruid))
    {
        for (size_t i = 0; i < route->chapter_count; i++)
        {
            if (!osStrcmp(route->chapters[i].name, name))
            {
                matches = TRUE;
                break;
            }
        }
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return matches;
}

bool_t v3_native_cache_route_version(uint8_t overlay_id,
                                     const char *ruid,
                                     uint32_t *version)
{
    char canonical_ruid[TB2_RUID_SIZE];
    if (overlay_id >= MAX_OVERLAYS || version == NULL ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return FALSE;
    }

    bool_t found = FALSE;
    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_t *route = &routes[overlay_id];
    if (route->valid && !osStrcasecmp(route->ruid, canonical_ruid))
    {
        *version = route->version;
        found = TRUE;
    }
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    return found;
}

void v3_native_cache_chapter_append(v3_native_cache_chapter_capture_t *capture,
                                    const void *data,
                                    size_t length)
{
    if (capture == NULL || capture->file == NULL || capture->failed || length == 0)
    {
        return;
    }
    if (capture->written > UINT32_MAX - length ||
        fsWriteFile(capture->file, (void *)data, length) != NO_ERROR)
    {
        capture->failed = TRUE;
        return;
    }
    capture->written += (uint32_t)length;
}

error_t v3_native_cache_chapter_finish(v3_native_cache_chapter_capture_t *capture)
{
    if (capture == NULL || capture->file == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    error_t error = capture->failed ? ERROR_WRITE_FAILED : fsFlushFile(capture->file);
    fsCloseFile(capture->file);
    capture->file = NULL;
    if (error == NO_ERROR && capture->written != capture->expected_size)
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR)
    {
        fsDeleteFile(capture->final_path);
        error = fsRenameFile(capture->temp_path, capture->final_path);
    }
    if (error == NO_ERROR)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_t *route = &routes[capture->overlay_id];
        if (route->valid && route->version == capture->version &&
            capture->object_index < route->chapter_count &&
            !osStrcmp(route->chapters[capture->object_index].name,
                      capture->name))
        {
            route->chapters[capture->object_index].capturing = FALSE;
            osStrcpy(route->chapters[capture->object_index].content_type,
                     capture->content_type[0] != '\0'
                         ? capture->content_type
                         : "application/octet-stream");
            error = v3_native_write_descriptor(
                route->generation_dir, route->overlay_id, route->ruid,
                route->version, route->content_type, route->chapters,
                route->chapter_count);
        }
        if (!route->valid || route->active || route->version != capture->version ||
            osStrcmp(route->ruid, capture->ruid) ||
            osStrcmp(route->generation_dir, capture->stage_dir))
        {
            error = ERROR_ABORTED;
        }
        else if (v3_native_cache_files_complete(route))
        {
            error = v3_native_activate_route(route, capture->cache_root);
        }
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    }
    if (error != NO_ERROR && capture->temp_path != NULL)
    {
        fsDeleteFile(capture->temp_path);
    }
    return error == ERROR_IN_PROGRESS ? NO_ERROR : error;
}

void v3_native_cache_chapter_abort(v3_native_cache_chapter_capture_t *capture)
{
    if (capture == NULL)
    {
        return;
    }
    v3_native_capture_paths_free(capture, TRUE);
    if (capture->overlay_id < MAX_OVERLAYS && capture->name[0] != '\0')
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_t *route = &routes[capture->overlay_id];
        if (route->valid && route->version == capture->version &&
            capture->object_index < route->chapter_count &&
            !osStrcmp(route->chapters[capture->object_index].name,
                      capture->name))
        {
            route->chapters[capture->object_index].capturing = FALSE;
        }
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    }
    osMemset(capture, 0, sizeof(*capture));
}
