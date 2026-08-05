#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "mqtt_nocloud_filter.h"
#include "os_port.h"
#include "tb2_nocloud_policy.h"

typedef struct mqtt_nocloud_policy_entry
{
    char ruid[TB2_RUID_SIZE];
    bool_t resolved;
    bool_t protected;
    struct mqtt_nocloud_policy_entry *next;
} mqtt_nocloud_policy_entry_t;

typedef struct
{
    settings_t *settings;
    mqtt_nocloud_policy_entry_t *policies;
} mqtt_nocloud_context_t;

static bool_t mqtt_nocloud_is_json_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static bool_t mqtt_nocloud_payload_contains(const uint8_t *payload,
                                            size_t payload_len,
                                            const char *needle)
{
    size_t needle_len = needle != NULL ? osStrlen(needle) : 0;
    if (payload == NULL || needle_len == 0 || payload_len < needle_len)
        return FALSE;

    for (size_t index = 0; index <= payload_len - needle_len; index++)
    {
        if (osMemcmp(payload + index, needle, needle_len) == 0)
            return TRUE;
    }
    return FALSE;
}

static cJSON *mqtt_nocloud_parse_json(const uint8_t *payload,
                                      size_t payload_len)
{
    const char *parse_end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)payload, payload_len,
                                            &parse_end, 0);
    if (json == NULL || parse_end == NULL)
        return json;

    const char *payload_end = (const char *)payload + payload_len;
    while (parse_end < payload_end &&
           mqtt_nocloud_is_json_space((uint8_t)*parse_end))
    {
        parse_end++;
    }
    if (parse_end != payload_end)
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static const char *mqtt_nocloud_topic_path(const char *topic)
{
    static const char prefix[] = "toniebox/";
    if (topic == NULL || osStrncmp(topic, prefix, sizeof(prefix) - 1) != 0)
        return NULL;

    const char *separator = osStrchr(topic + sizeof(prefix) - 1, '/');
    return separator != NULL && separator[1] != '\0' ? separator + 1 : NULL;
}

static bool_t mqtt_nocloud_is_protected(mqtt_nocloud_context_t *context,
                                        const char *ruid,
                                        bool_t *protected)
{
    char normalized[TB2_RUID_SIZE];
    if (!tb2_ruid_canonicalize(ruid, normalized))
    {
        *protected = FALSE;
        return TRUE;
    }

    for (mqtt_nocloud_policy_entry_t *entry = context->policies;
         entry != NULL; entry = entry->next)
    {
        if (osStrcmp(entry->ruid, normalized) == 0)
        {
            *protected = entry->protected;
            return entry->resolved;
        }
    }

    tb2_nocloud_policy_t policy;
    bool_t resolution_ok = tb2_nocloud_policy_resolve(context->settings,
                                                       normalized, &policy);
    bool_t resolved = resolution_ok
                          ? tb2_nocloud_policy_blocks_upstream(&policy)
                          : TRUE;

    mqtt_nocloud_policy_entry_t *entry = osAllocMem(sizeof(*entry));
    if (entry == NULL)
        return FALSE;
    osMemcpy(entry->ruid, normalized, sizeof(entry->ruid));
    entry->resolved = resolution_ok;
    entry->protected = resolved;
    entry->next = context->policies;
    context->policies = entry;
    *protected = resolved;
    return resolution_ok;
}

static void mqtt_nocloud_free_context(mqtt_nocloud_context_t *context)
{
    while (context->policies != NULL)
    {
        mqtt_nocloud_policy_entry_t *removed = context->policies;
        context->policies = removed->next;
        osFreeMem(removed);
    }
}

static void mqtt_nocloud_block(mqtt_nocloud_filter_result_t *result,
                               const char *filter_id, size_t removed_count)
{
    result->action = MQTT_NOCLOUD_BLOCK;
    result->filter_id = filter_id;
    result->removed_count = removed_count;
}

static void mqtt_nocloud_finish_array(cJSON *json, const char *filter_id,
                                      size_t removed,
                                      mqtt_nocloud_filter_result_t *result)
{
    if (removed == 0)
    {
        cJSON_Delete(json);
        return;
    }
    if (cJSON_GetArraySize(json) == 0)
    {
        cJSON_Delete(json);
        mqtt_nocloud_block(result, filter_id, removed);
        return;
    }

    result->payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (result->payload == NULL)
    {
        mqtt_nocloud_block(result, filter_id, removed);
        return;
    }
    result->payload_len = osStrlen(result->payload);
    result->removed_count = removed;
    result->filter_id = filter_id;
    result->action = MQTT_NOCLOUD_REWRITE;
}

static bool_t mqtt_nocloud_item_is_protected(
    mqtt_nocloud_context_t *context, const cJSON *item, bool_t *valid)
{
    *valid = TRUE;
    const cJSON *tonie = cJSON_IsObject(item) ?
                             cJSON_GetObjectItemCaseSensitive(item, "tonie") :
                             item;
    if (!cJSON_IsString(tonie) || tonie->valuestring == NULL)
        return FALSE;

    char normalized[TB2_RUID_SIZE];
    if (!tb2_ruid_canonicalize(tonie->valuestring, normalized))
        return FALSE;

    bool_t protected = FALSE;
    if (!mqtt_nocloud_is_protected(context, normalized, &protected))
    {
        *valid = FALSE;
        return TRUE;
    }
    return protected;
}

static void mqtt_nocloud_filter_array_json(
    mqtt_nocloud_context_t *context, cJSON *json, const char *filter_id,
    mqtt_nocloud_filter_result_t *result)
{
    if (!cJSON_IsArray(json))
    {
        cJSON_Delete(json);
        mqtt_nocloud_block(result, filter_id, 0);
        return;
    }

    size_t removed = 0;
    int index = 0;
    while (index < cJSON_GetArraySize(json))
    {
        bool_t valid = TRUE;
        cJSON *item = cJSON_GetArrayItem(json, index);
        if (mqtt_nocloud_item_is_protected(context, item, &valid))
        {
            if (!valid)
            {
                cJSON_Delete(json);
                mqtt_nocloud_block(result, filter_id, removed);
                return;
            }
            cJSON_DeleteItemFromArray(json, index);
            removed++;
            continue;
        }
        index++;
    }

    mqtt_nocloud_finish_array(json, filter_id, removed, result);
}

static void mqtt_nocloud_filter_array(mqtt_nocloud_context_t *context,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      const char *filter_id,
                                      mqtt_nocloud_filter_result_t *result)
{
    cJSON *json = mqtt_nocloud_parse_json(payload, payload_len);
    mqtt_nocloud_filter_array_json(context, json, filter_id, result);
}

static void mqtt_nocloud_filter_playback(mqtt_nocloud_context_t *context,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         mqtt_nocloud_filter_result_t *result)
{
    static const char filter_id[] = "nocloud.playback_state";
    cJSON *json = mqtt_nocloud_parse_json(payload, payload_len);
    if (!cJSON_IsObject(json))
    {
        cJSON_Delete(json);
        mqtt_nocloud_block(result, filter_id, 0);
        return;
    }

    cJSON *tonie = cJSON_GetObjectItemCaseSensitive(json, "tonie");
    if (cJSON_IsNull(tonie) || !cJSON_IsString(tonie) ||
        tonie->valuestring == NULL)
    {
        cJSON_Delete(json);
        return;
    }

    bool_t protected = FALSE;
    if (!mqtt_nocloud_is_protected(context, tonie->valuestring, &protected))
        protected = TRUE;
    cJSON_Delete(json);
    if (protected)
        mqtt_nocloud_block(result, filter_id, 1);
}

static void mqtt_nocloud_filter_fresh_tonies(
    mqtt_nocloud_context_t *context, const uint8_t *payload,
    size_t payload_len, mqtt_nocloud_filter_result_t *result)
{
    static const char filter_id[] = "nocloud.fresh_tonies";
    cJSON *json = mqtt_nocloud_parse_json(payload, payload_len);
    if (cJSON_IsArray(json))
    {
        mqtt_nocloud_filter_array_json(context, json, filter_id, result);
        return;
    }
    if (!cJSON_IsObject(json))
    {
        cJSON_Delete(json);
        mqtt_nocloud_block(result, filter_id, 0);
        return;
    }

    bool_t valid = TRUE;
    bool_t protected = mqtt_nocloud_item_is_protected(context, json, &valid);
    cJSON_Delete(json);
    if (!valid || protected)
        mqtt_nocloud_block(result, filter_id, protected ? 1 : 0);
}

static bool_t mqtt_nocloud_payload_has_protected_ruid(
    mqtt_nocloud_context_t *context, const uint8_t *payload,
    size_t payload_len);

static void mqtt_nocloud_filter_logs(mqtt_nocloud_context_t *context,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     mqtt_nocloud_filter_result_t *result)
{
    static const char filter_id[] = "nocloud.logs";
    cJSON *json = mqtt_nocloud_parse_json(payload, payload_len);
    if (cJSON_IsArray(json))
    {
        size_t removed = 0;
        int index = 0;
        while (index < cJSON_GetArraySize(json))
        {
            cJSON *item = cJSON_GetArrayItem(json, index);
            char *serialized = cJSON_PrintUnformatted(item);
            if (serialized == NULL)
            {
                cJSON_Delete(json);
                mqtt_nocloud_block(result, filter_id, removed);
                return;
            }
            bool_t protected = mqtt_nocloud_payload_has_protected_ruid(
                context, (const uint8_t *)serialized, osStrlen(serialized));
            cJSON_free(serialized);
            if (protected)
            {
                cJSON_DeleteItemFromArray(json, index);
                removed++;
                continue;
            }
            index++;
        }
        mqtt_nocloud_finish_array(json, filter_id, removed, result);
        return;
    }
    cJSON_Delete(json);
    if (mqtt_nocloud_payload_has_protected_ruid(context, payload, payload_len))
        mqtt_nocloud_block(result, filter_id, 1);
}

static bool_t mqtt_nocloud_payload_has_protected_ruid(
    mqtt_nocloud_context_t *context, const uint8_t *payload,
    size_t payload_len)
{
    size_t index = 0;
    while (index < payload_len)
    {
        if (!isxdigit(payload[index]) ||
            (index > 0 && isxdigit(payload[index - 1])))
        {
            index++;
            continue;
        }

        size_t end = index;
        while (end < payload_len && isxdigit(payload[end]))
            end++;
        if (end - index == TB2_RUID_HEX_LENGTH)
        {
            char ruid[TB2_RUID_SIZE];
            osMemcpy(ruid, payload + index, TB2_RUID_HEX_LENGTH);
            ruid[TB2_RUID_HEX_LENGTH] = '\0';
            bool_t protected = FALSE;
            if (!mqtt_nocloud_is_protected(context, ruid, &protected))
                protected = TRUE;
            if (protected)
                return TRUE;
        }
        index = end;
    }
    return FALSE;
}

void mqtt_nocloud_filter_publish(settings_t *box_settings,
                                 bool_t box_to_upstream,
                                 const char *topic,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 mqtt_nocloud_filter_result_t *result)
{
    if (result == NULL)
        return;
    osMemset(result, 0, sizeof(*result));
    result->action = MQTT_NOCLOUD_ALLOW;

    if (box_to_upstream &&
        mqtt_nocloud_payload_contains(payload, payload_len, "teddycloud_"))
    {
        mqtt_nocloud_block(result, "local_content.teddycloud_payload", 0);
        return;
    }

    const char *path = mqtt_nocloud_topic_path(topic);
    if (path == NULL || box_settings == NULL)
        return;

    mqtt_nocloud_context_t context = {
        .settings = box_settings,
        .policies = NULL,
    };

    static const char claim_prefix[] = "claim/";
    if (box_to_upstream &&
        osStrncmp(path, claim_prefix, sizeof(claim_prefix) - 1) == 0 &&
        osStrchr(path + sizeof(claim_prefix) - 1, '/') == NULL)
    {
        char normalized[TB2_RUID_SIZE];
        if (tb2_ruid_canonicalize(
                path + sizeof(claim_prefix) - 1, normalized))
        {
            bool_t protected = FALSE;
            if (!mqtt_nocloud_is_protected(&context, normalized, &protected))
                protected = TRUE;
            if (protected)
                mqtt_nocloud_block(result, "nocloud.claim", 1);
        }
    }
    else if (box_to_upstream && osStrcmp(path, "playback/state") == 0)
    {
        mqtt_nocloud_filter_playback(&context, payload, payload_len, result);
    }
    else if (box_to_upstream && osStrcmp(path, "metrics/fleet") == 0)
    {
        mqtt_nocloud_filter_array(&context, payload, payload_len,
                                  "nocloud.metrics_fleet", result);
    }
    else if (box_to_upstream && osStrcmp(path, "metrics/events") == 0)
    {
        mqtt_nocloud_filter_array(&context, payload, payload_len,
                                  "nocloud.metrics_events", result);
    }
    else if (box_to_upstream && osStrcmp(path, "bi-events") == 0)
    {
        mqtt_nocloud_filter_array(&context, payload, payload_len,
                                  "nocloud.bi_events", result);
    }
    else if (box_to_upstream && osStrcmp(path, "logs") == 0)
    {
        mqtt_nocloud_filter_logs(&context, payload, payload_len, result);
    }
    else if (!box_to_upstream && osStrcmp(path, "fresh-tonies") == 0)
    {
        mqtt_nocloud_filter_fresh_tonies(&context, payload, payload_len,
                                         result);
    }

    mqtt_nocloud_free_context(&context);
}

void mqtt_nocloud_filter_result_free(mqtt_nocloud_filter_result_t *result)
{
    if (result == NULL)
        return;
    if (result->payload != NULL)
        cJSON_free(result->payload);
    result->payload = NULL;
    result->payload_len = 0;
}
