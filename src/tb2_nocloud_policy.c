#include "tb2_nocloud_policy.h"

#include "cJSON.h"
#include "fs_port.h"
#include "handler.h"
#include "os_port.h"
#include "server_helpers.h"

static bool_t tb2_nocloud_policy_init(settings_t *settings,
                                      const char *ruid,
                                      tb2_nocloud_policy_t *policy)
{
    if (settings == NULL || policy == NULL)
    {
        return FALSE;
    }

    osMemset(policy, 0, sizeof(*policy));
    if (!tb2_ruid_canonicalize(ruid, policy->ruid) ||
        !tb2_ruid_to_uid(policy->ruid, &policy->uid))
    {
        return FALSE;
    }
    policy->overlay_id = settings->internal.overlayNumber;
    policy->kind = tb2_ruid_classify(policy->ruid);
    return policy->kind != TB2_RUID_INVALID;
}

bool_t tb2_nocloud_policy_from_content(settings_t *settings,
                                       const char *ruid,
                                       const contentJson_t *content,
                                       tb2_nocloud_policy_t *policy)
{
    if (!tb2_nocloud_policy_init(settings, ruid, policy))
    {
        return FALSE;
    }
    if (policy->kind == TB2_RUID_SYSTEM)
    {
        return TRUE;
    }

    policy->metadata_exists = content != NULL;
    if (content != NULL)
    {
        policy->manual_nocloud = content->nocloud_manual;
        policy->source_nocloud = content->nocloud_source;
        policy->nocloud = content->nocloud;
        policy->cloud_override = content->cloud_override;
    }
    return TRUE;
}

static bool_t tb2_nocloud_json_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static cJSON *tb2_nocloud_parse_json(const char *data, size_t length)
{
    const char *parse_end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(data, length, &parse_end, 0);
    if (json == NULL || parse_end == NULL)
    {
        return json;
    }

    const char *data_end = data + length;
    while (parse_end < data_end && tb2_nocloud_json_space((uint8_t)*parse_end))
    {
        parse_end++;
    }
    if (parse_end != data_end)
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static bool_t tb2_nocloud_read_file(const char *path, char **data,
                                    size_t *length)
{
    uint32_t file_size = 0;
    if (path == NULL || data == NULL || length == NULL ||
        fsGetFileSize(path, &file_size) != NO_ERROR || file_size == 0 ||
        file_size == UINT32_MAX)
    {
        return FALSE;
    }

    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    char *buffer = file != NULL ? osAllocMem((size_t)file_size + 1U) : NULL;
    if (file == NULL || buffer == NULL)
    {
        if (file != NULL)
        {
            fsCloseFile(file);
        }
        return FALSE;
    }

    size_t position = 0;
    error_t error = NO_ERROR;
    while (position < file_size)
    {
        size_t received = 0;
        error = fsReadFile(file, buffer + position, file_size - position,
                           &received);
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
        return FALSE;
    }

    buffer[position] = '\0';
    *data = buffer;
    *length = position;
    return TRUE;
}

static bool_t tb2_nocloud_optional_bool(const cJSON *root, const char *name,
                                        bool_t *value, bool_t *found)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL)
    {
        *value = FALSE;
        if (found != NULL)
        {
            *found = FALSE;
        }
        return TRUE;
    }
    if (!cJSON_IsBool(item))
    {
        return FALSE;
    }
    *value = cJSON_IsTrue(item);
    if (found != NULL)
    {
        *found = TRUE;
    }
    return TRUE;
}

bool_t tb2_nocloud_policy_resolve(settings_t *settings,
                                  const char *ruid,
                                  tb2_nocloud_policy_t *policy)
{
    if (!tb2_nocloud_policy_init(settings, ruid, policy))
    {
        return FALSE;
    }
    if (policy->kind == TB2_RUID_SYSTEM)
    {
        return TRUE;
    }

    char *content_path = NULL;
    getContentPathFromCharRUID(policy->ruid, &content_path, settings);
    char *json_path = content_path != NULL
                          ? custom_asprintf("%s.json", content_path)
                          : NULL;
    osFreeMem(content_path);
    if (json_path == NULL)
    {
        return FALSE;
    }

    if (!fsFileExists(json_path))
    {
        osFreeMem(json_path);
        return TRUE;
    }
    policy->metadata_exists = TRUE;

    char *data = NULL;
    size_t length = 0;
    bool_t read = tb2_nocloud_read_file(json_path, &data, &length);
    osFreeMem(json_path);
    if (!read)
    {
        return FALSE;
    }

    cJSON *json = tb2_nocloud_parse_json(data, length);
    osFreeMem(data);
    bool_t legacy_nocloud = FALSE;
    bool_t manual_found = FALSE;
    bool_t source_found = FALSE;
    const cJSON *source = cJSON_IsObject(json)
                              ? cJSON_GetObjectItemCaseSensitive(json, "source")
                              : NULL;
    bool_t source_configured = cJSON_IsString(source) && source->valuestring != NULL &&
                               source->valuestring[0] != '\0';
    bool_t valid = cJSON_IsObject(json) &&
                   tb2_nocloud_optional_bool(json, "nocloud",
                                             &legacy_nocloud, NULL) &&
                   tb2_nocloud_optional_bool(json, "nocloud_manual",
                                             &policy->manual_nocloud,
                                             &manual_found) &&
                   tb2_nocloud_optional_bool(json, "nocloud_source",
                                             &policy->source_nocloud,
                                             &source_found) &&
                   tb2_nocloud_optional_bool(json, "cloud_override",
                                             &policy->cloud_override, NULL);
    if (valid && (!manual_found || !source_found))
    {
        policy->source_nocloud = source_configured;
        policy->manual_nocloud = legacy_nocloud && !source_configured;
    }
    policy->nocloud = policy->manual_nocloud || policy->source_nocloud;
    cJSON_Delete(json);
    return valid;
}

bool_t tb2_nocloud_policy_blocks_upstream(const tb2_nocloud_policy_t *policy)
{
    return policy != NULL && policy->kind == TB2_RUID_CONTENT &&
           (policy->source_nocloud ||
            (policy->manual_nocloud && !policy->cloud_override));
}
