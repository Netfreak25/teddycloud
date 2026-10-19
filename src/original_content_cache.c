/* Bounded original-content work, deliberately independent of box playback. */
#include "original_content_cache.h"
#include "mutex_manager.h"
#include "tb2_client_identity.h"
#include "v3_native_cache.h"
#include "taf_original_cache.h"
#include "fs_ext.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#define ORIGINAL_POLICY_MAX_BYTES (64U * 1024U)
#define ORIGINAL_WORKER_STACK_BYTES (64U * 1024U)
#define ORIGINAL_WORKER_WAIT_MS 1000U
#define ORIGINAL_TONIEPLAY_RUID_SUFFIX "0104E0"

typedef struct
{
    const char *state;
    const char *reason;
} format_status_t;

typedef struct
{
    char owner[ORIGINAL_CONTENT_CACHE_OWNER_SIZE];
    char ruid[TB2_RUID_SIZE];
    uint32_t observed_version;
    original_cache_format_t observed_format;
    uint64_t sequence;
    bool valid;
    bool running;
    original_content_cache_job_t *job;
    format_status_t formats[ORIGINAL_CACHE_FORMAT_COUNT];
} original_record_t;

static struct
{
    bool ready;
    atomic_bool stopping;
    OsEvent wake;
    OsTaskId task;
    uint64_t sequence;
    original_record_t records[ORIGINAL_CONTENT_CACHE_CAPACITY];
} original_cache;

typedef struct
{
    char upstream_ruid[TB2_RUID_SIZE];
    uint8_t auth[TONIE_AUTH_TOKEN_LENGTH];
    bool override;
    original_cache_format_t preferred;
} original_policy_t;

static bool same_text(const char *a, const char *b)
{
    return a != NULL && b != NULL && !strcmp(a, b);
}

static bool copy_text(char *destination, size_t capacity, const char *source)
{
    if (!source || strlen(source) >= capacity) return false;
    memcpy(destination, source, strlen(source) + 1U);
    return true;
}

static bool complete_identity(const settings_cert_t *identity)
{
    return identity && identity->ca && *identity->ca &&
        identity->crt && *identity->crt && identity->key && *identity->key;
}

static const settings_cert_t *tb1_identity(settings_t *settings)
{
    const settings_cert_t *identity = &settings->internal.client_tb1;
    if (!complete_identity(identity) && settings->internal.overlayNumber)
        identity = &get_settings()->internal.client_tb1;
    return complete_identity(identity) ? identity : NULL;
}

/* Called under SETTINGS. Only configured generations count, never online. */
static bool mixed_boxes(void)
{
    bool tb1 = false, tb2 = false;
    for (unsigned index = 1; index < MAX_OVERLAYS; index++)
    {
        const settings_t *settings = get_settings_id(index);
        if (!settings->internal.config_used) continue;
        tb1 |= settings->toniebox.boxGeneration == GENERATION_TB1;
        tb2 |= settings->toniebox.boxGeneration == GENERATION_TB2;
    }
    return tb1 && tb2;
}

bool original_content_cache_playback_enabled(void)
{
    mutex_lock(MUTEX_SETTINGS);
    bool enabled = get_settings()->cloud.cacheBothFormatsOnPlayback && mixed_boxes();
    mutex_unlock(MUTEX_SETTINGS);
    return enabled;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (unsigned char)tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/** Read the few authoritative fields without load_content_json(): that legacy
 * loader repairs invalid files, while policy inspection must never write. */
static const char *read_policy(settings_t *settings, const char *ruid,
                               original_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
    policy->preferred = settings && settings->toniebox.boxGeneration == GENERATION_TB2
        ? ORIGINAL_CACHE_V3 : ORIGINAL_CACHE_TAF;
    if (!settings || !settings->internal.contentdirfull ||
        tb2_ruid_classify(ruid) != TB2_RUID_CONTENT) return "not_original_audio";
    char canonical[TB2_RUID_SIZE];
    if (!tb2_ruid_canonicalize(ruid, canonical)) return "invalid_ruid";
    char *content_path = NULL;
    getContentPathFromCharRUID(canonical, &content_path, settings);
    if (!content_path) return "out_of_memory";
    size_t path_length = strlen(content_path) + sizeof(".json");
    char *path = malloc(path_length);
    if (!path) { free(content_path); return "out_of_memory"; }
    snprintf(path, path_length, "%s.json", content_path);
    free(content_path);
    uint32_t size = 0;
    if (fsGetFileSize(path, &size) != NO_ERROR || !size || size > ORIGINAL_POLICY_MAX_BYTES)
    { free(path); return "missing_content_auth"; }
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    free(path);
    if (!file) return "content_metadata_unavailable";
    char *buffer = malloc((size_t)size + 1U);
    size_t received = 0, offset = 0;
    error_t error = NO_ERROR;
    if (buffer)
    {
        while (offset < size && error == NO_ERROR)
        {
            error = fsReadFile(file, buffer + offset, size - offset, &received);
            offset += received;
            if (!received) break;
        }
        buffer[offset] = '\0';
    }
    fsCloseFile(file);
    cJSON *root = buffer && offset == size
        ? cJSON_ParseWithLengthOpts(buffer, (size_t)size + 1U, NULL, true) : NULL;
    free(buffer);
    const char *reason = "invalid_content_metadata";
    if (cJSON_IsObject(root))
    {
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
        const cJSON *nocloud = cJSON_GetObjectItemCaseSensitive(root, "nocloud");
        const cJSON *override = cJSON_GetObjectItemCaseSensitive(root, "cloud_override");
        const cJSON *live = cJSON_GetObjectItemCaseSensitive(root, "live");
        const char *auth = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "cloud_auth"));
        const char *upstream = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "cloud_ruid"));
        const char *preference = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "cache_preference"));
        if (preference && !strcmp(preference, CONTENT_JSON_CACHE_PREFERENCE_TAF))
            policy->preferred = ORIGINAL_CACHE_TAF;
        else if (preference && !strcmp(preference, CONTENT_JSON_CACHE_PREFERENCE_V3))
            policy->preferred = ORIGINAL_CACHE_V3;
        if (source && (!cJSON_IsString(source) || *source->valuestring)) reason = "private_source";
        else if (nocloud && (!cJSON_IsBool(nocloud) || cJSON_IsTrue(nocloud))) reason = "nocloud";
        else if (live && (!cJSON_IsBool(live) || cJSON_IsTrue(live))) reason = "live_content";
        else if (override && !cJSON_IsBool(override)) reason = "invalid_content_metadata";
        else if (!auth || strlen(auth) != TONIE_AUTH_TOKEN_LENGTH * 2U) reason = "missing_content_auth";
        else
        {
            policy->override = cJSON_IsTrue(override);
            reason = tb2_ruid_canonicalize(policy->override ? upstream : canonical,
                policy->upstream_ruid) ? NULL : "missing_content_auth";
            for (size_t i = 0; !reason && i < TONIE_AUTH_TOKEN_LENGTH; i++)
            {
                int high = hex_digit((unsigned char)auth[i * 2U]);
                int low = hex_digit((unsigned char)auth[i * 2U + 1U]);
                if (high < 0 || low < 0) reason = "missing_content_auth";
                else policy->auth[i] = (uint8_t)((high << 4) | low);
            }
        }
    }
    cJSON_Delete(root);
    return reason;
}

original_cache_format_t original_content_cache_preferred(settings_t *settings,
    const char *ruid)
{
    original_policy_t policy;
    mutex_lock(MUTEX_SETTINGS);
    read_policy(settings, ruid, &policy);
    mutex_unlock(MUTEX_SETTINGS);
    return policy.preferred;
}

/* SETTINGS is held by all callers of these snapshot helpers. */
static const char *format_flags(const settings_t *settings, original_cache_format_t format)
{
    if (format == ORIGINAL_CACHE_TAF)
    {
        if (!settings->cloud.cacheContent) return "taf_cache_disabled";
        if (!settings->cloud.enabled || !settings->cloud.enableV2Content) return "boxine_forwarding_disabled";
    }
    else if (format == ORIGINAL_CACHE_V3)
    {
        if (!settings->cloud.cacheContentV3) return "v3_cache_disabled";
        if (settings->cloud.tb2_enabled || !settings->cloud.tb2_v3_enabled ||
            !settings->cloud.enableV3ContentMeta || !settings->cloud.enableV3Chapter)
            return "tonies_forwarding_disabled";
    }
    else return "invalid_format";
    return NULL;
}

static const char *format_policy(settings_t *settings, original_cache_format_t format)
{
    const char *reason = format_flags(settings, format);
    if (reason) return reason;
    const settings_cert_t *identity = format == ORIGINAL_CACHE_TAF
        ? tb1_identity(settings) : tb2_client_identity_resolve(settings, NULL);
    return complete_identity(identity) ? NULL : format == ORIGINAL_CACHE_TAF
        ? "missing_tb1_certificates" : "missing_tb2_certificates";
}

static bool complete_local_cache(settings_t *settings, const char *ruid,
                                  original_cache_format_t format)
{
    if (format == ORIGINAL_CACHE_V3)
        return settings->cloud.cacheContentV3 && v3_native_cache_active_info(
            settings->internal.cachedirfull, settings->internal.librarydirfull,
            settings->internal.overlayNumber, ruid, NULL, NULL, NULL);
    if (format != ORIGINAL_CACHE_TAF || !settings->cloud.cacheContent) return false;
    char canonical[TB2_RUID_SIZE], *path = NULL;
    if (!tb2_ruid_canonicalize(ruid, canonical)) return false;
    getContentPathFromCharRUID(canonical, &path, settings);
    if (!path) return false;
    bool complete = taf_original_file_complete(path);
    char *reference = complete ? NULL : taf_original_cache_resolve(path, settings);
    complete |= reference != NULL;
    free(reference); free(path);
    return complete;
}

const char *original_content_cache_blocked_reason(settings_t *settings,
    const char *ruid, original_cache_format_t format)
{
    if (!settings) return "invalid_overlay";
    mutex_lock(MUTEX_SETTINGS);
    original_policy_t policy;
    const char *reason = read_policy(settings, ruid, &policy);
    if (!reason)
    {
        reason = format_policy(settings, format);
        if (reason && complete_local_cache(settings, ruid, format)) reason = NULL;
    }
    mutex_unlock(MUTEX_SETTINGS);
    return reason;
}

static void free_identity(settings_cert_t *identity)
{
    free(identity->ca);
    free(identity->crt);
    if (identity->key) { memset(identity->key, 0, strlen(identity->key)); free(identity->key); }
}

static bool copy_identity(settings_cert_t *target, const settings_cert_t *source)
{
    if (!complete_identity(source)) return true;
    target->ca = strdup(source->ca);
    target->crt = strdup(source->crt);
    target->key = strdup(source->key);
    return target->ca && target->crt && target->key;
}

static void free_job(original_content_cache_job_t *job)
{
    if (!job) return;
    free(job->settings.cloud.remote_hostname);
    free(job->settings.cloud.remote_hostname_tb2);
    free(job->settings.internal.contentdirfull);
    free(job->settings.internal.librarydirfull);
    free(job->settings.internal.cachedirfull);
    free_identity(&job->tb1_identity);
    free_identity(&job->tb2_identity);
    memset(job->auth, 0, sizeof(job->auth));
    free(job);
}

static original_content_cache_job_t *copy_job(settings_t *source, const char *ruid,
    const original_policy_t *policy)
{
    original_content_cache_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    settings_t *target = &job->settings;
    target->cloud = source->cloud;
    target->cloud.remote_hostname = strdup(source->cloud.remote_hostname ? source->cloud.remote_hostname : "");
    target->cloud.remote_hostname_tb2 = strdup(source->cloud.remote_hostname_tb2 ? source->cloud.remote_hostname_tb2 : "");
    target->cloud.tb2_capture_dir = NULL;
    target->core.http_client_timeout = source->core.http_client_timeout;
    target->core.full_taf_validation = source->core.full_taf_validation;
    target->core.track_pos_taf_validation = source->core.track_pos_taf_validation;
    target->toniebox = source->toniebox;
    target->toniebox2 = source->toniebox2;
    target->internal.overlayNumber = source->internal.overlayNumber;
    target->internal.config_used = source->internal.config_used;
    target->internal.contentdirfull = source->internal.contentdirfull ? strdup(source->internal.contentdirfull) : NULL;
    target->internal.librarydirfull = source->internal.librarydirfull ? strdup(source->internal.librarydirfull) : NULL;
    target->internal.cachedirfull = source->internal.cachedirfull ? strdup(source->internal.cachedirfull) : NULL;
    bool copied = copy_identity(&job->tb1_identity, tb1_identity(source)) &&
        copy_identity(&job->tb2_identity, tb2_client_identity_resolve(source, NULL));
    /* The adapter passes owned identity pointers, never live resolver results. */
    target->internal.client_tb1 = job->tb1_identity;
    target->internal.client_tb2 = job->tb2_identity;
    target->internal.overlayUniqueId = job->owner;
    if (!copied || !target->cloud.remote_hostname || !target->cloud.remote_hostname_tb2 ||
        !target->internal.contentdirfull || !target->internal.librarydirfull ||
        !target->internal.cachedirfull ||
        !copy_text(job->owner, sizeof(job->owner), source->internal.overlayUniqueId) ||
        !tb2_ruid_canonicalize(ruid, job->ruid))
    { free_job(job); return NULL; }
    memcpy(job->auth, policy->auth, sizeof(job->auth));
    memcpy(job->upstream_ruid, policy->upstream_ruid, sizeof(job->upstream_ruid));
    return job;
}

static bool check_current_job(const original_content_cache_job_t *job,
    original_cache_format_t format, bool network)
{
    if (!job || atomic_load(&original_cache.stopping) ||
        job->settings.internal.overlayNumber >= MAX_OVERLAYS ||
        format >= ORIGINAL_CACHE_FORMAT_COUNT) return false;
    mutex_lock(MUTEX_SETTINGS);
    settings_t *live = get_settings_id(job->settings.internal.overlayNumber);
    original_policy_t policy;
    const settings_t *snapshot = &job->settings;
    bool allowed = live->internal.config_used &&
        same_text(live->internal.overlayUniqueId, job->owner) &&
        same_text(live->internal.contentdirfull, snapshot->internal.contentdirfull) &&
        same_text(live->internal.librarydirfull, snapshot->internal.librarydirfull) &&
        same_text(live->internal.cachedirfull, snapshot->internal.cachedirfull) &&
        !read_policy(live, job->ruid, &policy) &&
        same_text(policy.upstream_ruid, job->upstream_ruid) &&
        !memcmp(policy.auth, job->auth, sizeof(job->auth)) &&
        (format == ORIGINAL_CACHE_TAF
            ? live->cloud.cacheContent && live->cloud.cacheToLibrary == snapshot->cloud.cacheToLibrary
            : live->cloud.cacheContentV3 && live->cloud.cacheToLibraryV3 == snapshot->cloud.cacheToLibraryV3 &&
              live->cloud.cacheTonieplayToLibraryV3 == snapshot->cloud.cacheTonieplayToLibraryV3);
    if (allowed && (job->automatic || format != job->preferred))
        allowed = mixed_boxes() && (job->automatic
            ? get_settings()->cloud.cacheBothFormatsOnPlayback
            : get_settings()->cloud.cacheBothFormatsOnDownload);
    if (allowed && network)
    {
        const settings_cert_t *identity = format == ORIGINAL_CACHE_TAF
            ? tb1_identity(live) : tb2_client_identity_resolve(live, NULL);
        const settings_cert_t *owned = format == ORIGINAL_CACHE_TAF
            ? &job->tb1_identity : &job->tb2_identity;
        allowed = !format_policy(live, format) && identity && same_text(identity->ca, owned->ca) &&
            same_text(identity->crt, owned->crt) && same_text(identity->key, owned->key) &&
            (format == ORIGINAL_CACHE_TAF
                ? same_text(live->cloud.remote_hostname, snapshot->cloud.remote_hostname) &&
                  live->cloud.remote_port == snapshot->cloud.remote_port &&
                  live->cloud.cacheToLibrary == snapshot->cloud.cacheToLibrary
                : same_text(live->cloud.remote_hostname_tb2, snapshot->cloud.remote_hostname_tb2) &&
                  live->cloud.remote_port_tb2 == snapshot->cloud.remote_port_tb2 &&
                  live->cloud.cacheToLibraryV3 == snapshot->cloud.cacheToLibraryV3 &&
                  live->cloud.cacheTonieplayToLibraryV3 == snapshot->cloud.cacheTonieplayToLibraryV3);
    }
    mutex_unlock(MUTEX_SETTINGS);
    return allowed;
}

bool original_content_cache_job_allowed(const original_content_cache_job_t *job,
    original_cache_format_t format)
{
    return check_current_job(job, format, true);
}

bool original_content_cache_job_current(const original_content_cache_job_t *job,
    original_cache_format_t format)
{
    return check_current_job(job, format, false);
}

const char *original_content_cache_job_blocked_reason(
    const original_content_cache_job_t *job, original_cache_format_t format)
{
    if (!job) return "invalid_job";
    const char *reason = format_flags(&job->settings, format);
    if (reason) return reason;
    const settings_cert_t *identity = format == ORIGINAL_CACHE_TAF
        ? &job->tb1_identity : &job->tb2_identity;
    if (!complete_identity(identity)) return format == ORIGINAL_CACHE_TAF
        ? "missing_tb1_certificates" : "missing_tb2_certificates";
    return original_content_cache_job_allowed(job, format) ? NULL : "policy_changed_or_disabled";
}

static void set_status(original_record_t *record, original_cache_format_t format,
                       const char *state, const char *reason)
{
    mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
    record->formats[format] = (format_status_t){state, reason};
    mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
}

static void process_record(original_record_t *record)
{
    original_content_cache_job_t *job = record->job;
    for (unsigned phase = 0; phase < (job->both_formats ? 2U : 1U); phase++)
    {
        original_cache_format_t format = phase == 0 ? job->preferred :
            (job->preferred == ORIGINAL_CACHE_TAF ? ORIGINAL_CACHE_V3 : ORIGINAL_CACHE_TAF);
        if (format == ORIGINAL_CACHE_TAF &&
            (!strcmp(job->upstream_ruid + TB2_RUID_HEX_LENGTH -
                (sizeof(ORIGINAL_TONIEPLAY_RUID_SUFFIX) - 1U), ORIGINAL_TONIEPLAY_RUID_SUFFIX) ||
             v3_native_cache_active_is_tonieplay(
            job->settings.internal.cachedirfull, job->settings.internal.librarydirfull,
            job->settings.internal.overlayNumber, job->ruid)))
        { set_status(record, format, "skipped", "tonieplay_has_no_taf"); continue; }
        if (!original_content_cache_job_current(job, format))
        { set_status(record, format, "skipped", "policy_changed_or_disabled"); continue; }
        set_status(record, format, "running", NULL);
        const char *reason = NULL;
        bool library = format == ORIGINAL_CACHE_TAF
            ? job->settings.cloud.cacheToLibrary : job->settings.cloud.cacheToLibraryV3;
        error_t error = original_content_cache_materialize(job, format, library, &reason);
        if (!reason && error == ERROR_ACCESS_DENIED)
            reason = original_content_cache_job_blocked_reason(job, format);
        set_status(record, format, error == NO_ERROR ? "complete" :
            error == ERROR_ACCESS_DENIED || error == ERROR_ABORTED ? "skipped" : "failed",
            reason ? reason : error == NO_ERROR ? NULL : "download_failed");
        TRACE_INFO("Original content cache overlay=%u rUID=%s format=%s result=%d reason=%s\r\n",
            (unsigned)job->settings.internal.overlayNumber, job->ruid,
            format == ORIGINAL_CACHE_TAF ? "taf" : "v3", error, reason ? reason : "none");
    }
}

static void original_worker(void *unused)
{
    (void)unused;
    while (!atomic_load(&original_cache.stopping))
    {
        mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
        original_record_t *selected = NULL;
        for (size_t i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
        {
            original_record_t *record = &original_cache.records[i];
            if (record->job && !record->running && (!selected || record->sequence < selected->sequence))
                selected = record;
        }
        if (selected) selected->running = true;
        mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
        if (!selected) { osWaitForEvent(&original_cache.wake, ORIGINAL_WORKER_WAIT_MS); continue; }
        process_record(selected);
        mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
        free_job(selected->job);
        selected->job = NULL;
        selected->running = false;
        mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
    }
}

error_t original_content_cache_submit(settings_t *settings, const char *ruid,
    original_cache_format_t preferred, bool automatic, uint32_t observed_version,
    const char **reason)
{
    if (reason) *reason = NULL;
    if (!original_cache.ready || atomic_load(&original_cache.stopping))
    { if (reason) *reason = "worker_unavailable"; return ERROR_OUT_OF_RESOURCES; }
    if (!settings || preferred >= ORIGINAL_CACHE_FORMAT_COUNT)
        return ERROR_INVALID_PARAMETER;
    mutex_lock(MUTEX_SETTINGS);
    bool mixed = mixed_boxes();
    bool both = mixed && (automatic ? get_settings()->cloud.cacheBothFormatsOnPlayback :
        get_settings()->cloud.cacheBothFormatsOnDownload);
    if (automatic && !both)
    {
        mutex_unlock(MUTEX_SETTINGS);
        if (reason) *reason = mixed ? "automatic_cache_disabled" : "mixed_generations_required";
        return ERROR_ABORTED;
    }
    original_policy_t policy;
    const char *blocked = read_policy(settings, ruid, &policy);
    if (!blocked && !automatic && !both)
    {
        blocked = format_policy(settings, preferred);
        if (blocked && complete_local_cache(settings, ruid, preferred)) blocked = NULL;
    }
    original_content_cache_job_t *job = blocked ? NULL : copy_job(settings, ruid, &policy);
    mutex_unlock(MUTEX_SETTINGS);
    if (!job)
    {
        if (reason) *reason = blocked ? blocked : "out_of_memory";
        return blocked ? ERROR_ACCESS_DENIED : ERROR_OUT_OF_MEMORY;
    }
    job->automatic = automatic;
    job->both_formats = both;
    job->preferred = preferred;
    job->observed_version = observed_version;
    mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
    if (atomic_load(&original_cache.stopping))
    {
        mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE); free_job(job);
        if (reason) *reason = "worker_unavailable";
        return ERROR_OUT_OF_RESOURCES;
    }
    original_record_t *slot = NULL;
    for (size_t i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
    {
        original_record_t *record = &original_cache.records[i];
        if (!record->valid || strcmp(record->owner, job->owner) || strcmp(record->ruid, job->ruid)) continue;
        if (record->job || (automatic && record->observed_version == observed_version &&
            record->observed_format == preferred))
        {
            bool pending = record->job != NULL;
            mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
            free_job(job);
            if (reason) *reason = pending ? "already_queued" : "already_attempted";
            return NO_ERROR;
        }
        slot = record;
        break;
    }
    if (!slot)
    {
        for (size_t i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
        {
            original_record_t *record = &original_cache.records[i];
            if (record->job) continue;
            if (!record->valid) { slot = record; break; }
            if (!slot || record->sequence < slot->sequence) slot = record;
        }
    }
    if (!slot)
    {
        mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE); free_job(job);
        if (reason) *reason = "queue_full";
        return ERROR_OUT_OF_RESOURCES;
    }
    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->job = job;
    slot->sequence = ++original_cache.sequence;
    slot->observed_version = observed_version;
    slot->observed_format = preferred;
    memcpy(slot->owner, job->owner, sizeof(slot->owner));
    memcpy(slot->ruid, job->ruid, sizeof(slot->ruid));
    for (unsigned format = 0; format < ORIGINAL_CACHE_FORMAT_COUNT; format++)
        slot->formats[format] = both || format == (unsigned)preferred
            ? (format_status_t){"queued", NULL} : (format_status_t){"skipped", "not_requested"};
    mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
    osSetEvent(&original_cache.wake);
    return NO_ERROR;
}

void original_content_cache_add_status(cJSON *cache_state, const settings_t *settings,
                                       const char *ruid)
{
    char canonical[TB2_RUID_SIZE];
    if (!original_cache.ready || !settings || !settings->internal.overlayUniqueId ||
        !tb2_ruid_canonicalize(ruid, canonical)) return;
    format_status_t formats[ORIGINAL_CACHE_FORMAT_COUNT];
    bool found = false;
    mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
    for (size_t i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
    {
        const original_record_t *record = &original_cache.records[i];
        if (record->valid && same_text(record->owner, settings->internal.overlayUniqueId) &&
            !strcmp(record->ruid, canonical))
        { memcpy(formats, record->formats, sizeof(formats)); found = true; break; }
    }
    mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
    if (!found) return;
    cJSON *download = cJSON_AddObjectToObject(cache_state, "download");
    if (!download) return;
    for (unsigned format = 0; format < ORIGINAL_CACHE_FORMAT_COUNT; format++)
    {
        cJSON *item = cJSON_AddObjectToObject(download, format == ORIGINAL_CACHE_TAF ? "taf" : "v3");
        if (!item) continue;
        cJSON_AddStringToObject(item, "state", formats[format].state);
        if (formats[format].reason) cJSON_AddStringToObject(item, "reason", formats[format].reason);
    }
}

void original_content_cache_init(void)
{
    if (original_cache.ready) return;
    if (!osCreateEvent(&original_cache.wake)) return;
    atomic_store(&original_cache.stopping, false);
    OsTaskParameters parameters = OS_TASK_DEFAULT_PARAMS;
    parameters.stackSize = ORIGINAL_WORKER_STACK_BYTES;
    original_cache.task = osCreateTask("original-cache", original_worker, NULL, &parameters);
    if (original_cache.task == (OsTaskId)OS_INVALID_TASK_ID)
    { osDeleteEvent(&original_cache.wake); TRACE_ERROR("Original content cache worker unavailable\r\n"); return; }
    original_cache.ready = true;
}

void original_content_cache_deinit(void)
{
    if (!original_cache.ready) return;
    atomic_store(&original_cache.stopping, true);
    osSetEvent(&original_cache.wake);
#ifdef _WIN32
    WaitForSingleObject((HANDLE)original_cache.task, INFINITE);
    CloseHandle((HANDLE)original_cache.task);
#else
    pthread_join((pthread_t)original_cache.task, NULL);
#endif
    mutex_lock(MUTEX_ORIGINAL_CONTENT_CACHE);
    for (size_t i = 0; i < ORIGINAL_CONTENT_CACHE_CAPACITY; i++)
    {
        free_job(original_cache.records[i].job);
        original_cache.records[i].job = NULL;
    }
    mutex_unlock(MUTEX_ORIGINAL_CONTENT_CACHE);
    /* Like HTTP state, keep the event valid while handlers finish shutdown. */
}
