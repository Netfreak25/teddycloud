#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fs_ext.h"
#include "mqtt_forward_filter.h"
#include "mqtt_settings.h"
#include "mutex_manager.h"

#define MQTT_SETTINGS_SCHEMA_VERSION 1
#define MQTT_SETTINGS_OVERLAY_ID_MAX 64U

const mqtt_settings_descriptor_t mqtt_settings_descriptors[MQTT_TB2_SETTING_COUNT] = {
    {"max_volume", "toniebox2.max_volume"},
    {"bedtime_max_volume", "toniebox2.bedtime_max_volume"},
    {"max_headphone_volume", "toniebox2.max_headphone_volume"},
    {"bedtime_max_headphone_volume", "toniebox2.bedtime_max_headphone_volume"},
    {"lightring_brightness", "toniebox2.lightring_brightness"},
    {"bedtime_lightring_brightness", "toniebox2.bedtime_lightring_brightness"},
    {"scrubbing_enabled", "toniebox2.scrubbing_enabled"},
    {"skipping_enabled", "toniebox2.slap_enabled"},
    {"skipping_direction", "toniebox2.slap_back_left"},
    {"age_mode", "toniebox2.baby_mode"},
};

typedef struct
{
    char overlay_id[MQTT_SETTINGS_OVERLAY_ID_MAX + 1U];
    bool loaded;
    bool projection_save_pending;
    cJSON *desired;
    uint64_t confirmed[MQTT_TB2_SETTING_COUNT];
    uint16_t confirmed_mask;
} mqtt_settings_snapshot_t;

/* Bounded by existing overlays, not by incoming messages. No settings pointers
 * survive a reload. Lock order is always SETTINGS, then MQTT_SETTINGS. */
static mqtt_settings_snapshot_t snapshots[MAX_OVERLAYS];

int mqtt_settings_field_index(const char *setting_name)
{
    if (setting_name != NULL)
    {
        for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
            if (strcmp(setting_name, mqtt_settings_descriptors[i].setting_name) == 0)
                return (int)i;
    }
    return -1;
}

static int mqtt_settings_json_index(const char *name)
{
    for (size_t i = 0; name != NULL && i < MQTT_TB2_SETTING_COUNT; i++)
        if (strcmp(name, mqtt_settings_descriptors[i].json_name) == 0)
            return (int)i;
    return -1;
}

bool mqtt_settings_cloud_managed(settings_t *settings)
{
    if (settings == NULL || settings->internal.overlayNumber == 0 ||
        !settings->internal.config_used || settings->toniebox.boxGeneration != GENERATION_TB2 ||
        !settings_get_bool("mqtt_client_upstream.enabled"))
        return false;

    /* Only the suffix classifies this synthetic decision topic. It cannot
     * select an overlay; the connection's settings supply that identity. */
    mqtt_forward_filter_result_t decision = mqtt_forward_filter_evaluate(
        settings, MQTT_FORWARD_ROUTE_TONIES_TO_BOX,
        "toniebox/settings-policy/settings/desired", NULL, 0);
    return decision.action == MQTT_FORWARD_ACTION_FORWARD;
}

bool mqtt_settings_json_revision(const cJSON *item, uint64_t *revision)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > (double)MQTT_SETTINGS_REVISION_MAX)
        return false;
    uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble)
        return false;
    if (revision != NULL)
        *revision = value;
    return true;
}

static bool mqtt_settings_value_valid(int index, const cJSON *value)
{
    uint64_t number;
    if (value == NULL)
        return false;
    if (index < 0)
        return true;
    if (index <= MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS)
        return mqtt_settings_json_revision(value, &number) && number <= 100;
    if (index <= MQTT_TB2_SETTING_SKIPPING_ENABLED)
        return cJSON_IsBool(value);
    if (!cJSON_IsString(value) || value->valuestring == NULL)
        return false;
    if (index == MQTT_TB2_SETTING_SKIPPING_DIRECTION)
        return strcmp(value->valuestring, "left") == 0 || strcmp(value->valuestring, "right") == 0;
    return strcmp(value->valuestring, "1+") == 0 || strcmp(value->valuestring, "3+") == 0;
}

static bool mqtt_settings_overlay_valid(settings_t *settings)
{
    if (settings == NULL || settings->internal.overlayNumber == 0 ||
        settings->internal.overlayNumber >= MAX_OVERLAYS || !settings->internal.config_used)
        return false;
    const char *id = settings->internal.overlayUniqueId;
    if (id == NULL || id[0] == '\0' || strlen(id) > MQTT_SETTINGS_OVERLAY_ID_MAX)
        return false;
    for (; *id != '\0'; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
              (*id >= '0' && *id <= '9') || *id == '-' || *id == '_'))
            return false;
    return true;
}

static bool mqtt_settings_unique_object(const cJSON *object)
{
    if (!cJSON_IsObject(object))
        return false;
    for (const cJSON *field = object->child; field != NULL; field = field->next)
    {
        if (field->string == NULL || field->string[0] == '\0')
            return false;
        for (const cJSON *other = field->next; other != NULL; other = other->next)
            if (other->string != NULL && strcmp(field->string, other->string) == 0)
                return false;
    }
    return true;
}

static cJSON *mqtt_settings_parse(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0 || length > MQTT_SETTINGS_SNAPSHOT_LIMIT ||
        memchr(data, '\0', length) != NULL)
        return NULL;
    const char *end = NULL;
    cJSON *object = cJSON_ParseWithLengthOpts((const char *)data, length, &end, false);
    const char *limit = (const char *)data + length;
    while (end != NULL && end < limit && isspace((unsigned char)*end))
        end++;
    if (object == NULL || end != limit || !mqtt_settings_unique_object(object))
    {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static bool mqtt_settings_paths(settings_t *settings, char *directory, char *path)
{
    const char *base = get_settings()->internal.configdirfull;
    if (!mqtt_settings_overlay_valid(settings) || base == NULL)
        return false;
    int n = snprintf(directory, PATH_LEN, "%s/mqtt-settings", base);
    int p = snprintf(path, PATH_LEN, "%s/%s.json", directory, settings->internal.overlayUniqueId);
    return n > 0 && n < PATH_LEN && p > 0 && p < PATH_LEN;
}

static bool mqtt_settings_snapshot_valid(cJSON *root, const char *overlay)
{
    uint64_t schema;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "overlay");
    cJSON *desired = cJSON_GetObjectItemCaseSensitive(root, "desired");
    cJSON *history = cJSON_GetObjectItemCaseSensitive(desired, "settings_history");
    if (!mqtt_settings_json_revision(cJSON_GetObjectItemCaseSensitive(root, "schemaVersion"), &schema) ||
        schema != MQTT_SETTINGS_SCHEMA_VERSION || !cJSON_IsString(id) ||
        strcmp(id->valuestring, overlay) != 0 || !mqtt_settings_unique_object(desired) ||
        !mqtt_settings_unique_object(history))
        return false;
    for (cJSON *revision = history->child; revision != NULL; revision = revision->next)
    {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(desired, revision->string);
        if (!strcmp(revision->string, "settings_history") ||
            !strcmp(revision->string, "settings_applied") ||
            !mqtt_settings_json_revision(revision, NULL) ||
            !mqtt_settings_value_valid(mqtt_settings_json_index(revision->string), value))
            return false;
    }
    for (cJSON *value = desired->child; value != NULL; value = value->next)
    {
        if (strcmp(value->string, "settings_history") == 0)
            continue;
        if (strcmp(value->string, "settings_applied") == 0 && cJSON_IsBool(value))
            continue;
        if (cJSON_GetObjectItemCaseSensitive(history, value->string) == NULL)
            return false;
    }
    return true;
}

static mqtt_settings_snapshot_t *mqtt_settings_load_locked(settings_t *settings)
{
    char directory[PATH_LEN], path[PATH_LEN];
    if (!mqtt_settings_paths(settings, directory, path))
        return NULL;
    mqtt_settings_snapshot_t *slot = &snapshots[settings->internal.overlayNumber];
    if (strcmp(slot->overlay_id, settings->internal.overlayUniqueId) != 0)
    {
        cJSON_Delete(slot->desired);
        memset(slot, 0, sizeof(*slot));
        strcpy(slot->overlay_id, settings->internal.overlayUniqueId);
    }
    if (slot->loaded)
        return slot;
    slot->loaded = true;
    if (!fsFileExists(path))
        return slot;

    uint32_t size = 0;
    error_t error = fsGetFileSize(path, &size);
    uint8_t *buffer = NULL;
    cJSON *root = NULL;
    FsFile *file = NULL;
    if (error == NO_ERROR && size > 0 && size <= MQTT_SETTINGS_SNAPSHOT_LIMIT)
    {
        buffer = osAllocMem(size);
        file = buffer != NULL ? fsOpenFile(path, FS_FILE_MODE_READ) : NULL;
        size_t received = 0;
        if (file != NULL && fsReadFile(file, buffer, size, &received) == NO_ERROR && received == size)
            root = mqtt_settings_parse(buffer, size);
    }
    if (root != NULL && mqtt_settings_snapshot_valid(root, slot->overlay_id))
        slot->desired = cJSON_DetachItemFromObjectCaseSensitive(root, "desired");
    else
        TRACE_WARNING("MQTT settings snapshot ignored overlay=%s: invalid or unreadable snapshot\r\n", slot->overlay_id);
    cJSON_Delete(root);
    if (file != NULL)
        fsCloseFile(file);
    osFreeMem(buffer);
    return slot;
}

static bool mqtt_settings_set_json(cJSON *object, const char *name, const cJSON *value)
{
    cJSON *copy = cJSON_Duplicate(value, true);
    if (copy == NULL)
        return false;
    bool success = cJSON_HasObjectItem(object, name)
                       ? cJSON_ReplaceItemInObjectCaseSensitive(object, name, copy)
                       : cJSON_AddItemToObject(object, name, copy);
    if (!success)
        cJSON_Delete(copy);
    return success;
}

static error_t mqtt_settings_write_locked(settings_t *settings, const cJSON *desired)
{
    char directory[PATH_LEN], path[PATH_LEN], temporary[PATH_LEN];
    if (!mqtt_settings_paths(settings, directory, path))
        return ERROR_INVALID_PARAMETER;
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count < 0 || (size_t)count >= sizeof(temporary))
        return ERROR_INVALID_PARAMETER;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return ERROR_OUT_OF_MEMORY;
    bool complete = cJSON_AddNumberToObject(root, "schemaVersion", MQTT_SETTINGS_SCHEMA_VERSION) &&
                    cJSON_AddStringToObject(root, "overlay", settings->internal.overlayUniqueId) &&
                    mqtt_settings_set_json(root, "desired", desired);
    char *encoded = complete ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (encoded == NULL)
        return ERROR_OUT_OF_MEMORY;
    size_t size = strlen(encoded);
    error_t error = size > MQTT_SETTINGS_SNAPSHOT_LIMIT ? ERROR_INVALID_LENGTH : NO_ERROR;
    if (error == NO_ERROR && !fsDirExists(directory))
        error = fsCreateDirEx(directory, true);
    FsFile *file = NULL;
    if (error == NO_ERROR)
    {
        file = fsOpenFile(temporary, FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
        error = file == NULL ? ERROR_FILE_OPENING_FAILED : fsWriteFile(file, encoded, size);
    }
    if (error == NO_ERROR)
        error = fsFlushFile(file);
    if (file != NULL)
        fsCloseFile(file);
    if (error == NO_ERROR)
        error = fsRenameFile(temporary, path);
    if (error != NO_ERROR)
        fsDeleteFile(temporary);
    cJSON_free(encoded);
    return error;
}

/* Caller holds SETTINGS then MQTT_SETTINGS. Setters for nonzero overlays do
 * not enter settings_changed_id or invoke MQTT hooks. */
static error_t mqtt_settings_project_locked(settings_t *settings, const cJSON *desired,
                                             uint16_t fields, bool *changed)
{
    *changed = false;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        if (!(fields & (1U << i)))
            continue;
        const char *key = mqtt_settings_descriptors[i].setting_name;
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(desired, mqtt_settings_descriptors[i].json_name);
        if (!mqtt_settings_value_valid((int)i, value))
            continue;
        setting_item_t *option = settings_get_by_name_ovl(key, settings->internal.overlayUniqueId);
        if (option == NULL || option->ptr == NULL)
            return ERROR_NOT_FOUND;
        bool ok;
        if (i <= MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS)
        {
            uint32_t number = (uint32_t)value->valuedouble;
            if (option->overlayed && *((uint32_t *)option->ptr) == number)
                continue;
            ok = settings_set_unsigned_id(key, number, settings->internal.overlayNumber);
        }
        else
        {
            bool enabled = i == MQTT_TB2_SETTING_SKIPPING_DIRECTION ? strcmp(value->valuestring, "left") == 0
                          : i == MQTT_TB2_SETTING_AGE_MODE ? strcmp(value->valuestring, "1+") == 0
                          : cJSON_IsTrue(value);
            if (option->overlayed && *((bool *)option->ptr) == enabled)
                continue;
            ok = settings_set_bool_id(key, enabled, settings->internal.overlayNumber);
        }
        if (!ok)
            return ERROR_INVALID_PARAMETER;
        *changed = true;
    }
    if (*changed)
        settings->internal.config_changed = true;
    return NO_ERROR;
}

error_t mqtt_settings_accept_cloud(settings_t *settings, const uint8_t *payload,
                                  size_t length, uint16_t *accepted_mask)
{
    if (accepted_mask != NULL)
        *accepted_mask = 0;
    cJSON *incoming = mqtt_settings_parse(payload, length);
    cJSON *history = cJSON_GetObjectItemCaseSensitive(incoming, "settings_history");
    if (incoming == NULL || !mqtt_settings_unique_object(history))
    {
        cJSON_Delete(incoming);
        return ERROR_INVALID_SYNTAX;
    }

    mutex_lock(MUTEX_SETTINGS);
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    error_t error = slot == NULL ? ERROR_INVALID_PARAMETER : NO_ERROR;
    cJSON *merged = slot != NULL && slot->desired != NULL ? cJSON_Duplicate(slot->desired, true) : cJSON_CreateObject();
    cJSON *merged_history = cJSON_GetObjectItemCaseSensitive(merged, "settings_history");
    if (merged != NULL && merged_history == NULL)
        merged_history = cJSON_AddObjectToObject(merged, "settings_history");
    if (error == NO_ERROR && (merged == NULL || merged_history == NULL))
        error = ERROR_OUT_OF_MEMORY;
    uint16_t accepted = 0;
    bool changed = false;
    for (cJSON *revision = history->child; error == NO_ERROR && revision != NULL; revision = revision->next)
    {
        const char *name = revision->string;
        cJSON *value = cJSON_GetObjectItemCaseSensitive(incoming, name);
        int index = mqtt_settings_json_index(name);
        uint64_t next, previous;
        if (!strcmp(name, "settings_history") || !strcmp(name, "settings_applied") ||
            !mqtt_settings_json_revision(revision, &next) || !mqtt_settings_value_valid(index, value))
        {
            TRACE_WARNING("MQTT settings field rejected overlay=%s field=%s reason=invalid_pair\r\n", slot->overlay_id, name);
            continue;
        }
        bool had_previous = mqtt_settings_json_revision(cJSON_GetObjectItemCaseSensitive(merged_history, name), &previous);
        bool same = had_previous && previous == next;
        if (had_previous && (previous > next ||
            (same && !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(merged, name), value, true))))
        {
            TRACE_WARNING("MQTT settings field rejected overlay=%s field=%s reason=old_or_conflicting_revision\r\n", slot->overlay_id, name);
            continue;
        }
        if (!same)
        {
            if (!mqtt_settings_set_json(merged, name, value) || !mqtt_settings_set_json(merged_history, name, revision))
            {
                error = ERROR_OUT_OF_MEMORY;
                break;
            }
            changed = true;
        }
        if (index >= 0)
            accepted |= (uint16_t)(1U << index);
    }
    /* Keep this protocol envelope flag only when actually received, not as an
     * invented Cloud setting. It is not a versioned device setting. */
    cJSON *applied = cJSON_GetObjectItemCaseSensitive(incoming, "settings_applied");
    if (error == NO_ERROR && merged_history->child != NULL && cJSON_IsBool(applied) &&
        !cJSON_Compare(applied, cJSON_GetObjectItemCaseSensitive(merged, "settings_applied"), true))
    {
        if (!mqtt_settings_set_json(merged, "settings_applied", applied))
            error = ERROR_OUT_OF_MEMORY;
        else
            changed = true;
    }
    if (error == NO_ERROR && changed)
        error = mqtt_settings_write_locked(settings, merged);
    if (error == NO_ERROR && (changed || slot->desired != NULL))
    {
        if (changed)
        {
            for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
            {
                const char *name = mqtt_settings_descriptors[i].json_name;
                const cJSON *old_history = cJSON_GetObjectItemCaseSensitive(slot->desired, "settings_history");
                if (!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(old_history, name),
                                   cJSON_GetObjectItemCaseSensitive(merged_history, name), true))
                    slot->confirmed_mask &= (uint16_t)~(1U << i);
            }
            cJSON_Delete(slot->desired);
            slot->desired = merged;
            merged = NULL;
        }
        if (accepted_mask != NULL)
            *accepted_mask = accepted;
        bool projected;
        error = mqtt_settings_project_locked(settings, slot->desired, accepted, &projected);
        slot->projection_save_pending |= projected;
        if (error == NO_ERROR && slot->projection_save_pending)
        {
            error = settings_save_overlays_locked();
            if (error == NO_ERROR)
                slot->projection_save_pending = false;
        }
        TRACE_INFO("MQTT settings received overlay=%s fields=0x%03x snapshot=%s\r\n",
                   slot->overlay_id, accepted, changed ? "updated" : "unchanged");
    }
    if (error != NO_ERROR)
        TRACE_WARNING("MQTT settings import failed overlay=%s error=%d; Cloud forwarding retained\r\n",
                      slot != NULL ? slot->overlay_id : "invalid", error);
    cJSON_Delete(merged);
    mutex_unlock(MUTEX_MQTT_SETTINGS);
    mutex_unlock(MUTEX_SETTINGS);
    cJSON_Delete(incoming);
    return error;
}

cJSON *mqtt_settings_snapshot_copy(settings_t *settings)
{
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    cJSON *copy = slot != NULL && slot->desired != NULL ? cJSON_Duplicate(slot->desired, true) : NULL;
    mutex_unlock(MUTEX_MQTT_SETTINGS);
    return copy;
}

uint64_t mqtt_settings_revision_floor(settings_t *settings)
{
    uint64_t highest = 0;
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    cJSON *history = cJSON_GetObjectItemCaseSensitive(slot != NULL ? slot->desired : NULL, "settings_history");
    for (cJSON *field = history != NULL ? history->child : NULL; field != NULL; field = field->next)
    {
        uint64_t revision;
        if (mqtt_settings_json_revision(field, &revision) && revision > highest)
            highest = revision;
    }
    mutex_unlock(MUTEX_MQTT_SETTINGS);
    return highest;
}

const char *mqtt_settings_field_state(settings_t *settings, size_t index)
{
    if (!mqtt_settings_cloud_managed(settings))
        return "local";
    const char *state = "waiting";
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    if (slot != NULL && index < MQTT_TB2_SETTING_COUNT)
    {
        cJSON *history = cJSON_GetObjectItemCaseSensitive(slot->desired, "settings_history");
        uint64_t revision;
        if (mqtt_settings_json_revision(cJSON_GetObjectItemCaseSensitive(history, mqtt_settings_descriptors[index].json_name), &revision))
            state = (slot->confirmed_mask & (1U << index)) && slot->confirmed[index] == revision ? "confirmed" : "received";
    }
    mutex_unlock(MUTEX_MQTT_SETTINGS);
    return state;
}

void mqtt_settings_confirm_cloud(settings_t *settings, cJSON *toniebox_history)
{
    if (!mqtt_settings_unique_object(toniebox_history))
        return;
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    if (slot != NULL)
    {
        cJSON *history = cJSON_GetObjectItemCaseSensitive(slot->desired, "settings_history");
        for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
        {
            const char *name = mqtt_settings_descriptors[i].json_name;
            uint64_t expected, confirmed;
            if (mqtt_settings_json_revision(cJSON_GetObjectItemCaseSensitive(history, name), &expected) &&
                mqtt_settings_json_revision(cJSON_GetObjectItemCaseSensitive(toniebox_history, name), &confirmed) &&
                expected == confirmed)
            {
                if (!(slot->confirmed_mask & (1U << i)) || slot->confirmed[i] != confirmed)
                    TRACE_INFO("MQTT Cloud settings confirmed overlay=%s field=%s\r\n", slot->overlay_id, name);
                slot->confirmed_mask |= (uint16_t)(1U << i);
                slot->confirmed[i] = confirmed;
            }
        }
    }
    mutex_unlock(MUTEX_MQTT_SETTINGS);
}

void mqtt_settings_local_sent(settings_t *settings, size_t index)
{
    if (index >= MQTT_TB2_SETTING_COUNT)
        return;
    mutex_lock(MUTEX_MQTT_SETTINGS);
    mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
    if (slot != NULL)
        slot->confirmed_mask &= (uint16_t)~(1U << index);
    mutex_unlock(MUTEX_MQTT_SETTINGS);
}

void mqtt_settings_reapply_overlays_locked(void)
{
    mutex_lock(MUTEX_MQTT_SETTINGS);
    for (uint8_t id = 1; id < MAX_OVERLAYS; id++)
    {
        settings_t *settings = get_settings_id(id);
        if (!mqtt_settings_overlay_valid(settings) || !mqtt_settings_cloud_managed(settings))
            continue;
        mqtt_settings_snapshot_t *slot = mqtt_settings_load_locked(settings);
        if (slot != NULL && slot->desired != NULL)
        {
            bool changed;
            error_t error = mqtt_settings_project_locked(settings, slot->desired,
                                      (1U << MQTT_TB2_SETTING_COUNT) - 1U, &changed);
            if (error != NO_ERROR)
                TRACE_WARNING("MQTT Cloud settings projection failed overlay=%s error=%d\r\n", slot->overlay_id, error);
        }
    }
    mutex_unlock(MUTEX_MQTT_SETTINGS);
}

static void mqtt_settings_sent_expire(mqtt_settings_sent_t *state, uint32_t now)
{
    if (state->transition_pending && (uint32_t)(now - state->transition_at) >= MQTT_SETTINGS_SENT_GRACE_MS)
    {
        for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
            if (state->transition_mask & (1U << i))
                state->valid[i] = false;
        state->transition_mask = 0;
        state->transition_pending = false;
    }
}

void mqtt_settings_sent_authority(mqtt_settings_sent_t *state, bool cloud, uint32_t now)
{
    mqtt_settings_sent_expire(state, now);
    if (state->authority_known && state->cloud_managed != cloud)
    {
        if (!state->transition_pending)
        {
            state->transition_pending = true;
            state->transition_at = now;
        }
        for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
            if (state->valid[i])
                state->transition_mask |= (uint16_t)(1U << i);
    }
    state->authority_known = true;
    state->cloud_managed = cloud;
}

void mqtt_settings_sent_record(mqtt_settings_sent_t *state, size_t index, uint64_t revision)
{
    if (index >= MQTT_TB2_SETTING_COUNT || revision > MQTT_SETTINGS_REVISION_MAX)
        return;
    if (!state->valid[index] || state->revision[index] != revision)
    {
        state->transition_mask &= (uint16_t)~(1U << index);
        state->ambiguous[index] = false;
    }
    state->revision[index] = revision;
    state->valid[index] = true;
}

void mqtt_settings_sent_cloud(mqtt_settings_sent_t *state, size_t index, uint64_t revision)
{
    if (index < MQTT_TB2_SETTING_COUNT && state->valid[index] && state->revision[index] == revision)
        state->ambiguous[index] = true;
}

bool mqtt_settings_sent_match(mqtt_settings_sent_t *state, size_t index,
                             uint64_t revision, uint32_t now)
{
    mqtt_settings_sent_expire(state, now);
    return index < MQTT_TB2_SETTING_COUNT && state->valid[index] &&
           !state->ambiguous[index] && state->revision[index] == revision;
}
