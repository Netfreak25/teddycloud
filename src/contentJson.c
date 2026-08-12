#include "contentJson.h"

#include "settings.h"
#include "debug.h"
#include "cJSON.h"
#include "os_port.h"
#include "net_config.h"
#include "server_helpers.h"
#include "toniesJson.h"
#include "handler.h"
#include "json_helper.h"
#include "v3_native_cache.h"

static bool_t content_json_is_native_library_metadata(const char *json_path,
                                                      const settings_t *settings)
{
    if (json_path == NULL || settings == NULL ||
        settings->internal.librarydirfull == NULL)
    {
        return FALSE;
    }

    char *prefix = custom_asprintf("%s%cby%ccontentHash%c",
                                   settings->internal.librarydirfull,
                                   PATH_SEPARATOR, PATH_SEPARATOR,
                                   PATH_SEPARATOR);
    if (prefix == NULL)
    {
        return FALSE;
    }

    size_t prefix_length = osStrlen(prefix);
    bool_t protected_path = FALSE;
    if (!osStrncmp(json_path, prefix, prefix_length))
    {
        const char *relative = json_path + prefix_length;
        bool_t canonical_hash =
            osStrlen(relative) > V3_NATIVE_LIBRARY_HASH_HEX_LENGTH;
        for (size_t i = 0; i < V3_NATIVE_LIBRARY_HASH_HEX_LENGTH; i++)
        {
            if (!canonical_hash)
            {
                break;
            }
            char digit = relative[i];
            if (!((digit >= '0' && digit <= '9') ||
                  (digit >= 'a' && digit <= 'f')))
            {
                canonical_hash = FALSE;
                break;
            }
        }

        const char *filename = relative + V3_NATIVE_LIBRARY_HASH_HEX_LENGTH;
        protected_path = canonical_hash && *filename == PATH_SEPARATOR &&
                         (!osStrcmp(filename + 1, "library-entry.json") ||
                          !osStrcmp(filename + 1, "content-meta.json"));
    }

    osFreeMem(prefix);
    return protected_path;
}

static bool_t is_cache_preference_valid(const char *cache_preference)
{
    return cache_preference != NULL &&
           (!osStrcmp(cache_preference, CONTENT_JSON_CACHE_PREFERENCE_AUTO) ||
            !osStrcmp(cache_preference, CONTENT_JSON_CACHE_PREFERENCE_TAF) ||
            !osStrcmp(cache_preference, CONTENT_JSON_CACHE_PREFERENCE_V3));
}

error_t load_content_json(const char *content_path, contentJson_t *content_json, bool create_if_missing, settings_t *settings)
{
    char *jsonPath = custom_asprintf("%s.json", content_path);
    error_t error = NO_ERROR;
    osMemset(content_json, 0, sizeof(contentJson_t));
    content_json->live = false;
    content_json->nocloud = false;
    content_json->source = NULL;
    content_json->cache_preference = strdup(CONTENT_JSON_CACHE_PREFERENCE_AUTO);
    content_json->skip_seconds = 0;
    content_json->cache = false;
    content_json->_updated = false;
    content_json->_source_type = CT_SOURCE_NONE;
    content_json->_streamFile = custom_asprintf("%s.stream", content_path);
    content_json->cloud_ruid = NULL;
    content_json->cloud_auth = NULL;
    content_json->cloud_auth_len = 0;
    content_json->cloud_override = false;
    content_json->_has_cloud_auth = false;
    content_json->tonie_model = NULL;
    content_json->_source_model = NULL;
    content_json->hide = false;
    content_json->claimed = false;
    content_json->_valid = false;
    content_json->_create_if_missing = create_if_missing;

    osMemset(&content_json->_tap, 0, sizeof(tonie_audio_playlist_t));

    if (content_json_is_native_library_metadata(jsonPath, settings))
    {
        TRACE_WARNING("Refusing to parse protected TB2 library metadata as content JSON: %s\r\n",
                      jsonPath);
        osFreeMem(jsonPath);
        return ERROR_ACCESS_DENIED;
    }

    if (fsFileExists(jsonPath))
    {
        size_t fileSize = 0;
        fsGetFileSize(jsonPath, (uint32_t *)(&fileSize));

        FsFile *fsFile = fsOpenFile(jsonPath, FS_FILE_MODE_READ);
        if (fsFile != NULL)
        {
            size_t sizeRead;
            char *data = osAllocMem(fileSize);
            size_t pos = 0;

            while (pos < fileSize)
            {
                fsReadFile(fsFile, &data[pos], fileSize - pos, &sizeRead);
                pos += sizeRead;
            }
            fsCloseFile(fsFile);

            cJSON *contentJson = cJSON_ParseWithLengthOpts(data, fileSize, 0, 0);
            osFreeMem(data);
            if (contentJson == NULL)
            {
                const char *error_ptr = cJSON_GetErrorPtr();
                TRACE_ERROR("Json parse error\r\n");
                if (error_ptr != NULL)
                {
                    // TRACE_ERROR(" before: %s\r\n", error_ptr); //==194402==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020000bd2d0 at pc 0x555555ac8ba9 bp 0x7ffff2ffb490 sp 0x7ffff2ffac08
                }
                error = ERROR_INVALID_FILE;
            }
            else
            {
                content_json->live = jsonGetBool(contentJson, "live");
                content_json->nocloud = jsonGetBool(contentJson, "nocloud");
                content_json->source = jsonGetString(contentJson, "source");
                char *cache_preference =
                    jsonGetString(contentJson, "cache_preference");
                if (is_cache_preference_valid(cache_preference))
                {
                    osFreeMem(content_json->cache_preference);
                    content_json->cache_preference = cache_preference;
                }
                else
                {
                    osFreeMem(cache_preference);
                }
                content_json->_source_resolved = jsonGetString(contentJson, "source");
                content_json->skip_seconds = jsonGetUInt32(contentJson, "skip_seconds");
                content_json->cache = jsonGetBool(contentJson, "cache");
                content_json->cloud_ruid = jsonGetString(contentJson, "cloud_ruid");
                content_json->cloud_auth = jsonGetBytes(contentJson, "cloud_auth", &content_json->cloud_auth_len);
                content_json->cloud_override = jsonGetBool(contentJson, "cloud_override");
                content_json->tonie_model = jsonGetString(contentJson, "tonie_model");
                content_json->hide = jsonGetBool(contentJson, "hide");
                content_json->claimed = jsonGetBool(contentJson, "claimed");

                // TODO: use checkCustomTonie to validate
                // TODO validate rUID
                if (osStrlen(content_json->cloud_ruid) == 16 && content_json->cloud_auth_len == TONIE_AUTH_TOKEN_LENGTH)
                {
                    content_json->_has_cloud_auth = true;
                }
                else
                {
                    content_json->cloud_override = false;
                }

                if (osStrlen(content_json->source) > 0)
                {
                    resolveSpecialPathPrefix(&content_json->_source_resolved, settings);
                    if (!osStrncmp(content_json->source,
                                   "lib://by/contentHash/",
                                   sizeof("lib://by/contentHash/") - 1U))
                    {
                        v3_native_library_collection_t collection;
                        error_t collection_error =
                            v3_native_library_source_is_candidate(
                                content_json->source)
                                ? v3_native_library_collection_load(
                                      settings->internal.librarydirfull,
                                      content_json->source, FALSE, &collection)
                                : ERROR_INVALID_FILE;
                        if (collection_error == NO_ERROR)
                        {
                            content_json->_source_type =
                                CT_SOURCE_NATIVE_COLLECTION;
                            v3_native_library_collection_free(&collection);
                        }
                        else
                        {
                            v3_tonieplay_library_collection_t tonieplay;
                            error_t tonieplay_error =
                                v3_tonieplay_library_collection_load(
                                    settings->internal.librarydirfull,
                                    content_json->source, FALSE, &tonieplay);
                            if (tonieplay_error == NO_ERROR)
                            {
                                content_json->_source_type =
                                    CT_SOURCE_TONIEPLAY_COLLECTION;
                                content_json->_version =
                                    tonieplay.content_version;
                                v3_tonieplay_library_collection_free(
                                    &tonieplay);
                            }
                            else
                            {
                                TRACE_ERROR("Invalid native library source %s: audio=%s tonieplay=%s\r\n",
                                            content_json->source,
                                            error2text(collection_error),
                                            error2text(tonieplay_error));
                            }
                        }
                    }
                    else if (isValidTaf(content_json->_source_resolved, settings->core.full_taf_validation))
                    {
                        content_json->_source_type = CT_SOURCE_TAF;
                    }
                    else
                    {
                        /* Only try tap_load for .tap files (JSON). TAF files are binary – parsing them as JSON
                         * causes "Json parse error". Skip tap_load for .taf and other non-TAP paths. */
                        size_t src_len = osStrlen(content_json->_source_resolved);
                        bool_t looks_like_tap = (src_len >= 4 && osStrncasecmp(content_json->_source_resolved + src_len - 4, ".tap", 4) == 0);
                        error_t tap_error = looks_like_tap ? tap_load(content_json->_source_resolved, &content_json->_tap) : ERROR_INVALID_FILE;
                        if (tap_error == NO_ERROR && content_json->_tap._valid)
                        {
                            if (content_json->_tap._cached)
                            {
                                content_json->_source_type = CT_SOURCE_TAP_CACHED;
                            }
                            else
                            {
                                content_json->_source_type = CT_SOURCE_TAP_STREAM;
                            }
                            osFreeMem(content_json->_source_resolved);
                            content_json->_source_resolved = strdup(content_json->_tap._filepath_resolved);
                        }
                        else if (fsFileExists(content_json->_source_resolved) || osStrstr(content_json->_source_resolved, "://"))
                        {
                            content_json->_source_type = CT_SOURCE_STREAM;
                            if (!content_json->live || !content_json->nocloud)
                            {
                                content_json->live = true;
                                content_json->nocloud = true;
                                content_json->_updated = true;
                            }
                        }
                    }
                }

                if (jsonGetUInt32(contentJson, "_version") != CONTENT_JSON_VERSION)
                {
                    error = ERROR_INVALID_FILE;
                }

                cJSON_Delete(contentJson);
            }
        }
    }
    else
    {
        error = ERROR_FILE_NOT_FOUND;
    }

    if (error == NO_ERROR)
    {
        content_json->_valid = true;
    }

    if (error != NO_ERROR && (error != ERROR_FILE_NOT_FOUND || create_if_missing))
    {
        error = save_content_json(jsonPath, content_json);
        if (error == NO_ERROR)
        {
            load_content_json(content_path, content_json, true, settings);
        }
    }

    osFreeMem(jsonPath);

    return error;
}

error_t save_content_json(const char *json_path, contentJson_t *content_json)
{
    /* retrieve content directory */
    char *content_dir = strdup(json_path);

    if (fsRemoveFilename(content_dir) != NO_ERROR)
    {
        TRACE_ERROR("Error retrieving content directory from json path.\r\n");
        TRACE_ERROR("  json_path: '%s'\r\n", json_path);
        return ERROR_INVALID_PARAMETER;
    }
    /* create if not existing */
    if (!fsDirExists(content_dir))
    {
        TRACE_INFO("Content dir for JSON '%s' not existing, creating it.\r\n", json_path);
        fsCreateDir(content_dir);
    }
    osFreeMem(content_dir);

    char *jsonPathTmp = custom_asprintf("%s.tmp", json_path);
    error_t error = NO_ERROR;
    cJSON *contentJson = cJSON_CreateObject();

    cJSON_AddBoolToObject(contentJson, "live", content_json->live);
    cJSON_AddBoolToObject(contentJson, "nocloud", content_json->nocloud);
    jsonAddStringToObject(contentJson, "source", content_json->source);
    jsonAddStringToObject(contentJson, "cache_preference",
                          content_json->cache_preference);
    cJSON_AddNumberToObject(contentJson, "skip_seconds", content_json->skip_seconds);
    cJSON_AddBoolToObject(contentJson, "cache", content_json->cache);
    jsonAddStringToObject(contentJson, "cloud_ruid", content_json->cloud_ruid);
    jsonAddByteArrayToObject(contentJson, "cloud_auth", content_json->cloud_auth, content_json->cloud_auth_len);
    cJSON_AddBoolToObject(contentJson, "cloud_override", content_json->cloud_override);
    jsonAddStringToObject(contentJson, "tonie_model", content_json->tonie_model);
    cJSON_AddBoolToObject(contentJson, "hide", content_json->hide);
    cJSON_AddBoolToObject(contentJson, "claimed", content_json->claimed);
    cJSON_AddNumberToObject(contentJson, "_version", CONTENT_JSON_VERSION);

    char *jsonRaw = cJSON_Print(contentJson);

    FsFile *file = fsOpenFile(jsonPathTmp, FS_FILE_MODE_WRITE);
    if (file != NULL)
    {
        error = fsWriteFile(file, jsonRaw, osStrlen(jsonRaw));
        fsCloseFile(file);
    }
    else
    {
        error = ERROR_FILE_OPENING_FAILED;
    }

    if (error == NO_ERROR)
    {
        error = fsMoveFile(jsonPathTmp, json_path, true);
    }

    if (error == NO_ERROR)
    {
        content_json->_updated = false;
        content_json->_version = CONTENT_JSON_VERSION;
    }

    cJSON_Delete(contentJson);
    osFreeMem(jsonRaw);
    osFreeMem(jsonPathTmp);
    return error;
}

void content_json_update_model(contentJson_t *content_json, uint32_t audio_id, uint8_t *hash)
{
    if (content_json->_valid)
    {
        toniesJson_item_t *toniesJson = tonies_byAudioIdHash(audio_id, hash);
        if (toniesJson != NULL)
        {
            if (osStrcmp(content_json->tonie_model, toniesJson->model) != 0)
            {
                if (audio_id == SPECIAL_AUDIO_ID_ONE && hash == NULL)
                { // don't update special tonies without hash
                }
                else
                {
                    osFreeMem(content_json->tonie_model);
                    content_json->tonie_model = strdup(toniesJson->model);
                    content_json->_updated = true;
                }
            }
        }
        else
        {
            // TODO add to tonies.custom.json + report
            TRACE_DEBUG("Audio-id %08X unknown but previous content known by model %s.\r\n", audio_id, content_json->tonie_model);
        }
    }
}

void free_content_json(contentJson_t *content_json)
{
    content_json->_valid = false;
    if (content_json->source)
    {
        osFreeMem(content_json->source);
        content_json->source = NULL;
    }
    if (content_json->cache_preference)
    {
        osFreeMem(content_json->cache_preference);
        content_json->cache_preference = NULL;
    }
    if (content_json->cloud_ruid)
    {
        osFreeMem(content_json->cloud_ruid);
        content_json->cloud_ruid = NULL;
    }
    if (content_json->cloud_auth)
    {
        osFreeMem(content_json->cloud_auth);
        content_json->cloud_auth = NULL;
    }
    if (content_json->tonie_model)
    {
        osFreeMem(content_json->tonie_model);
        content_json->tonie_model = NULL;
    }
    if (content_json->_source_model)
    {
        osFreeMem(content_json->_source_model);
        content_json->_source_model = NULL;
    }
    if (content_json->_streamFile)
    {
        osFreeMem(content_json->_streamFile);
        content_json->_streamFile = NULL;
    }
    if (content_json->_source_resolved)
    {
        osFreeMem(content_json->_source_resolved);
        content_json->_source_resolved = NULL;
    }
    tap_free(&content_json->_tap);
    content_json->cloud_auth_len = 0;
}
