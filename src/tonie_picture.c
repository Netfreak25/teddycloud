#include "tonie_picture.h"
#include "tonie_user_metadata.h"
#include "toniesJson.h"
#include "handler.h"
#include "hash/sha256.h"
#include "mutex_manager.h"
#include "server_helpers.h"
#include "v3_native_cache.h"
#include "cJSON.h"
#include <ctype.h>

#define PICTURE_METADATA_MAX_SIZE (64U * 1024U)
#define PICTURE_HASH_BUFFER_SIZE 1024U

bool_t tonie_picture_is_unknown(const char *picture)
{
    if (picture == NULL || picture[0] == '\0') return TRUE;
    while (isspace((unsigned char)*picture)) picture++;
    size_t length = strcspn(picture, "?#");
    while (length > 0 && isspace((unsigned char)picture[length - 1])) length--;
    if (length == 0) return TRUE;
    const char *name = picture;
    for (size_t i = 0; i < length; i++)
        if (picture[i] == '/') name = picture + i + 1;
    return (size_t)(picture + length - name) == sizeof("img_unknown.png") - 1 &&
           osStrncasecmp(name, "img_unknown.png", sizeof("img_unknown.png") - 1) == 0;
}

/* Do not use load_content_json/getTonieInfo: those can repair/write content state. */
static cJSON *picture_read_tag(settings_t *settings, const char *ruid)
{
    char canonical[TONIE_USER_RUID_LENGTH + 1];
    if (settings == NULL || settings->internal.contentdirfull == NULL ||
        tonie_user_canonicalize_ruid(ruid, canonical) != NO_ERROR) return NULL;
    char *content_path = NULL;
    getContentPathFromCharRUID(canonical, &content_path, settings);
    if (content_path == NULL) return NULL;
    char *path = custom_asprintf("%s.json", content_path);
    osFreeMem(content_path);
    uint32_t size = 0;
    cJSON *json = NULL;
    if (path != NULL && fsGetFileSize(path, &size) == NO_ERROR &&
        size > 0 && size <= PICTURE_METADATA_MAX_SIZE)
    {
        char *data = osAllocMem(size);
        FsFile *file = data != NULL ? fsOpenFile(path, FS_FILE_MODE_READ) : NULL;
        size_t received = 0;
        if (file != NULL)
        {
            if (fsReadFile(file, data, size, &received) == NO_ERROR && received == size)
                json = cJSON_ParseWithLength(data, size);
            fsCloseFile(file);
        }
        osFreeMem(data);
    }
    osFreeMem(path);
    return json;
}

static const char *picture_json_string(cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static toniesJson_item_t *picture_taf_item(const char *path)
{
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL) return NULL;
    uint8_t header[TAF_HEADER_SIZE];
    size_t received = 0;
    toniesJson_item_t *item = NULL;
    if (fsReadFile(file, header, 4, &received) == NO_ERROR && received == 4)
    {
        uint32_t length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                          ((uint32_t)header[2] << 8) | header[3];
        if (length > 0 && length <= sizeof(header) &&
            fsReadFile(file, header, length, &received) == NO_ERROR && received == length)
        {
            TonieboxAudioFileHeader *taf = toniebox_audio_file_header__unpack(NULL, length, header);
            if (taf != NULL)
            {
                item = tonies_byAudioIdHash(taf->audio_id,
                    taf->sha1_hash.len == SHA1_DIGEST_SIZE ? taf->sha1_hash.data : NULL);
                toniebox_audio_file_header__free_unpacked(taf, NULL);
            }
        }
    }
    fsCloseFile(file);
    return item;
}

static toniesJson_item_t *picture_source_item(settings_t *settings, const char *source)
{
    if (v3_native_library_source_is_candidate(source))
    {
        v3_native_library_collection_t collection;
        if (v3_native_library_collection_load(settings->internal.librarydirfull, source, FALSE,
                                               &collection) != NO_ERROR) return NULL;
        toniesJson_item_t *item = NULL;
        toniesJson_item_t *fallback = NULL;
        for (size_t i = 0; i < collection.origin_count; i++)
        {
            const v3_native_library_origin_t *origin = &collection.origins[i];
            item = tonies_byAudioIdTrackCountUnique(origin->content_version, collection.chapter_count);
            if (item != NULL && (size_t)item->tracks_count == collection.chapter_count) break;
            if (fallback == NULL) fallback = item;
            settings_t *origin_settings = get_settings_id(origin->overlay_id);
            cJSON *json = origin_settings != NULL && origin_settings->internal.config_used
                              ? picture_read_tag(origin_settings, origin->ruid) : NULL;
            const char *model = picture_json_string(json, "tonie_model");
            item = model != NULL ? tonies_byModel((char *)model) : NULL;
            cJSON_Delete(json);
            if (item != NULL && (size_t)item->tracks_count == collection.chapter_count) break;
            if (fallback == NULL) fallback = item;
            item = NULL;
        }
        v3_native_library_collection_free(&collection);
        return item != NULL ? item : fallback;
    }
    char *path = strdup(source);
    if (path == NULL) return NULL;
    resolveSpecialPathPrefix(&path, settings);
    // Read only a fixed TAF header, never materialize TAP/stream content for artwork.
    toniesJson_item_t *item = path != NULL && strstr(path, "://") == NULL ? picture_taf_item(path) : NULL;
    osFreeMem(path);
    return item;
}

static char *picture_custom_url(const char *ruid)
{
    char *path = tonie_user_image_path(get_settings()->internal.wwwdirfull, ruid, FALSE);
    if (path == NULL) return NULL;
    char *url = NULL;
    mutex_lock_id(path);
    if (tonie_user_image_is_valid(path))
    {
        FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
        if (file != NULL)
        {
            Sha256Context hash;
            sha256Init(&hash);
            uint8_t buffer[PICTURE_HASH_BUFFER_SIZE], digest[SHA256_DIGEST_SIZE];
            size_t received;
            error_t error;
            do {
                received = 0;
                error = fsReadFile(file, buffer, sizeof(buffer), &received);
                if (received > 0) sha256Update(&hash, buffer, received);
            } while (error == NO_ERROR && received > 0);
            fsCloseFile(file);
            if (error == NO_ERROR || error == ERROR_END_OF_FILE)
            {
                sha256Final(&hash, digest);
                char hex[SHA256_DIGEST_SIZE * 2 + 1];
                for (size_t i = 0; i < sizeof(digest); i++) osSprintf(hex + 2 * i, "%02x", digest[i]);
                url = custom_asprintf("/api/tonie/image/%s?v=%s", ruid, hex);
            }
        }
    }
    mutex_unlock_id(path);
    osFreeMem(path);
    return url;
}

char *tonie_picture_resolve(settings_t *settings, const char *ruid, uint32_t audio_id)
{
    char canonical[TONIE_USER_RUID_LENGTH + 1];
    bool_t known_tag = tonie_user_canonicalize_ruid(ruid, canonical) == NO_ERROR;
    cJSON *json = known_tag ? picture_read_tag(settings, canonical) : NULL;
    const char *source = picture_json_string(json, "source");
    const char *model = picture_json_string(json, "tonie_model");
    toniesJson_item_t *content = source != NULL && source[0] != '\0'
                                    ? picture_source_item(settings, source)
                                    : (audio_id != 0 ? tonies_byAudioId(audio_id) : NULL);
    char *picture = content != NULL && !tonie_picture_is_unknown(content->picture)
                        ? strdup(content->picture) : NULL;
    if (picture == NULL && known_tag) picture = picture_custom_url(canonical);
    toniesJson_item_t *figure = model != NULL ? tonies_byModel((char *)model) : NULL;
    if (picture == NULL && figure != NULL && !tonie_picture_is_unknown(figure->picture))
        picture = strdup(figure->picture);
    cJSON_Delete(json);
    return picture != NULL ? picture : strdup("/img_unknown.png");
}
