#include "taf_original_cache.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "fs_ext.h"
#include "hash/sha1.h"
#include "hash/sha256.h"
#include "mutex_manager.h"
#include "path.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"
#include "toniefile.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#define TAF_REFERENCE_VERSION 1
#define TAF_REFERENCE_MAX_BYTES 4096
#define TAF_COPY_BUFFER_BYTES 4096
#define TAF_RUID_LENGTH 16
#define TAF_HASH_TEXT_LENGTH (SHA1_DIGEST_SIZE * 2)
#define TAF_REFERENCE_TEMP_ATTEMPTS 16

typedef struct
{
    uint32_t audio_id;
    uint32_t size;
    uint8_t hash[SHA1_DIGEST_SIZE];
} taf_original_identity_t;

/* Protected by MUTEX_TAF_ORIGINAL_CACHE; temporary files are never shared. */
static uint32_t taf_original_temp_sequence;

static char *taf_original_join(const char *base, const char *suffix)
{
    if (!base || !base[0] || !suffix)
        return NULL;
    size_t length = strlen(base) + strlen(suffix) + 2;
    if (length > PATH_LEN)
        return NULL;
    char *path = osAllocMem(length);
    if (path)
        snprintf(path, length, "%s/%s", base, suffix);
    return path;
}

static char *taf_original_normalize(const char *path)
{
    if (!path || !path[0] || strlen(path) >= PATH_LEN)
        return NULL;
    char *normalized = osAllocMem(strlen(path) + 1);
    if (!normalized)
        return NULL;
    strcpy(normalized, path);
    pathCanonicalize(normalized);
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/')
        normalized[--length] = '\0';
#ifdef _WIN32
    for (char *p = normalized; *p; p++)
        *p = (char)tolower((unsigned char)*p);
#endif
    return normalized;
}

static void taf_original_hex(const uint8_t *hash, size_t length, char *text)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++)
    {
        text[i * 2] = digits[hash[i] >> 4];
        text[i * 2 + 1] = digits[hash[i] & 15];
    }
    text[length * 2] = '\0';
}

/** Scope references by the effective content directory, never by overlay index. */
static char *taf_original_reference_path(const char *raw_path, const settings_t *settings)
{
    if (!settings)
        return NULL;
    char *root = taf_original_normalize(settings->internal.contentdirfull);
    char *raw = taf_original_normalize(raw_path);
    char *reference = NULL;
    if (!root || !raw)
        goto cleanup;
    size_t root_length = strlen(root);
    if (strlen(raw) != root_length + 1 + TAF_RUID_LENGTH + 1 ||
        strncmp(root, raw, root_length) || raw[root_length] != '/')
        goto cleanup;
    const char *suffix = raw + root_length + 1;
    if (suffix[8] != '/')
        goto cleanup;
    char ruid[TAF_RUID_LENGTH + 1];
    for (size_t i = 0; i < TAF_RUID_LENGTH; i++)
    {
        unsigned char c = (unsigned char)suffix[i + (i >= 8)];
        if (!isxdigit(c))
            goto cleanup;
        ruid[i] = (char)toupper(c);
    }
    ruid[TAF_RUID_LENGTH] = '\0';
    uint8_t hash[SHA256_DIGEST_SIZE];
    char hash_text[SHA256_DIGEST_SIZE * 2 + 1];
    if (sha256Compute(root, root_length, hash) != NO_ERROR)
        goto cleanup;
    taf_original_hex(hash, sizeof(hash), hash_text);
    char relative[sizeof("taf-original/") + sizeof(hash_text) + sizeof(ruid) + sizeof("/.json")];
    snprintf(relative, sizeof(relative), "taf-original/%s/%s.json", hash_text, ruid);
    const settings_t *global = get_settings();
    if (global)
        reference = taf_original_join(global->internal.cachedirfull, relative);
cleanup:
    osFreeMem(root);
    osFreeMem(raw);
    return reference;
}

static bool taf_original_same(const taf_original_identity_t *a, const taf_original_identity_t *b)
{
    return a->audio_id == b->audio_id && a->size == b->size && !memcmp(a->hash, b->hash, sizeof(a->hash));
}

static error_t taf_original_create_directory(char *directory)
{
    if (!directory)
        return ERROR_OUT_OF_MEMORY;
    fsFixPath(directory);
    if (fsDirExists(directory))
        return NO_ERROR;
    error_t error = fsCreateDirEx(directory, true);
    return error && !fsDirExists(directory) ? error : NO_ERROR;
}

/** Reject oversized files instead of trusting the filesystem adapter's uint32 size. */
static error_t taf_original_file_size(FsFile *file, uint32_t *size)
{
#ifdef _WIN32
    struct _stat64 status;
    if (_fstat64(_fileno(file), &status) || (status.st_mode & _S_IFMT) != _S_IFREG)
#else
    struct stat status;
    if (fstat(fileno(file), &status) || !S_ISREG(status.st_mode))
#endif
        return ERROR_INVALID_FILE;
    if (status.st_size < 0 || (uint64_t)status.st_size > UINT32_MAX)
        return ERROR_INVALID_FILE;
    *size = (uint32_t)status.st_size;
    return NO_ERROR;
}

/** Normal reads validate header/size; imports additionally hash all audio bytes. */
static error_t taf_original_inspect(const char *path, bool full_hash, taf_original_identity_t *identity)
{
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (!file)
        return ERROR_FILE_OPENING_FAILED;
    uint32_t size;
    if (taf_original_file_size(file, &size) || size <= TONIEFILE_FRAME_SIZE)
    {
        fsCloseFile(file);
        return ERROR_INVALID_FILE;
    }
    uint8_t buffer[TAF_COPY_BUFFER_BYTES];
    size_t read = 0;
    error_t error = fsReadFile(file, buffer, TONIEFILE_FRAME_SIZE, &read);
    TonieboxAudioFileHeader *header = NULL;
    if (error || read != TONIEFILE_FRAME_SIZE)
    {
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }
    uint32_t header_length = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                             ((uint32_t)buffer[2] << 8) | buffer[3];
    if (!header_length || header_length > TONIEFILE_FRAME_SIZE - sizeof(header_length))
    {
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }
    header = toniebox_audio_file_header__unpack(NULL, header_length, buffer + sizeof(header_length));
    if (!header || header->audio_id <= 1 || header->sha1_hash.len != SHA1_DIGEST_SIZE ||
        header->num_bytes != (uint64_t)size - TONIEFILE_FRAME_SIZE)
    {
        error = ERROR_INVALID_FILE;
        goto cleanup;
    }
    identity->audio_id = header->audio_id;
    identity->size = size;
    memcpy(identity->hash, header->sha1_hash.data, sizeof(identity->hash));
    if (full_hash)
    {
        Sha1Context hash;
        sha1Init(&hash);
        uint32_t remaining = size - TONIEFILE_FRAME_SIZE;
        while (remaining)
        {
            size_t length = MIN(remaining, sizeof(buffer));
            error = fsReadFile(file, buffer, length, &read);
            if (error || read != length)
            {
                error = ERROR_FILE_READING_FAILED;
                goto cleanup;
            }
            sha1Update(&hash, buffer, length);
            remaining -= (uint32_t)length;
        }
        uint8_t digest[SHA1_DIGEST_SIZE];
        sha1Final(&hash, digest);
        if (memcmp(digest, identity->hash, sizeof(digest)))
            error = ERROR_WRONG_CHECKSUM;
    }
cleanup:
    toniebox_audio_file_header__free_unpacked(header, NULL);
    fsCloseFile(file);
    return error;
}

bool taf_original_file_complete(const char *path)
{
    taf_original_identity_t identity;
    return path && path[0] && taf_original_inspect(path, false, &identity) == NO_ERROR;
}

static bool taf_original_integer(const cJSON *value, uint32_t *result)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        (double)(uint32_t)value->valuedouble != value->valuedouble)
        return false;
    *result = (uint32_t)value->valuedouble;
    return true;
}

static char *taf_original_library_path(const settings_t *settings, uint32_t audio_id, char *uri, size_t uri_size)
{
    char relative[sizeof("by/audioID/4294967295.taf")];
    snprintf(relative, sizeof(relative), "by/audioID/%" PRIu32 ".taf", audio_id);
    snprintf(uri, uri_size, "lib://%s", relative);
    return taf_original_join(settings->internal.librarydirfull, relative);
}

char *taf_original_cache_resolve(const char *raw_content_path, const settings_t *settings)
{
    char *reference = taf_original_reference_path(raw_content_path, settings);
    if (!reference)
        return NULL;
    char *resolved = NULL;
    cJSON *json = NULL;
    FsFile *file = NULL;
    mutex_lock(MUTEX_TAF_ORIGINAL_CACHE);
    uint32_t size;
    file = fsOpenFile(reference, FS_FILE_MODE_READ);
    if (!file || taf_original_file_size(file, &size) || !size || size > TAF_REFERENCE_MAX_BYTES)
        goto cleanup;
    char body[TAF_REFERENCE_MAX_BYTES + 1];
    size_t read = 0;
    if (fsReadFile(file, body, size, &read) || read != size)
        goto cleanup;
    body[size] = '\0';
    const char *end = NULL;
    json = cJSON_ParseWithLengthOpts(body, size + 1, &end, true);
    uint32_t version, audio_id, expected_size;
    if (!cJSON_IsObject(json) || end != body + size ||
        !taf_original_integer(cJSON_GetObjectItemCaseSensitive(json, "_version"), &version) || version != TAF_REFERENCE_VERSION ||
        !taf_original_integer(cJSON_GetObjectItemCaseSensitive(json, "audioId"), &audio_id) || audio_id <= 1 ||
        !taf_original_integer(cJSON_GetObjectItemCaseSensitive(json, "size"), &expected_size))
        goto cleanup;
    cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "source");
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(json, "headerSha1");
    if (!cJSON_IsString(source) || !cJSON_IsString(hash))
        goto cleanup;
    char uri[sizeof("lib://by/audioID/4294967295.taf")];
    resolved = taf_original_library_path(settings, audio_id, uri, sizeof(uri));
    taf_original_identity_t identity;
    char hash_text[TAF_HASH_TEXT_LENGTH + 1];
    if (!resolved || strcmp(source->valuestring, uri) || taf_original_inspect(resolved, false, &identity))
        goto invalid;
    taf_original_hex(identity.hash, sizeof(identity.hash), hash_text);
    if (identity.audio_id != audio_id || identity.size != expected_size || strcmp(hash->valuestring, hash_text))
        goto invalid;
    goto cleanup;
invalid:
    osFreeMem(resolved);
    resolved = NULL;
cleanup:
    if (file)
        fsCloseFile(file);
    cJSON_Delete(json);
    mutex_unlock(MUTEX_TAF_ORIGINAL_CACHE);
    osFreeMem(reference);
    return resolved;
}

/** Exclusively create an owned adjacent temporary file; stale files are untouched. */
static FsFile *taf_original_open_temp(const char *target, char **temp_path)
{
    size_t size = strlen(target) + sizeof(".taf-original-4294967295.tmp");
    *temp_path = osAllocMem(size);
    if (!*temp_path)
        return NULL;
    for (unsigned attempt = 0; attempt < TAF_REFERENCE_TEMP_ATTEMPTS; attempt++)
    {
        snprintf(*temp_path, size, "%s.taf-original-%" PRIu32 ".tmp", target, ++taf_original_temp_sequence);
        if (fsFileExists(*temp_path))
            continue;
        FsFile *file = fsOpenFileEx(*temp_path, "wbx");
        if (file)
            return file;
        break;
    }
    osFreeMem(*temp_path);
    *temp_path = NULL;
    return NULL;
}

static error_t taf_original_copy(const char *raw, const char *library, char **temp)
{
    FsFile *source = fsOpenFile(raw, FS_FILE_MODE_READ);
    if (!source)
        return ERROR_FILE_OPENING_FAILED;
    FsFile *target = taf_original_open_temp(library, temp);
    error_t error = target ? NO_ERROR : ERROR_FILE_OPENING_FAILED;
    uint8_t buffer[TAF_COPY_BUFFER_BYTES];
    while (!error)
    {
        size_t read = 0;
        error = fsReadFile(source, buffer, sizeof(buffer), &read);
        if (error && error != ERROR_END_OF_FILE)
            break;
        bool end = error == ERROR_END_OF_FILE || !read;
        error = read ? fsWriteFile(target, buffer, read) : NO_ERROR;
        if (error || end)
            break;
    }
    if (!error)
        error = fsFlushFile(target);
    fsCloseFile(source);
    if (target)
        fsCloseFile(target);
    return error;
}

/** Publish without replacing a different file created by another importer. */
static error_t taf_original_publish_library(const char *temp, const char *library)
{
#ifdef _WIN32
    return MoveFileA(temp, library) ? NO_ERROR : ERROR_WRITE_FAILED;
#else
    return link(temp, library) == 0 ? NO_ERROR : ERROR_WRITE_FAILED;
#endif
}

static error_t taf_original_publish_reference(const char *reference, const char *uri, const taf_original_identity_t *identity)
{
    char hash[TAF_HASH_TEXT_LENGTH + 1];
    taf_original_hex(identity->hash, sizeof(identity->hash), hash);
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return ERROR_OUT_OF_MEMORY;
    bool valid = cJSON_AddNumberToObject(json, "_version", TAF_REFERENCE_VERSION) &&
                 cJSON_AddStringToObject(json, "source", uri) &&
                 cJSON_AddNumberToObject(json, "audioId", identity->audio_id) &&
                 cJSON_AddStringToObject(json, "headerSha1", hash) &&
                 cJSON_AddNumberToObject(json, "size", identity->size);
    char *body = valid ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!body)
        return ERROR_OUT_OF_MEMORY;
    char *temp = NULL;
    FsFile *file = taf_original_open_temp(reference, &temp);
    error_t error = file ? fsWriteFile(file, body, strlen(body)) : ERROR_FILE_OPENING_FAILED;
    if (!error)
        error = fsFlushFile(file);
    if (file)
        fsCloseFile(file);
    cJSON_free(body);
    if (!error)
    {
#ifdef _WIN32
        error = MoveFileExA(temp, reference, MOVEFILE_REPLACE_EXISTING) ? NO_ERROR : ERROR_WRITE_FAILED;
#else
        error = fsRenameFile(temp, reference);
#endif
    }
    if (temp)
        fsDeleteFile(temp);
    osFreeMem(temp);
    return error;
}

error_t taf_original_cache_import(const char *raw_content_path, const settings_t *settings)
{
    char *reference = taf_original_reference_path(raw_content_path, settings);
    if (!reference)
        return ERROR_INVALID_PATH;
    char *library = NULL;
    char *temp = NULL;
    char *directory = NULL;
    taf_original_identity_t raw, staged, existing;
    mutex_lock(MUTEX_TAF_ORIGINAL_CACHE);
    error_t error = taf_original_inspect(raw_content_path, false, &raw);
    if (error)
        goto cleanup;
    char uri[sizeof("lib://by/audioID/4294967295.taf")];
    library = taf_original_library_path(settings, raw.audio_id, uri, sizeof(uri));
    if (!library)
    {
        error = ERROR_INVALID_PATH;
        goto cleanup;
    }
    if (fsFileExists(library))
    {
        error = taf_original_inspect(library, true, &existing);
        if (!error && !taf_original_same(&raw, &existing))
            error = ERROR_WRONG_CHECKSUM;
        if (error)
            goto cleanup;
    }
    else
    {
        directory = taf_original_join(settings->internal.librarydirfull, "by/audioID");
        error = taf_original_create_directory(directory);
        osFreeMem(directory);
        directory = NULL;
        if (error)
            goto cleanup;
        error = taf_original_copy(raw_content_path, library, &temp);
        if (!error)
            error = taf_original_inspect(temp, true, &staged);
        if (!error && !taf_original_same(&raw, &staged))
            error = ERROR_WRONG_CHECKSUM;
        if (error)
            goto cleanup;
        error = taf_original_publish_library(temp, library);
        /* A concurrent importer may have won publication with the same object. */
        if (error && !taf_original_inspect(library, true, &existing) && taf_original_same(&raw, &existing))
            error = NO_ERROR;
        if (error)
            goto cleanup;
    }
    directory = osAllocMem(strlen(reference) + 1);
    if (!directory)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }
    strcpy(directory, reference);
    pathRemoveFilename(directory);
    error = taf_original_create_directory(directory);
    if (!error)
        error = taf_original_publish_reference(reference, uri, &raw);
    if (error)
        goto cleanup;
    /* Never remove a different generation substituted by an external writer. */
    error = taf_original_inspect(raw_content_path, false, &existing);
    if (!error && !taf_original_same(&raw, &existing))
        error = ERROR_WRONG_CHECKSUM;
    if (!error)
        error = fsDeleteFile(raw_content_path);
cleanup:
    if (temp)
        fsDeleteFile(temp);
    osFreeMem(temp);
    osFreeMem(directory);
    osFreeMem(library);
    osFreeMem(reference);
    mutex_unlock(MUTEX_TAF_ORIGINAL_CACHE);
    return error;
}
