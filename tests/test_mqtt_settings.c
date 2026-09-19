/* Bounded executable test: real settings module, cJSON and forwarding matcher.
 * Only project I/O boundaries are stubbed; snapshots use actual temporary files.
 * Including the module permits a process-restart simulation without adding a
 * production-only reset/test API. */
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/mqtt_settings.c"

static settings_t test_settings[MAX_OVERLAYS];
static setting_item_t options[MAX_OVERLAYS][MQTT_TB2_SETTING_COUNT];
static uint32_t numbers[MAX_OVERLAYS][MQTT_TB2_SETTING_COUNT];
static bool booleans[MAX_OVERLAYS][MQTT_TB2_SETTING_COUNT];
static bool upstream = true, master = true, global_desired = true, box_desired = true;
static setting_item_t global_filter = {.type = TYPE_BOOL, .ptr = &global_desired};
static setting_item_t box_filter = {.type = TYPE_BOOL, .ptr = &box_desired};
static bool locked[MUTEX_LAST];
static unsigned saves, renames, projections;
static int fail_io; /* 1 write, 2 flush, 3 rename, 4 overlay save. */

settings_t *get_settings(void) { return &test_settings[0]; }
settings_t *get_settings_id(uint8_t id) { return &test_settings[id]; }
bool settings_get_bool(const char *key)
{
    if (strcmp(key, "mqtt_client_upstream.enabled") == 0)
        return upstream;
    assert(strcmp(key, "mqtt_client_upstream.filters_enabled") == 0);
    return master;
}

setting_item_t *settings_get_by_name_ovl(const char *key, const char *overlay)
{
    if (strcmp(key, "mqtt_client_upstream.forward.settings.desired") == 0)
        return overlay != NULL ? &box_filter : &global_filter;
    int index = mqtt_settings_field_index(key);
    assert(index >= 0);
    for (uint8_t id = 1; id < MAX_OVERLAYS; id++)
        if (test_settings[id].internal.overlayUniqueId != NULL &&
            strcmp(test_settings[id].internal.overlayUniqueId, overlay) == 0)
            return &options[id][index];
    return NULL;
}

bool settings_set_unsigned_id(const char *key, uint32_t value, uint8_t id)
{
    int index = mqtt_settings_field_index(key);
    assert(index >= 0 && options[id][index].type == TYPE_UNSIGNED);
    numbers[id][index] = value;
    options[id][index].overlayed = true;
    projections++;
    return true;
}

bool settings_set_bool_id(const char *key, bool value, uint8_t id)
{
    int index = mqtt_settings_field_index(key);
    assert(index >= 0 && options[id][index].type == TYPE_BOOL);
    booleans[id][index] = value;
    options[id][index].overlayed = true;
    projections++;
    return true;
}

error_t settings_save_overlays_locked(void)
{
    assert(locked[MUTEX_SETTINGS]);
    saves++;
    return fail_io == 4 ? ERROR_WRITE_FAILED : NO_ERROR;
}

void mutex_lock(mutex_id_t id)
{
    assert(!locked[id]);
    if (id == MUTEX_SETTINGS)
        assert(!locked[MUTEX_MQTT_SETTINGS]);
    locked[id] = true;
}
void mutex_unlock(mutex_id_t id) { assert(locked[id]); locked[id] = false; }
void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *p) { free(p); }
void osSuspendAllTasks(void) {}
void osResumeAllTasks(void) {}

bool_t fsFileExists(const char *path) { struct stat s; return stat(path, &s) == 0 && S_ISREG(s.st_mode); }
bool_t fsDirExists(const char *path) { struct stat s; return stat(path, &s) == 0 && S_ISDIR(s.st_mode); }
error_t fsCreateDirEx(const char *path, bool_t recursive) { (void)recursive; return mkdir(path, 0700) == 0 ? NO_ERROR : ERROR_FAILURE; }
error_t fsGetFileSize(const char *path, uint32_t *size)
{
    struct stat s;
    if (stat(path, &s) != 0) return ERROR_FILE_NOT_FOUND;
    *size = s.st_size;
    return NO_ERROR;
}
FsFile *fsOpenFile(const char *path, uint_t mode) { return fopen(path, mode & FS_FILE_MODE_WRITE ? "wb" : "rb"); }
void fsCloseFile(FsFile *file) { assert(fclose(file) == 0); }
error_t fsWriteFile(FsFile *file, void *data, size_t size)
{
    if (fail_io == 1) return ERROR_WRITE_FAILED;
    return fwrite(data, 1, size, file) == size ? NO_ERROR : ERROR_WRITE_FAILED;
}
error_t fsReadFile(FsFile *file, void *data, size_t size, size_t *length)
{
    *length = fread(data, 1, size, file);
    return ferror(file) ? ERROR_READ_FAILED : NO_ERROR;
}
error_t fsFlushFile(FsFile *file) { return fail_io == 2 || fflush(file) != 0 ? ERROR_WRITE_FAILED : NO_ERROR; }
error_t fsRenameFile(const char *from, const char *to)
{
    if (fail_io == 3) return ERROR_FAILURE;
    if (rename(from, to) != 0) return ERROR_FAILURE;
    renames++;
    return NO_ERROR;
}
error_t fsDeleteFile(const char *path) { return unlink(path) == 0 ? NO_ERROR : ERROR_FILE_NOT_FOUND; }

static void setup_box(uint8_t id, char *name)
{
    settings_t *box = &test_settings[id];
    box->internal.overlayNumber = id;
    box->internal.overlayUniqueId = name;
    box->internal.config_used = true;
    box->toniebox.boxGeneration = GENERATION_TB2;
    for (size_t i = 0; i < MQTT_TB2_SETTING_COUNT; i++)
    {
        options[id][i].type = i <= MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS ? TYPE_UNSIGNED : TYPE_BOOL;
        options[id][i].ptr = i <= MQTT_TB2_SETTING_BEDTIME_LIGHTRING_BRIGHTNESS ? (void *)&numbers[id][i] : (void *)&booleans[id][i];
    }
}

static void restart_snapshots(void)
{
    for (size_t i = 0; i < MAX_OVERLAYS; i++) cJSON_Delete(snapshots[i].desired);
    memset(snapshots, 0, sizeof(snapshots));
}

static uint16_t accept(settings_t *box, const char *json)
{
    uint16_t mask = 0;
    assert(mqtt_settings_accept_cloud(box, (const uint8_t *)json, strlen(json), &mask) == NO_ERROR);
    return mask;
}

static void check_snapshot(settings_t *box, unsigned volume, unsigned revision)
{
    cJSON *snapshot = mqtt_settings_snapshot_copy(box);
    assert(snapshot != NULL);
    assert(cJSON_GetObjectItemCaseSensitive(snapshot, "max_volume")->valuedouble == volume);
    cJSON *history = cJSON_GetObjectItemCaseSensitive(snapshot, "settings_history");
    assert(cJSON_GetObjectItemCaseSensitive(history, "max_volume")->valuedouble == revision);
    cJSON_Delete(snapshot);
}

static void test_authority(settings_t *box)
{
    assert(mqtt_settings_cloud_managed(box));
    upstream = false;
    assert(!mqtt_settings_cloud_managed(box));
    upstream = true;
    global_desired = false;
    assert(!mqtt_settings_cloud_managed(box));
    box_filter.overlayed = true;
    assert(mqtt_settings_cloud_managed(box));
    box_desired = false;
    assert(!mqtt_settings_cloud_managed(box));
    master = false;
    assert(mqtt_settings_cloud_managed(box));
    master = global_desired = box_desired = true;
    box_filter.overlayed = false;
    box->toniebox.boxGeneration = GENERATION_TB1;
    assert(!mqtt_settings_cloud_managed(box));
    box->toniebox.boxGeneration = GENERATION_TB2;
    assert(!mqtt_settings_cloud_managed(get_settings()));
}

static void test_snapshot(settings_t *box)
{
    const char *first = "{\"settings_history\":{\"max_volume\":10,\"scrubbing_enabled\":11,\"skipping_direction\":12,\"age_mode\":13,\"alarms\":14,\"timezone\":15},\"max_volume\":70,\"scrubbing_enabled\":true,\"skipping_direction\":\"left\",\"age_mode\":\"1+\",\"alarms\":[{\"hour\":7}],\"timezone\":null}";
    assert(strcmp(mqtt_settings_field_state(box, 0), "waiting") == 0);
    uint16_t accepted = accept(box, first);
    assert(accepted == ((1U << 0) | (1U << 6) | (1U << 8) | (1U << 9)));
    assert(numbers[1][0] == 70 && booleans[1][6] && booleans[1][8] && booleans[1][9]);
    assert(mqtt_settings_revision_floor(box) == 15 && saves == 1);
    assert(strcmp(mqtt_settings_field_state(box, 0), "received") == 0);
    unsigned previous_renames = renames, previous_projections = projections;
    assert(accept(box, first) == accepted);
    assert(renames == previous_renames && projections == previous_projections && saves == 1);

    assert(accept(box, "{\"settings_history\":{\"max_volume\":9},\"max_volume\":60}") == 0);
    assert(accept(box, "{\"settings_history\":{\"max_volume\":10},\"max_volume\":60}") == 0);
    assert(accept(box, "{\"settings_history\":{\"max_volume\":10.5,\"scrubbing_enabled\":12,\"age_mode\":14},\"max_volume\":101,\"scrubbing_enabled\":1,\"age_mode\":\"2+\"}") == 0);
    assert(renames == previous_renames);
    check_snapshot(box, 70, 10);
    assert(accept(box, "{\"settings_history\":{\"max_volume\":20,\"alarms\":21},\"max_volume\":80,\"alarms\":[]}") == 1);
    cJSON *copy = mqtt_settings_snapshot_copy(box);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "alarms")) == 0);
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(copy, "timezone")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(copy, "scrubbing_enabled")));
    cJSON_Delete(copy);

    cJSON *confirmation = cJSON_Parse("{\"max_volume\":10}");
    mqtt_settings_confirm_cloud(box, confirmation);
    assert(strcmp(mqtt_settings_field_state(box, 0), "received") == 0);
    cJSON_Delete(confirmation);
    confirmation = cJSON_Parse("{\"max_volume\":20}");
    mqtt_settings_confirm_cloud(box, confirmation);
    assert(strcmp(mqtt_settings_field_state(box, 0), "confirmed") == 0);
    upstream = false;
    mqtt_settings_local_sent(box, 0);
    upstream = true;
    assert(strcmp(mqtt_settings_field_state(box, 0), "received") == 0);
    mqtt_settings_confirm_cloud(box, confirmation);
    assert(strcmp(mqtt_settings_field_state(box, 0), "confirmed") == 0);
    cJSON_Delete(confirmation);

    restart_snapshots();
    assert(strcmp(mqtt_settings_field_state(box, 0), "received") == 0);
    numbers[1][0] = 30; options[1][0].overlayed = false;
    unsigned previous_saves = saves;
    mutex_lock(MUTEX_SETTINGS);
    mqtt_settings_reapply_overlays_locked();
    mutex_unlock(MUTEX_SETTINGS);
    assert(numbers[1][0] == 80 && options[1][0].overlayed && saves == previous_saves);
    upstream = false;
    numbers[1][0] = 45;
    mutex_lock(MUTEX_SETTINGS);
    mqtt_settings_reapply_overlays_locked();
    mutex_unlock(MUTEX_SETTINGS);
    assert(numbers[1][0] == 45 && strcmp(mqtt_settings_field_state(box, 0), "local") == 0);
    upstream = true;
}

static void test_failures(settings_t *box)
{
    const char *next = "{\"settings_history\":{\"max_volume\":30},\"max_volume\":90}";
    uint16_t mask = 123;
    for (fail_io = 1; fail_io <= 3; fail_io++)
    {
        assert(mqtt_settings_accept_cloud(box, (const uint8_t *)next, strlen(next), &mask) != NO_ERROR);
        assert(mask == 0);
        check_snapshot(box, 80, 20);
        restart_snapshots();
        check_snapshot(box, 80, 20);
    }
    /* A snapshot published before a later config-write failure is authoritative
     * and retires stale pending intentions rather than echoing them back. */
    assert(mqtt_settings_accept_cloud(box, (const uint8_t *)next, strlen(next), &mask) != NO_ERROR);
    assert(mask == 1 && numbers[1][0] == 90);
    check_snapshot(box, 90, 30);
    fail_io = 0;
    unsigned retry_saves = saves;
    assert(accept(box, next) == 1 && saves == retry_saves + 1);
    assert(accept(box, next) == 1 && saves == retry_saves + 1);
    assert(mqtt_settings_accept_cloud(box, (const uint8_t *)"{}x", 3, &mask) != NO_ERROR);
    uint8_t *oversized = calloc(MQTT_SETTINGS_SNAPSHOT_LIMIT + 1, 1);
    assert(mqtt_settings_accept_cloud(box, oversized, MQTT_SETTINGS_SNAPSHOT_LIMIT + 1, &mask) != NO_ERROR);
    free(oversized);
    char *old_id = box->internal.overlayUniqueId;
    box->internal.overlayUniqueId = "../escaped";
    assert(mqtt_settings_accept_cloud(box, (const uint8_t *)next, strlen(next), &mask) != NO_ERROR);
    box->internal.overlayUniqueId = old_id;

    settings_t *other = &test_settings[2];
    assert(mqtt_settings_snapshot_copy(other) == NULL);
    accept(other, "{\"settings_history\":{\"max_volume\":4},\"max_volume\":15}");
    check_snapshot(other, 15, 4);
    check_snapshot(box, 90, 30);
    char directory[PATH_LEN], path[PATH_LEN];
    assert(mqtt_settings_paths(other, directory, path));
    FILE *bad = fopen(path, "wb"); assert(bad);
    assert(fwrite("{broken", 1, 7, bad) == 7); fclose(bad);
    restart_snapshots();
    assert(mqtt_settings_snapshot_copy(other) == NULL);
    check_snapshot(box, 90, 30);
}

static void test_sent_ledger(void)
{
    mqtt_settings_sent_t state = {0};
    mqtt_settings_sent_authority(&state, false, 100);
    assert(!mqtt_settings_sent_match(&state, 0, 1, 101));
    mqtt_settings_sent_record(&state, 0, 42);
    assert(mqtt_settings_sent_match(&state, 0, 42, 102));
    assert(!mqtt_settings_sent_match(&state, 0, 41, 102));
    mqtt_settings_sent_cloud(&state, 0, 42);
    assert(!mqtt_settings_sent_match(&state, 0, 42, 103));
    mqtt_settings_sent_record(&state, 0, 42);
    assert(!mqtt_settings_sent_match(&state, 0, 42, 104));
    mqtt_settings_sent_record(&state, 0, 43);
    assert(mqtt_settings_sent_match(&state, 0, 43, 105));
    mqtt_settings_sent_authority(&state, true, 200);
    mqtt_settings_sent_authority(&state, false, 250);
    mqtt_settings_sent_authority(&state, false, 300);
    assert(state.transition_at == 200);
    mqtt_settings_sent_record(&state, 1, 44);
    assert(mqtt_settings_sent_match(&state, 0, 43, 30199));
    assert(!mqtt_settings_sent_match(&state, 0, 43, 30200));
    assert(mqtt_settings_sent_match(&state, 1, 44, 30200));
    memset(&state, 0, sizeof(state));
    assert(!mqtt_settings_sent_match(&state, 1, 44, 30201));
    /* Rapid owner toggles must not leave newer entries outside the grace mask. */
    mqtt_settings_sent_authority(&state, false, 100);
    mqtt_settings_sent_authority(&state, true, 200);
    mqtt_settings_sent_authority(&state, false, 300);
    mqtt_settings_sent_record(&state, 0, 45);
    mqtt_settings_sent_authority(&state, true, 400);
    assert(state.transition_at == 200);
    assert(!mqtt_settings_sent_match(&state, 0, 45, 30200));
}

int main(void)
{
    char temporary[] = "/tmp/teddycloud-mqtt-settings.XXXXXX";
    char *directory = mkdtemp(temporary);
    assert(directory != NULL);
    test_settings[0].internal.configdirfull = directory;
    setup_box(1, "Box-A"); setup_box(2, "box-b");
    test_authority(&test_settings[1]);
    test_snapshot(&test_settings[1]);
    test_failures(&test_settings[1]);
    test_sent_ledger();
    restart_snapshots();
    printf("PASS: authority, snapshot merge/persist/reload/failure, Cloud confirms and local sent ledger (%s)\n", directory);
    return 0;
}
