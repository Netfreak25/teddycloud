#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fs_ext.h"
#include "json_helper.h"
#include "mutex_manager.h"
#include "os_ext.h"
#include "path.h"
#include "server_helpers.h"
#include "tonie_user_metadata.h"

#define TONIE_USER_METADATA_VERSION 1U
#define TONIE_USER_METADATA_DIRECTORY "tonie-metadata"
#define TONIE_USER_IMAGE_DIRECTORY "ruid"

static char *tonie_user_comment_path(const char *config_dir,
                                     const char *canonical_ruid,
                                     bool_t temporary)
{
    return custom_asprintf("%s%c%s%c%s.json%s", config_dir, PATH_SEPARATOR,
                           TONIE_USER_METADATA_DIRECTORY, PATH_SEPARATOR,
                           canonical_ruid, temporary ? ".tmp" : "");
}

static error_t tonie_user_ensure_directory(const char *path)
{
    if (fsDirExists(path))
    {
        return NO_ERROR;
    }
    return fsCreateDirEx(path, true);
}

error_t tonie_user_canonicalize_ruid(
    const char *ruid, char canonical[TONIE_USER_RUID_LENGTH + 1U])
{
    if (ruid == NULL || canonical == NULL ||
        osStrlen(ruid) != TONIE_USER_RUID_LENGTH)
    {
        return ERROR_INVALID_PARAMETER;
    }

    for (size_t i = 0; i < TONIE_USER_RUID_LENGTH; i++)
    {
        unsigned char value = (unsigned char)ruid[i];
        if (!isxdigit(value))
        {
            return ERROR_INVALID_PARAMETER;
        }
        canonical[i] = (char)toupper(value);
    }
    canonical[TONIE_USER_RUID_LENGTH] = '\0';
    return NO_ERROR;
}

static bool_t tonie_user_utf8_continuation(uint8_t value)
{
    return (value & 0xC0U) == 0x80U;
}

static error_t tonie_user_utf8_character_count(const char *value,
                                               size_t *character_count)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t count = 0;

    for (size_t i = 0; bytes[i] != 0;)
    {
        uint8_t first = bytes[i];
        size_t width = 0;
        uint32_t codepoint = 0;

        if (first < 0x80U)
        {
            width = 1;
            codepoint = first;
        }
        else if (first >= 0xC2U && first <= 0xDFU)
        {
            width = 2;
            codepoint = first & 0x1FU;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            width = 3;
            codepoint = first & 0x0FU;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            width = 4;
            codepoint = first & 0x07U;
        }
        else
        {
            return ERROR_INVALID_REQUEST;
        }

        for (size_t offset = 1; offset < width; offset++)
        {
            if (bytes[i + offset] == 0 ||
                !tonie_user_utf8_continuation(bytes[i + offset]))
            {
                return ERROR_INVALID_REQUEST;
            }
            codepoint = (codepoint << 6U) | (bytes[i + offset] & 0x3FU);
        }

        if ((width == 2 && codepoint < 0x80U) ||
            (width == 3 && codepoint < 0x800U) ||
            (width == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU)
        {
            return ERROR_INVALID_REQUEST;
        }

        count++;
        if (count > TONIE_USER_COMMENT_MAX_CHARACTERS)
        {
            return ERROR_INVALID_REQUEST;
        }
        i += width;
    }

    *character_count = count;
    return NO_ERROR;
}

error_t tonie_user_comment_validate_and_copy(const char *comment,
                                             char **normalized_comment)
{
    if (comment == NULL || normalized_comment == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (osStrchr(comment, '\r') != NULL || osStrchr(comment, '\n') != NULL)
    {
        return ERROR_INVALID_REQUEST;
    }

    const char *start = comment;
    while (*start == ' ' || *start == '\t')
    {
        start++;
    }
    const char *end = comment + osStrlen(comment);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    {
        end--;
    }

    size_t length = (size_t)(end - start);
    char *copy = osAllocMem(length + 1U);
    if (copy == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    osMemcpy(copy, start, length);
    copy[length] = '\0';

    size_t character_count = 0;
    error_t error = tonie_user_utf8_character_count(copy, &character_count);
    if (error != NO_ERROR)
    {
        osFreeMem(copy);
        return error;
    }

    *normalized_comment = copy;
    return NO_ERROR;
}

error_t tonie_user_comment_load(const char *config_dir, const char *ruid,
                                char **comment)
{
    if (config_dir == NULL || comment == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    *comment = strdup("");
    if (*comment == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    char canonical_ruid[TONIE_USER_RUID_LENGTH + 1U];
    error_t error = tonie_user_canonicalize_ruid(ruid, canonical_ruid);
    if (error != NO_ERROR)
    {
        osFreeMem(*comment);
        *comment = NULL;
        return error;
    }

    char *path = tonie_user_comment_path(config_dir, canonical_ruid, FALSE);
    if (path == NULL)
    {
        osFreeMem(*comment);
        *comment = NULL;
        return ERROR_OUT_OF_MEMORY;
    }
    if (!fsFileExists(path))
    {
        osFreeMem(path);
        return NO_ERROR;
    }

    uint32_t file_size = 0;
    error = fsGetFileSize(path, &file_size);
    if (error != NO_ERROR || file_size == 0 || file_size > 4096U)
    {
        osFreeMem(path);
        return ERROR_INVALID_FILE;
    }

    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    osFreeMem(path);
    if (file == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }

    char *raw = osAllocMem((size_t)file_size + 1U);
    if (raw == NULL)
    {
        fsCloseFile(file);
        return ERROR_OUT_OF_MEMORY;
    }
    size_t bytes_read = 0;
    error = fsReadFile(file, raw, file_size, &bytes_read);
    fsCloseFile(file);
    if (error != NO_ERROR || bytes_read != file_size)
    {
        osFreeMem(raw);
        return error != NO_ERROR ? error : ERROR_UNEXPECTED_END_OF_FILE;
    }
    raw[file_size] = '\0';

    cJSON *json = cJSON_ParseWithLengthOpts(raw, file_size, NULL, FALSE);
    osFreeMem(raw);
    if (json == NULL || !cJSON_IsObject(json) ||
        jsonGetUInt32(json, "_version") != TONIE_USER_METADATA_VERSION)
    {
        cJSON_Delete(json);
        return ERROR_INVALID_FILE;
    }

    char *stored = jsonGetString(json, "comment");
    cJSON_Delete(json);
    char *normalized = NULL;
    error = tonie_user_comment_validate_and_copy(stored, &normalized);
    osFreeMem(stored);
    if (error != NO_ERROR)
    {
        return ERROR_INVALID_FILE;
    }

    osFreeMem(*comment);
    *comment = normalized;
    return NO_ERROR;
}

error_t tonie_user_comment_save(const char *config_dir, const char *ruid,
                                const char *comment)
{
    if (config_dir == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    char canonical_ruid[TONIE_USER_RUID_LENGTH + 1U];
    error_t error = tonie_user_canonicalize_ruid(ruid, canonical_ruid);
    if (error != NO_ERROR)
    {
        return error;
    }

    char *normalized = NULL;
    error = tonie_user_comment_validate_and_copy(comment, &normalized);
    if (error != NO_ERROR)
    {
        return error;
    }

    char *directory = custom_asprintf("%s%c%s", config_dir, PATH_SEPARATOR,
                                      TONIE_USER_METADATA_DIRECTORY);
    char *path = tonie_user_comment_path(config_dir, canonical_ruid, FALSE);
    char *temporary_path =
        tonie_user_comment_path(config_dir, canonical_ruid, TRUE);
    if (directory == NULL || path == NULL || temporary_path == NULL)
    {
        osFreeMem(directory);
        osFreeMem(path);
        osFreeMem(temporary_path);
        osFreeMem(normalized);
        return ERROR_OUT_OF_MEMORY;
    }

    error = tonie_user_ensure_directory(directory);
    osFreeMem(directory);
    if (error != NO_ERROR)
    {
        osFreeMem(path);
        osFreeMem(temporary_path);
        osFreeMem(normalized);
        return error;
    }

    mutex_lock_id(path);
    if (normalized[0] == '\0')
    {
        if (fsFileExists(path))
        {
            error = fsDeleteFile(path);
        }
        fsDeleteFile(temporary_path);
    }
    else
    {
        cJSON *json = cJSON_CreateObject();
        if (json == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            cJSON_AddNumberToObject(json, "_version",
                                    TONIE_USER_METADATA_VERSION);
            cJSON_AddStringToObject(json, "comment", normalized);
            char *raw = cJSON_Print(json);
            cJSON_Delete(json);
            if (raw == NULL)
            {
                error = ERROR_OUT_OF_MEMORY;
            }
            else
            {
                FsFile *file = fsOpenFile(temporary_path,
                                          FS_FILE_MODE_WRITE |
                                              FS_FILE_MODE_CREATE |
                                              FS_FILE_MODE_TRUNC);
                if (file == NULL)
                {
                    error = ERROR_FILE_OPENING_FAILED;
                }
                else
                {
                    error = fsWriteFile(file, raw, osStrlen(raw));
                    fsCloseFile(file);
                    if (error == NO_ERROR)
                    {
                        error = fsMoveFile(temporary_path, path, true);
                    }
                }
                osFreeMem(raw);
            }
        }
        if (error != NO_ERROR)
        {
            fsDeleteFile(temporary_path);
        }
    }
    mutex_unlock_id(path);

    osFreeMem(path);
    osFreeMem(temporary_path);
    osFreeMem(normalized);
    return error;
}

char *tonie_user_image_path(const char *www_dir, const char *ruid,
                            bool_t temporary)
{
    char canonical_ruid[TONIE_USER_RUID_LENGTH + 1U];
    if (www_dir == NULL ||
        tonie_user_canonicalize_ruid(ruid, canonical_ruid) != NO_ERROR)
    {
        return NULL;
    }
    return custom_asprintf("%s%ccustom_img%c%s%c%s.image%s", www_dir,
                           PATH_SEPARATOR, PATH_SEPARATOR,
                           TONIE_USER_IMAGE_DIRECTORY, PATH_SEPARATOR,
                           canonical_ruid, temporary ? ".tmp" : "");
}

const char *tonie_user_image_detect_mime(const char *path)
{
    if (path == NULL || !fsFileExists(path))
    {
        return NULL;
    }

    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL)
    {
        return NULL;
    }
    uint8_t header[12] = {0};
    size_t bytes_read = 0;
    error_t error = fsReadFile(file, header, sizeof(header), &bytes_read);
    fsCloseFile(file);
    if (error != NO_ERROR && error != ERROR_END_OF_FILE)
    {
        return NULL;
    }

    static const uint8_t png_signature[] = {0x89U, 'P', 'N', 'G', 0x0DU,
                                            0x0AU, 0x1AU, 0x0AU};
    if (bytes_read >= sizeof(png_signature) &&
        osMemcmp(header, png_signature, sizeof(png_signature)) == 0)
    {
        return "image/png";
    }
    if (bytes_read >= 3U && header[0] == 0xFFU && header[1] == 0xD8U &&
        header[2] == 0xFFU)
    {
        return "image/jpeg";
    }
    if (bytes_read >= 6U &&
        (!osMemcmp(header, "GIF87a", 6U) ||
         !osMemcmp(header, "GIF89a", 6U)))
    {
        return "image/gif";
    }
    if (bytes_read >= 12U && !osMemcmp(header, "RIFF", 4U) &&
        !osMemcmp(&header[8], "WEBP", 4U))
    {
        return "image/webp";
    }
    return NULL;
}

bool_t tonie_user_image_is_valid(const char *path)
{
    uint32_t size = 0;
    return path != NULL && fsGetFileSize(path, &size) == NO_ERROR && size > 0 &&
           size <= TONIE_USER_IMAGE_MAX_SIZE &&
           tonie_user_image_detect_mime(path) != NULL;
}
