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
#define V3_NATIVE_CACHE_SCHEMA 1
#define V3_NATIVE_RESERVED_LOCAL_PREFIX "teddycloud_"
#define V3_NATIVE_LIBRARY_SCHEMA 2U
#define V3_NATIVE_LIBRARY_HASH_BUFFER_SIZE 8192U
#define V3_NATIVE_LIBRARY_BY_DIR "by"
#define V3_NATIVE_LIBRARY_CONTENT_HASH_DIR "contentHash"
#define V3_NATIVE_LIBRARY_HASH_DOMAIN "TeddyCloud TB2 library collection v1"
#define V3_NATIVE_LIBRARY_SOURCE_PREFIX "lib://by/contentHash/"
#define V3_NATIVE_LIBRARY_SOURCE_SUFFIX "/library-entry.json"

typedef struct
{
    char name[V3_NATIVE_CACHE_CHAPTER_NAME_SIZE];
    char auth[V3_NATIVE_CACHE_CHAPTER_AUTH_SIZE];
    uint32_t file_size;
} v3_native_chapter_t;

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
    char *generation_dir;
    v3_native_chapter_t *chapters;
    size_t chapter_count;
} v3_native_route_t;

static v3_native_route_t routes[MAX_OVERLAYS];

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
    osFreeMem(route->generation_dir);
    osFreeMem(route->chapters);
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

static bool_t v3_native_chapter_auth_is_safe(const char *auth)
{
    if (auth == NULL)
    {
        return FALSE;
    }
    size_t length = osStrlen(auth);
    if (length == 0 || length >= V3_NATIVE_CACHE_CHAPTER_AUTH_SIZE)
    {
        return FALSE;
    }
    for (size_t i = 0; i < length; i++)
    {
        unsigned char value = (unsigned char)auth[i];
        if (!(isalnum(value) || value == '-' || value == '_' || value == '.'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static error_t v3_native_parse_manifest(const uint8_t *data,
                                        size_t length,
                                        uint32_t *version,
                                        v3_native_chapter_t **chapters,
                                        size_t *chapter_count)
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

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (cJSON_IsString(type) && type->valuestring != NULL && !osStrcmp(type->valuestring, "audio"))
        {
            count++;
        }
    }
    if (count == 0)
    {
        cJSON_Delete(root);
        return ERROR_INVALID_FILE;
    }

    v3_native_chapter_t *parsed = osAllocMem(count * sizeof(*parsed));
    if (parsed == NULL)
    {
        cJSON_Delete(root);
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(parsed, 0, count * sizeof(*parsed));
    size_t index = 0;
    error_t error = NO_ERROR;
    cJSON_ArrayForEach(item, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(type) || type->valuestring == NULL || osStrcmp(type->valuestring, "audio"))
        {
            continue;
        }
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *auth = cJSON_GetObjectItemCaseSensitive(item, "auth");
        cJSON *file_size = cJSON_GetObjectItemCaseSensitive(item, "fileSize");
        if (!cJSON_IsString(name) || name->valuestring == NULL ||
            !v3_native_cache_chapter_name_is_safe(name->valuestring) ||
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
        if (cJSON_IsString(auth) &&
            v3_native_chapter_auth_is_safe(auth->valuestring))
        {
            osStrcpy(parsed[index].auth, auth->valuestring);
        }
        index++;
    }
    cJSON_Delete(root);
    if (error != NO_ERROR)
    {
        osFreeMem(parsed);
        return error;
    }
    *chapters = parsed;
    *chapter_count = count;
    return NO_ERROR;
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
        (array = cJSON_AddArrayToObject(root, "chapters")) == NULL)
    {
        cJSON_Delete(root);
        return ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < chapter_count; i++)
    {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddStringToObject(entry, "name", chapters[i].name) == NULL ||
            cJSON_AddNumberToObject(entry, "fileSize", chapters[i].file_size) == NULL ||
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

static bool_t v3_native_files_complete(const v3_native_route_t *route)
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

static error_t v3_native_read_active_marker(const char *cache_root,
                                            uint8_t overlay_id,
                                            const char *canonical_ruid,
                                            uint32_t *version)
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
                   schema == V3_NATIVE_CACHE_SCHEMA &&
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
    return NO_ERROR;
}

error_t v3_native_cache_read_active_manifest(const char *cache_root,
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
    error_t error = v3_native_read_active_marker(cache_root, overlay_id,
                                                  canonical_ruid,
                                                  &marker_version);
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
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error = v3_native_parse_manifest(manifest_data, manifest_length,
                                     &manifest_version, &chapters,
                                     &chapter_count);
    v3_native_route_t loaded;
    osMemset(&loaded, 0, sizeof(loaded));
    loaded.valid = error == NO_ERROR && manifest_version == marker_version;
    loaded.active = loaded.valid;
    loaded.capture_enabled = TRUE;
    loaded.overlay_id = overlay_id;
    loaded.version = marker_version;
    loaded.generation_dir = generation_dir;
    loaded.chapters = chapters;
    loaded.chapter_count = chapter_count;
    osStrcpy(loaded.ruid, canonical_ruid);
    if (!loaded.valid || !v3_native_files_complete(&loaded))
    {
        v3_native_route_clear(&loaded);
        osFreeMem(manifest_data);
        return error != NO_ERROR ? error : ERROR_INVALID_FILE;
    }

    mutex_lock(MUTEX_V3_NATIVE_CACHE);
    v3_native_route_clear(&routes[overlay_id]);
    routes[overlay_id] = loaded;
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);

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
                                     &active_version) != NO_ERROR)
    {
        return FALSE;
    }
    if (version != NULL)
    {
        *version = active_version;
    }
    return TRUE;
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
                   cJSON_IsArray(origins) && cJSON_GetArraySize(origins) > 0;

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

error_t v3_native_cache_import_active_library(const char *cache_root,
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
        cache_root, overlay_id, canonical_ruid, &manifest, &manifest_length,
        &version);
    if (error != NO_ERROR)
    {
        return error;
    }
    uint32_t parsed_version = 0;
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error = v3_native_parse_manifest(manifest, manifest_length, &parsed_version,
                                     &chapters, &chapter_count);
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
    if (collection_dir == NULL || derived_taf == NULL)
    {
        osFreeMem(collection_dir);
        osFreeMem(derived_taf);
        return ERROR_OUT_OF_MEMORY;
    }
    if (!fsDirExists(collection_dir))
    {
        osFreeMem(collection_dir);
        osFreeMem(derived_taf);
        return ERROR_FILE_NOT_FOUND;
    }

    mutex_lock(MUTEX_V3_NATIVE_LIBRARY);
    error_t error = v3_native_remove_tree(collection_dir);
    if (error == NO_ERROR && fsFileExists(derived_taf))
    {
        error = fsDeleteFile(derived_taf);
    }
    mutex_unlock(MUTEX_V3_NATIVE_LIBRARY);

    osFreeMem(collection_dir);
    osFreeMem(derived_taf);
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
    uint32_t schema = 0;
    int chapter_count = cJSON_IsArray(chapters) ? cJSON_GetArraySize(chapters) : 0;
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
                   chapter_count > 0 && chapter_count <= TONIEFILE_MAX_SOURCES;
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
    if (!v3_native_files_complete(route))
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
    v3_native_chapter_t *chapters = NULL;
    size_t chapter_count = 0;
    error_t error = v3_native_parse_manifest(capture->data, capture->length,
                                             &version, &chapters, &chapter_count);
    if (error == NO_ERROR && !capture->store)
    {
        mutex_lock(MUTEX_V3_NATIVE_CACHE);
        v3_native_route_t *route = &routes[capture->overlay_id];
        v3_native_route_clear(route);
        route->valid = TRUE;
        route->overlay_id = capture->overlay_id;
        route->version = version;
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
        error = v3_native_write_descriptor(stage_dir, capture->overlay_id,
                                           capture->ruid, version, chapters,
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
            route->active = v3_native_files_complete(route);
            if (!route->active)
            {
                error = ERROR_INVALID_FILE;
            }
        }
        else
        {
            osFreeMem(version_dir);
        }
        if (error == NO_ERROR && !route->active && v3_native_files_complete(route))
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
        plan->chapters = osAllocMem(route->chapter_count * sizeof(*plan->chapters));
        if (plan->chapters == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            osMemset(plan->chapters, 0,
                     route->chapter_count * sizeof(*plan->chapters));
            for (size_t i = 0; i < route->chapter_count; i++)
            {
                if (!v3_native_chapter_auth_is_safe(route->chapters[i].auth))
                {
                    error = ERROR_AUTH_REQUIRED;
                    break;
                }
                osStrcpy(plan->chapters[i].name, route->chapters[i].name);
                osStrcpy(plan->chapters[i].auth, route->chapters[i].auth);
                plan->chapters[i].file_size = route->chapters[i].file_size;
            }
            if (error == NO_ERROR)
            {
                osStrcpy(plan->ruid, route->ruid);
                plan->version = route->version;
                plan->chapter_count = route->chapter_count;
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
    osFreeMem(plan->chapters);
    osMemset(plan, 0, sizeof(*plan));
}

v3_native_cache_chapter_action_t v3_native_cache_chapter_prepare(
    const char *cache_root,
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
    if (cache_root == NULL || overlay_id >= MAX_OVERLAYS ||
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
    osStrcpy(capture->ruid, route->ruid);
    osStrcpy(capture->name, name);
    if (!route->capture_enabled)
    {
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_FORWARD;
    }

    char *path = v3_native_format("%s%cchapters%c%s", route->generation_dir,
                                  PATH_SEPARATOR, PATH_SEPARATOR, name);
    if (route->active)
    {
        uint32_t size = 0;
        if (path != NULL && fsGetFileSize(path, &size) == NO_ERROR &&
            size == route->chapters[index].file_size)
        {
            *serve_path = path;
            mutex_unlock(MUTEX_V3_NATIVE_CACHE);
            return V3_NATIVE_CHAPTER_SERVE;
        }
        osFreeMem(path);
        mutex_unlock(MUTEX_V3_NATIVE_CACHE);
        return V3_NATIVE_CHAPTER_REJECT;
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
        v3_native_cache_chapter_abort(capture);
        return V3_NATIVE_CHAPTER_REJECT;
    }
    fsDeleteFile(capture->temp_path);
    capture->file = fsOpenFile(capture->temp_path,
                               FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
    mutex_unlock(MUTEX_V3_NATIVE_CACHE);
    if (capture->file == NULL)
    {
        v3_native_cache_chapter_abort(capture);
        return V3_NATIVE_CHAPTER_REJECT;
    }
    return V3_NATIVE_CHAPTER_CAPTURE;
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
        if (!route->valid || route->active || route->version != capture->version ||
            osStrcmp(route->ruid, capture->ruid) ||
            osStrcmp(route->generation_dir, capture->stage_dir))
        {
            error = ERROR_ABORTED;
        }
        else if (v3_native_files_complete(route))
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
    if (capture->file != NULL)
    {
        fsCloseFile(capture->file);
    }
    if (capture->temp_path != NULL)
    {
        fsDeleteFile(capture->temp_path);
    }
    osFreeMem(capture->cache_root);
    osFreeMem(capture->stage_dir);
    osFreeMem(capture->temp_path);
    osFreeMem(capture->final_path);
    osMemset(capture, 0, sizeof(*capture));
}
