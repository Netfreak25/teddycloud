#include "content_playlist.h"

#include <inttypes.h>

#include "cJSON.h"
#include "debug.h"
#include "fs_ext.h"
#include "mutex_manager.h"
#include "os_port.h"
#include "server_helpers.h"

#define CONTENT_PLAYLIST_SCHEMA_VERSION 2
#define CONTENT_PLAYLIST_SCHEMA_VERSION_LEGACY 1
#define CONTENT_PLAYLIST_SHA1_SIZE 20
#define CONTENT_PLAYLIST_SHA1_HEX_SIZE (CONTENT_PLAYLIST_SHA1_SIZE * 2 + 1)
#define CONTENT_PLAYLIST_MAX_FILE_SIZE (64U * 1024U)
#define CONTENT_PLAYLIST_DIRECTORY "content-metadata"
#define CONTENT_PLAYLIST_TAF_DIRECTORY "taf-playlists"

static bool_t content_playlist_string_valid(const char *value, size_t max_length)
{
    if (value == NULL || osStrlen(value) > max_length)
    {
        return FALSE;
    }

    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++)
    {
        if (*cursor < 0x20)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static bool_t content_playlist_identity(const tonie_info_t *tonie_info,
                                        uint32_t *audio_id,
                                        char sha1_hex[CONTENT_PLAYLIST_SHA1_HEX_SIZE])
{
    if (!content_playlist_is_editable(tonie_info) || audio_id == NULL || sha1_hex == NULL ||
        tonie_info->tafHeader->sha1_hash.len != CONTENT_PLAYLIST_SHA1_SIZE)
    {
        return FALSE;
    }

    *audio_id = tonie_info->tafHeader->audio_id;
    for (size_t i = 0; i < CONTENT_PLAYLIST_SHA1_SIZE; i++)
    {
        osSnprintf(&sha1_hex[i * 2], 3, "%02x", tonie_info->tafHeader->sha1_hash.data[i]);
    }
    sha1_hex[CONTENT_PLAYLIST_SHA1_HEX_SIZE - 1] = '\0';
    return TRUE;
}

static char *content_playlist_path(settings_t *settings,
                                   const tonie_info_t *tonie_info,
                                   bool_t create_directory)
{
    uint32_t audio_id = 0;
    char sha1_hex[CONTENT_PLAYLIST_SHA1_HEX_SIZE];
    if (settings == NULL || settings->internal.datadirfull == NULL ||
        !content_playlist_identity(tonie_info, &audio_id, sha1_hex))
    {
        return NULL;
    }

    char *directory = custom_asprintf("%s%c%s%c%s",
                                      settings->internal.datadirfull,
                                      PATH_SEPARATOR,
                                      CONTENT_PLAYLIST_DIRECTORY,
                                      PATH_SEPARATOR,
                                      CONTENT_PLAYLIST_TAF_DIRECTORY);
    if (directory == NULL)
    {
        return NULL;
    }

    if (create_directory)
    {
        error_t error = fsCreateDirEx(directory, true);
        if (error != NO_ERROR && !fsDirExists(directory))
        {
            TRACE_ERROR("Could not create content playlist directory %s: %s\r\n",
                        directory,
                        error2text(error));
            osFreeMem(directory);
            return NULL;
        }
    }

    char *path = custom_asprintf("%s%c%08" PRIX32 "-%s.json",
                                 directory,
                                 PATH_SEPARATOR,
                                 audio_id,
                                 sha1_hex);
    osFreeMem(directory);
    return path;
}

bool_t content_playlist_is_editable(const tonie_info_t *tonie_info)
{
    if (tonie_info == NULL || !tonie_info->valid || tonie_info->tafHeader == NULL || tonie_info->json.source == NULL ||
        tonie_info->json.source[0] == '\0')
    {
        return FALSE;
    }

    return tonie_info->json._source_type == CT_SOURCE_TAF;
}

size_t content_playlist_chapter_count(const tonie_info_t *tonie_info)
{
    if (!content_playlist_is_editable(tonie_info))
    {
        return 0;
    }
    return tonie_info->tafHeader->n_track_page_nums;
}

bool_t content_playlist_calculate_durations(const tonie_info_t *tonie_info,
                                            uint32_t *durations,
                                            size_t duration_count)
{
    size_t chapter_count = content_playlist_chapter_count(tonie_info);
    const track_positions_t *positions = tonie_info != NULL ? &tonie_info->additional.track_positions : NULL;
    if (durations == NULL || duration_count != chapter_count || chapter_count == 0 ||
        positions == NULL || positions->pos == NULL || positions->count != chapter_count ||
        positions->total_seconds == 0)
    {
        return FALSE;
    }

    uint32_t chapter_start = 0;
    for (size_t i = 0; i < chapter_count; i++)
    {
        uint32_t chapter_end = i + 1 < chapter_count ?
                                   positions->pos[i + 1] :
                                   positions->total_seconds;
        if (chapter_end < chapter_start)
        {
            return FALSE;
        }
        durations[i] = chapter_end - chapter_start;
        chapter_start = chapter_end;
    }
    return TRUE;
}

void content_playlist_free(content_playlist_t *playlist)
{
    if (playlist == NULL)
    {
        return;
    }

    osFreeMem(playlist->title);
    for (size_t i = 0; playlist->tracks != NULL && i < playlist->track_count; i++)
    {
        osFreeMem(playlist->tracks[i]);
    }
    osFreeMem(playlist->tracks);
    osFreeMem(playlist->durations);
    osMemset(playlist, 0, sizeof(*playlist));
}

static error_t content_playlist_read_file(const char *path, char **data, size_t *data_length)
{
    uint32_t file_size = 0;
    error_t error = fsGetFileSize(path, &file_size);
    if (error != NO_ERROR)
    {
        return error;
    }
    if (file_size == 0 || file_size > CONTENT_PLAYLIST_MAX_FILE_SIZE)
    {
        return ERROR_INVALID_LENGTH;
    }

    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }

    char *buffer = osAllocMem((size_t)file_size + 1);
    if (buffer == NULL)
    {
        fsCloseFile(file);
        return ERROR_OUT_OF_MEMORY;
    }

    size_t offset = 0;
    while (offset < file_size)
    {
        size_t received = 0;
        error = fsReadFile(file, buffer + offset, file_size - offset, &received);
        if (error != NO_ERROR || received == 0)
        {
            break;
        }
        offset += received;
    }
    fsCloseFile(file);

    if (error != NO_ERROR || offset != file_size)
    {
        osFreeMem(buffer);
        return error != NO_ERROR ? error : ERROR_END_OF_STREAM;
    }

    buffer[offset] = '\0';
    *data = buffer;
    *data_length = offset;
    return NO_ERROR;
}

error_t content_playlist_load(settings_t *settings,
                              const tonie_info_t *tonie_info,
                              content_playlist_t *playlist)
{
    if (playlist == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(playlist, 0, sizeof(*playlist));

    char *path = content_playlist_path(settings, tonie_info, FALSE);
    if (path == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (!fsFileExists(path))
    {
        osFreeMem(path);
        return ERROR_NOT_FOUND;
    }

    char *data = NULL;
    size_t data_length = 0;
    error_t error = content_playlist_read_file(path, &data, &data_length);
    osFreeMem(path);
    if (error != NO_ERROR)
    {
        return error;
    }

    cJSON *json = cJSON_ParseWithLengthOpts(data, data_length, NULL, FALSE);
    osFreeMem(data);
    if (!cJSON_IsObject(json))
    {
        cJSON_Delete(json);
        return ERROR_INVALID_FILE;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(json, "title");
    cJSON *tracks = cJSON_GetObjectItemCaseSensitive(json, "tracks");
    cJSON *durations = cJSON_GetObjectItemCaseSensitive(json, "durations");
    size_t expected_count = content_playlist_chapter_count(tonie_info);
    bool_t schema_supported = cJSON_IsNumber(schema) &&
                              (schema->valueint == CONTENT_PLAYLIST_SCHEMA_VERSION ||
                               schema->valueint == CONTENT_PLAYLIST_SCHEMA_VERSION_LEGACY);
    if (!schema_supported ||
        !cJSON_IsString(title) || !content_playlist_string_valid(title->valuestring, CONTENT_PLAYLIST_TITLE_MAX) ||
        !cJSON_IsArray(tracks) || (size_t)cJSON_GetArraySize(tracks) != expected_count ||
        (schema->valueint == CONTENT_PLAYLIST_SCHEMA_VERSION &&
         (!cJSON_IsArray(durations) || (size_t)cJSON_GetArraySize(durations) != expected_count)))
    {
        cJSON_Delete(json);
        return ERROR_INVALID_FILE;
    }

    playlist->title = strdup(title->valuestring);
    playlist->track_count = expected_count;
    playlist->tracks = expected_count > 0 ? osAllocMem(expected_count * sizeof(char *)) : NULL;
    playlist->durations = expected_count > 0 ? osAllocMem(expected_count * sizeof(uint32_t)) : NULL;
    if (playlist->title == NULL ||
        (expected_count > 0 && (playlist->tracks == NULL || playlist->durations == NULL)))
    {
        cJSON_Delete(json);
        content_playlist_free(playlist);
        return ERROR_OUT_OF_MEMORY;
    }
    if (playlist->tracks != NULL)
    {
        osMemset(playlist->tracks, 0, expected_count * sizeof(char *));
        osMemset(playlist->durations, 0, expected_count * sizeof(uint32_t));
    }

    for (size_t i = 0; i < expected_count; i++)
    {
        cJSON *track = cJSON_GetArrayItem(tracks, (int)i);
        if (!cJSON_IsString(track) ||
            !content_playlist_string_valid(track->valuestring, CONTENT_PLAYLIST_TRACK_TITLE_MAX))
        {
            cJSON_Delete(json);
            content_playlist_free(playlist);
            return ERROR_INVALID_FILE;
        }
        playlist->tracks[i] = strdup(track->valuestring);
        if (playlist->tracks[i] == NULL)
        {
            cJSON_Delete(json);
            content_playlist_free(playlist);
            return ERROR_OUT_OF_MEMORY;
        }
    }

    if (schema->valueint == CONTENT_PLAYLIST_SCHEMA_VERSION)
    {
        for (size_t i = 0; i < expected_count; i++)
        {
            cJSON *duration = cJSON_GetArrayItem(durations, (int)i);
            if (!cJSON_IsNumber(duration) || duration->valuedouble < 0 || duration->valuedouble > UINT32_MAX ||
                duration->valuedouble != (double)(uint32_t)duration->valuedouble)
            {
                cJSON_Delete(json);
                content_playlist_free(playlist);
                return ERROR_INVALID_FILE;
            }
            playlist->durations[i] = (uint32_t)duration->valuedouble;
        }
        playlist->durations_valid = TRUE;
    }
    else
    {
        playlist->durations_valid =
            content_playlist_calculate_durations(tonie_info, playlist->durations, expected_count);
    }

    playlist->valid = TRUE;
    cJSON_Delete(json);
    return NO_ERROR;
}

error_t content_playlist_save(settings_t *settings,
                              const tonie_info_t *tonie_info,
                              const char *title,
                              const char *const *tracks,
                              size_t track_count)
{
    size_t expected_count = content_playlist_chapter_count(tonie_info);
    if (!content_playlist_string_valid(title, CONTENT_PLAYLIST_TITLE_MAX) ||
        track_count != expected_count || (track_count > 0 && tracks == NULL))
    {
        return ERROR_INVALID_PARAMETER;
    }
    for (size_t i = 0; i < track_count; i++)
    {
        if (!content_playlist_string_valid(tracks[i], CONTENT_PLAYLIST_TRACK_TITLE_MAX))
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    uint32_t *durations = track_count > 0 ? osAllocMem(track_count * sizeof(uint32_t)) : NULL;
    if (track_count > 0 && durations == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    if (!content_playlist_calculate_durations(tonie_info, durations, track_count))
    {
        osFreeMem(durations);
        return ERROR_INVALID_FILE;
    }

    char *path = content_playlist_path(settings, tonie_info, TRUE);
    if (path == NULL)
    {
        osFreeMem(durations);
        return ERROR_FAILURE;
    }
    char *temp_path = custom_asprintf("%s.tmp", path);
    if (temp_path == NULL)
    {
        osFreeMem(durations);
        osFreeMem(path);
        return ERROR_OUT_OF_MEMORY;
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL || cJSON_AddNumberToObject(json, "schema", CONTENT_PLAYLIST_SCHEMA_VERSION) == NULL ||
        cJSON_AddStringToObject(json, "title", title) == NULL)
    {
        cJSON_Delete(json);
        osFreeMem(durations);
        osFreeMem(temp_path);
        osFreeMem(path);
        return ERROR_OUT_OF_MEMORY;
    }

    cJSON *track_array = cJSON_AddArrayToObject(json, "tracks");
    if (track_array == NULL)
    {
        cJSON_Delete(json);
        osFreeMem(durations);
        osFreeMem(temp_path);
        osFreeMem(path);
        return ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < track_count; i++)
    {
        cJSON *track = cJSON_CreateString(tracks[i]);
        if (track == NULL)
        {
            cJSON_Delete(json);
            osFreeMem(durations);
            osFreeMem(temp_path);
            osFreeMem(path);
            return ERROR_OUT_OF_MEMORY;
        }
        cJSON_AddItemToArray(track_array, track);
    }
    cJSON *duration_array = cJSON_AddArrayToObject(json, "durations");
    if (duration_array == NULL)
    {
        cJSON_Delete(json);
        osFreeMem(durations);
        osFreeMem(temp_path);
        osFreeMem(path);
        return ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < track_count; i++)
    {
        cJSON *duration = cJSON_CreateNumber(durations[i]);
        if (duration == NULL)
        {
            cJSON_Delete(json);
            osFreeMem(durations);
            osFreeMem(temp_path);
            osFreeMem(path);
            return ERROR_OUT_OF_MEMORY;
        }
        cJSON_AddItemToArray(duration_array, duration);
    }
    osFreeMem(durations);
    char *serialized = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (serialized == NULL)
    {
        osFreeMem(temp_path);
        osFreeMem(path);
        return ERROR_OUT_OF_MEMORY;
    }

    mutex_lock_id(path);
    FsFile *file = fsOpenFile(temp_path, FS_FILE_MODE_WRITE);
    error_t error = file != NULL ? fsWriteFile(file, serialized, osStrlen(serialized)) :
                                   ERROR_FILE_OPENING_FAILED;
    if (file != NULL)
    {
        fsCloseFile(file);
    }
    if (error == NO_ERROR)
    {
        error = fsMoveFile(temp_path, path, TRUE);
    }
    if (error != NO_ERROR)
    {
        fsDeleteFile(temp_path);
    }
    mutex_unlock_id(path);

    if (error == NO_ERROR)
    {
        TRACE_INFO("Saved custom content playlist audioId=%08" PRIX32 " chapters=%" PRIuSIZE "\r\n",
                   tonie_info->tafHeader->audio_id,
                   track_count);
    }

    osFreeMem(serialized);
    osFreeMem(temp_path);
    osFreeMem(path);
    return error;
}
