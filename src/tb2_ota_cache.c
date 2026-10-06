/* Optional TB2 counterpart cache. Slots come from the manifest, never filenames. */
#include "tb2_ota_cache.h"
#include "tb2_client_identity.h"
#include "mutex_manager.h"
#include "server_helpers.h"
#include "fs_ext.h"
#include "cJSON.h"
#include "hash/sha256.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#define OTA_MANIFEST_LIMIT (16U * 1024U)
#define OTA_URI_SIZE 512U
#define OTA_QUERY_SIZE 1024U
#define OTA_ID_SIZE 128U
#define OTA_VERSION_SIZE 64U
#define OTA_IO_SIZE 4096U
#define OTA_WAIT_MS 20U
#define OTA_ACTIVE_MAX (HTTP_SERVER_MAX_CONNECTIONS + 1U)
#define OTA_RESOURCE_LOCKS 16U

typedef struct
{
    char uri[OTA_URI_SIZE], query[OTA_QUERY_SIZE], id[OTA_ID_SIZE];
    char version[OTA_VERSION_SIZE];
    unsigned type;
    uint8_t digest[SHA256_DIGEST_SIZE];
} ota_image_t;

typedef struct
{
    char owner[OTA_ID_SIZE];
    uint64_t revision;
    bool valid;
    ota_image_t images[2];
} ota_offer_t;

typedef struct
{
    ota_image_t image;
    settings_t settings;
    client_ctx_t client;
    settings_cert_t identity;
    char *agent;
    char owner[OTA_ID_SIZE];
    unsigned overlay;
    char path[PATH_LEN];
} ota_job_t;

typedef struct
{
    char path[PATH_LEN];
    ota_image_t image;
    unsigned users;
    bool done;
    error_t result;
} ota_active_t;

static struct
{
    OsMutex mutex;
    OsMutex resources[OTA_RESOURCE_LOCKS];
    OsEvent wake;
    OsTaskId task;
    bool ready, enabled, stopping, working;
    uint64_t epoch;
    unsigned handlers;
    ota_offer_t offers[MAX_OVERLAYS];
    ota_job_t *pending[MAX_OVERLAYS];
    ota_active_t active[OTA_ACTIVE_MAX];
} ota;

static bool ota_copy(char *dst, size_t size, const char *src)
{
    if (src == NULL || strlen(src) >= size) return false;
    memcpy(dst, src, strlen(src) + 1);
    return true;
}

static bool ota_policy(const settings_t *settings)
{
    return settings != NULL && settings->toniebox.boxGeneration == GENERATION_TB2 &&
           settings->cloud.tb2_v3_enabled && settings->cloud.enableV3Ota &&
           settings->cloud.cacheOta;
}

/** Compare the opaque resource key, not an assumed digest or A/B suffix. */
static bool ota_resource(const char *uri, ota_image_t *image)
{
    const char *prefix = "/v3/ota/";
    if (uri == NULL || strncmp(uri, prefix, strlen(prefix))) return false;
    const char *p = uri + strlen(prefix);
    unsigned type = 0;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p))
    {
        type = type * 10U + (unsigned)(*p++ - '0');
        if (type > UINT8_MAX) return false;
    }
    if (*p++ != '/' || !*p || !ota_copy(image->id, sizeof(image->id), p)) return false;
    for (const char *c = p; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') return false;
    image->type = type;
    return ota_copy(image->uri, sizeof(image->uri), uri);
}

static bool ota_same_resource(const ota_image_t *a, const ota_image_t *b)
{
    return a->type == b->type && !strcmp(a->id, b->id);
}

static bool ota_url(const char *url, const char *host, unsigned port, ota_image_t *image)
{
    if (url == NULL || host == NULL || strchr(url, '#')) return false;
    for (const char *c = url; *c; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c == 127) return false;
    const char *path = url;
    if (!strncmp(url, "https://", 8))
    {
        const char *slash = strchr(url + 8, '/');
        char authority[OTA_URI_SIZE], expected[OTA_URI_SIZE];
        if (!slash || (size_t)(slash - url - 8) >= sizeof(authority)) return false;
        memcpy(authority, url + 8, (size_t)(slash - url - 8));
        authority[slash - url - 8] = 0;
        int n = snprintf(expected, sizeof(expected), "%s:%u", host, port);
        if (n < 0 || (size_t)n >= sizeof(expected)) return false;
        if (osStrcasecmp(authority, expected) &&
            (port != HTTPS_PORT || osStrcasecmp(authority, host))) return false;
        path = slash;
    }
    else if (*url != '/') return false;
    const char *query = strchr(path, '?');
    size_t length = query ? (size_t)(query - path) : strlen(path);
    char uri[OTA_URI_SIZE];
    if (length >= sizeof(uri)) return false;
    memcpy(uri, path, length); uri[length] = 0;
    return ota_resource(uri, image) &&
           ota_copy(image->query, sizeof(image->query), query ? query + 1 : "");
}

static bool ota_unique(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    return true;
}

static int ota_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ota_image_parse(const cJSON *object, const char *host, unsigned port,
                            ota_image_t *image)
{
    if (!ota_unique(object)) return false;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "url"));
    const char *hash = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "sha2_256"));
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "version_string"));
    if (!hash || strlen(hash) != SHA256_DIGEST_SIZE * 2U || !version || !*version ||
        !ota_copy(image->version, sizeof(image->version), version) ||
        !ota_url(url, host, port, image)) return false;
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        int high = ota_hex(hash[i * 2]), low = ota_hex(hash[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        image->digest[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool ota_parse(const char *body, size_t length, const char *host,
                      unsigned port, ota_offer_t *offer)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, length + 1, &end, true);
    bool valid = end == body + length && ota_unique(root) &&
        ota_image_parse(cJSON_GetObjectItemCaseSensitive(root, "FirmwareSlot0"),
                        host, port, &offer->images[0]) &&
        ota_image_parse(cJSON_GetObjectItemCaseSensitive(root, "FirmwareSlot1"),
                        host, port, &offer->images[1]) &&
        !ota_same_resource(&offer->images[0], &offer->images[1]);
    cJSON_Delete(root);
    return valid;
}

static void ota_job_free(ota_job_t *job)
{
    if (!job) return;
    free(job->settings.cloud.remote_hostname_tb2);
    free(job->settings.internal.firmwaredirfull);
    free(job->identity.ca); free(job->identity.crt);
    if (job->identity.key)
    {
        memset(job->identity.key, 0, strlen(job->identity.key));
        free(job->identity.key);
    }
    free(job->agent); free(job);
}

/** Called with MUTEX_SETTINGS held. Resolve identity before copying; never use
 * live settings/certificate pointers from the worker's HTTP/TLS callbacks. */
static ota_job_t *ota_job_copy(settings_t *settings, const char *agent,
                               const ota_image_t *image)
{
    const settings_cert_t *identity = tb2_client_identity_resolve(settings, NULL);
    if (!identity || !identity->ca || !identity->crt || !identity->key ||
        !settings->cloud.remote_hostname_tb2 || !settings->internal.firmwaredirfull) return NULL;
    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->image = *image;
    job->overlay = settings->internal.overlayNumber;
    job->settings.cloud.tb2_v3_enabled = true;
    job->settings.cloud.remote_port_tb2 = settings->cloud.remote_port_tb2;
    job->settings.core.http_client_timeout = settings->core.http_client_timeout;
    job->client.settings = &job->settings;
    job->settings.cloud.remote_hostname_tb2 = strdup(settings->cloud.remote_hostname_tb2);
    job->settings.internal.firmwaredirfull = strdup(settings->internal.firmwaredirfull);
    job->identity.ca = strdup(identity->ca);
    job->identity.crt = strdup(identity->crt);
    job->identity.key = strdup(identity->key);
    job->agent = strdup(agent ? agent : "");
    int n = snprintf(job->path, sizeof(job->path), "%s%cota%ctb2%c%u%c%s.bin",
        settings->internal.firmwaredirfull, PATH_SEPARATOR, PATH_SEPARATOR,
        PATH_SEPARATOR, image->type, PATH_SEPARATOR, image->id);
    if (!job->settings.cloud.remote_hostname_tb2 || !job->settings.internal.firmwaredirfull ||
        !job->identity.ca || !job->identity.crt || !job->identity.key || !job->agent ||
        n < 0 || (size_t)n >= sizeof(job->path) ||
        !ota_copy(job->owner, sizeof(job->owner), settings->internal.overlayUniqueId))
    {
        ota_job_free(job); return NULL;
    }
    return job;
}

static bool ota_verified_file(const char *path, const uint8_t *digest_expected)
{
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (!file) return false;
    Sha256Context hash; sha256Init(&hash);
    uint8_t buffer[OTA_IO_SIZE], digest[SHA256_DIGEST_SIZE];
    size_t received = 0, total = 0;
    error_t error;
    do
    {
        error = fsReadFile(file, buffer, sizeof(buffer), &received);
        if (received) { sha256Update(&hash, buffer, received); total += received; }
    } while (!error && received);
    fsCloseFile(file);
    sha256Final(&hash, digest);
    return (!error || error == ERROR_END_OF_FILE) && total > 0 &&
           !memcmp(digest, digest_expected, sizeof(digest));
}

/** Bounded path locks also cover the legacy writer before an offer is known.
 * Hash collisions only serialize unrelated resources; they cannot mix files. */
static OsMutex *ota_resource_lock(const char *path)
{
    uint32_t hash = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        hash = hash * 31U + *p;
    return &ota.resources[hash % OTA_RESOURCE_LOCKS];
}

typedef struct
{
    cbr_ctx_t base; /* cloud_request requires the common context at offset zero */
    FsFile *file;
    Sha256Context hash;
    uint64_t bytes;
    error_t error;
    bool eof;
} ota_transfer_t;

static void ota_transfer_response(void *context, HttpClientContext *http)
{
    ota_transfer_t *transfer = context;
    if (http->statusCode != 200) transfer->error = ERROR_INVALID_RESPONSE;
}

static void ota_transfer_body(void *context, HttpClientContext *http,
                               const char *data, size_t length, error_t error)
{
    (void)http;
    ota_transfer_t *transfer = context;
    if (transfer->error) return;
    if (error && error != ERROR_END_OF_STREAM) { transfer->error = error; return; }
    if (length)
    {
        transfer->error = fsWriteFile(transfer->file, (void *)data, length);
        if (transfer->error) return;
        sha256Update(&transfer->hash, data, length);
        transfer->bytes += length;
    }
    if (error == ERROR_END_OF_STREAM) transfer->eof = true;
}

static error_t ota_fetch_locked(ota_job_t *job)
{
    if (ota_verified_file(job->path, job->image.digest)) return NO_ERROR;
    char directory[PATH_LEN], temporary[PATH_LEN];
    if (!ota_copy(directory, sizeof(directory), job->path)) return ERROR_INVALID_PARAMETER;
    char *separator = strrchr(directory, PATH_SEPARATOR);
    if (!separator) return ERROR_INVALID_PARAMETER;
    *separator = 0;
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return ERROR_INVALID_PARAMETER;
    error_t error = fsCreateDirEx(directory, true);
    if (error) return error;
    ota_transfer_t transfer = {0};
    transfer.file = fsOpenFile(temporary, FS_FILE_MODE_WRITE);
    if (!transfer.file) return ERROR_OPEN_FAILED;
    sha256Init(&transfer.hash);
    transfer.base.client_ctx = &job->client;
    transfer.base.user_agent = job->agent;
    transfer.base.tb2_identity = &job->identity;
    transfer.base.reject_redirects = true;
    req_cbr_t callbacks = {.ctx = &transfer, .response = ota_transfer_response,
                           .body = ota_transfer_body};
    error = cloud_request_tb2_get(job->settings.cloud.remote_hostname_tb2,
        job->settings.cloud.remote_port_tb2, job->image.uri, job->image.query, NULL, &callbacks);
    if (!error) error = transfer.error;
    if (!error && (!transfer.eof || !transfer.bytes)) error = ERROR_INVALID_RESPONSE;
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256Final(&transfer.hash, digest);
    if (!error && memcmp(digest, job->image.digest, sizeof(digest))) error = ERROR_WRONG_CHECKSUM;
    if (!error) error = fsFlushFile(transfer.file);
    fsCloseFile(transfer.file);
    /* The resource lock excludes readers/writers while replacing an invalid
     * legacy entry. Never remove a valid cached image before validation. */
    if (!error && fsFileExists(job->path)) error = fsDeleteFile(job->path);
    if (!error) error = fsRenameFile(temporary, job->path);
    if (error) fsDeleteFile(temporary);
    return error;
}

static error_t ota_fetch(ota_job_t *job)
{
    OsMutex *lock = ota_resource_lock(job->path);
    osAcquireMutex(lock);
    error_t error = ota_fetch_locked(job);
    osReleaseMutex(lock);
    if (error)
    {
        TRACE_WARNING("TB2 OTA cache type=%u resource=%s failed: %s\r\n",
                      job->image.type, job->image.id, error2text(error));
    }
    return error;
}

/** Join a write by resource path. Waiters hold a reference through failure too,
 * so a failed first attempt does not cause an immediate retry stampede. */
static error_t ota_download(ota_job_t *job)
{
    osAcquireMutex(&ota.mutex);
    ota_active_t *entry = NULL, *vacant = NULL;
    for (size_t i = 0; i < OTA_ACTIVE_MAX; i++)
    {
        ota_active_t *candidate = &ota.active[i];
        if (!candidate->users) vacant = candidate;
        else if (!strcmp(candidate->path, job->path)) entry = candidate;
    }
    bool owner = entry == NULL;
    if (owner && vacant)
    {
        entry = vacant; memset(entry, 0, sizeof(*entry));
        strcpy(entry->path, job->path); entry->image = job->image;
    }
    if (!entry || memcmp(entry->image.digest, job->image.digest, SHA256_DIGEST_SIZE))
    {
        osReleaseMutex(&ota.mutex); return ERROR_OUT_OF_RESOURCES;
    }
    entry->users++;
    osReleaseMutex(&ota.mutex);
    error_t result;
    if (owner)
    {
        result = ota_fetch(job);
        osAcquireMutex(&ota.mutex);
        entry->result = result; entry->done = true;
    }
    else
    {
        for (;;)
        {
            osAcquireMutex(&ota.mutex);
            if (entry->done) break;
            osReleaseMutex(&ota.mutex); osDelayTask(OTA_WAIT_MS);
        }
        result = entry->result;
    }
    entry->users--;
    osReleaseMutex(&ota.mutex);
    return result;
}

static bool ota_job_allowed(const ota_job_t *job)
{
    mutex_lock(MUTEX_SETTINGS);
    settings_t *settings = get_settings_id((uint8_t)job->overlay);
    bool allowed = get_settings()->cloud.cacheOtaV3BothSlots && ota_policy(settings) &&
        settings->internal.overlayUniqueId && !strcmp(settings->internal.overlayUniqueId, job->owner);
    mutex_unlock(MUTEX_SETTINGS);
    return allowed;
}

static void ota_worker(void *unused)
{
    (void)unused;
    for (;;)
    {
        osAcquireMutex(&ota.mutex);
        bool stop = ota.stopping;
        ota_job_t *job = NULL;
        for (size_t i = 1; !stop && i < MAX_OVERLAYS; i++)
            if (ota.pending[i]) { job = ota.pending[i]; ota.pending[i] = NULL; break; }
        ota.working = job != NULL;
        osReleaseMutex(&ota.mutex);
        if (stop) break;
        if (job)
        {
            if (ota_job_allowed(job)) ota_download(job);
            ota_job_free(job);
            osAcquireMutex(&ota.mutex); ota.working = false; osReleaseMutex(&ota.mutex);
        }
        else osWaitForEvent(&ota.wake, 1000);
    }
}

void tb2_ota_cache_init(void)
{
    if (ota.ready) return;
    memset(&ota, 0, sizeof(ota));
    if (!osCreateMutex(&ota.mutex)) return;
    if (!osCreateEvent(&ota.wake)) { osDeleteMutex(&ota.mutex); return; }
    for (size_t i = 0; i < OTA_RESOURCE_LOCKS; i++)
    {
        if (!osCreateMutex(&ota.resources[i]))
        {
            while (i) osDeleteMutex(&ota.resources[--i]);
            osDeleteEvent(&ota.wake); osDeleteMutex(&ota.mutex); return;
        }
    }
    ota.enabled = get_settings()->cloud.cacheOtaV3BothSlots;
    OsTaskParameters parameters = OS_TASK_DEFAULT_PARAMS;
    parameters.stackSize = 64U * 1024U;
    ota.task = osCreateTask("tb2-ota-cache", ota_worker, NULL, &parameters);
    if (ota.task == (OsTaskId)OS_INVALID_TASK_ID)
    {
        TRACE_ERROR("TB2 OTA counterpart worker could not start\r\n");
        for (size_t i = 0; i < OTA_RESOURCE_LOCKS; i++) osDeleteMutex(&ota.resources[i]);
        osDeleteEvent(&ota.wake); osDeleteMutex(&ota.mutex); return;
    }
    ota.ready = true;
}

void tb2_ota_cache_set_enabled(bool enabled)
{
    if (!ota.ready) return;
    osAcquireMutex(&ota.mutex);
    if (ota.enabled == enabled) { osReleaseMutex(&ota.mutex); return; }
    ota.enabled = enabled; ota.epoch++;
    for (size_t i = 0; i < MAX_OVERLAYS; i++)
    {
        ota.offers[i].valid = false;
        ota_job_free(ota.pending[i]); ota.pending[i] = NULL;
    }
    osReleaseMutex(&ota.mutex);
    osSetEvent(&ota.wake);
}

void tb2_ota_cache_deinit(void)
{
    if (!ota.ready) return;
    tb2_ota_cache_set_enabled(false);
    osAcquireMutex(&ota.mutex); ota.stopping = true; osReleaseMutex(&ota.mutex);
    osSetEvent(&ota.wake);
#ifdef _WIN32
    WaitForSingleObject((HANDLE)ota.task, INFINITE);
    CloseHandle((HANDLE)ota.task);
#else
    pthread_join((pthread_t)ota.task, NULL);
#endif
    for (;;)
    {
        osAcquireMutex(&ota.mutex); unsigned handlers = ota.handlers; osReleaseMutex(&ota.mutex);
        if (!handlers) break;
        osDelayTask(OTA_WAIT_MS);
    }
    /* HTTP connection tasks can still unwind during server shutdown. Keep the
     * synchronization objects alive until process exit, like other HTTP state. */
}

typedef struct
{
    cbr_ctx_t base;
    req_cbr_t forward;
    char *body;
    size_t length;
    bool failed, eof;
} ota_observer_t;

static void ota_observe_body(void *context, HttpClientContext *http,
                             const char *payload, size_t length, error_t error)
{
    ota_observer_t *observer = context;
    observer->forward.body(&observer->base, http, payload, length, error);
    if (http->statusCode != 200 || (error && error != ERROR_END_OF_STREAM) ||
        length > OTA_MANIFEST_LIMIT - observer->length) observer->failed = true;
    if (!observer->failed && length)
    {
        memcpy(observer->body + observer->length, payload, length);
        observer->length += length;
    }
    if (error == ERROR_END_OF_STREAM) observer->eof = true;
}

error_t tb2_ota_cache_check(HttpConnection *connection, const char *uri,
                            const char *query, client_ctx_t *client,
                            const uint8_t *body, size_t length)
{
    ota_observer_t observer = {0};
    observer.forward = getCloudCbr(connection, uri, query, V3_CHECK_OTA, &observer.base, client);
    req_cbr_t callbacks = observer.forward;
    unsigned id = client->settings->internal.overlayNumber;
    ota_offer_t offer = {0};
    uint64_t epoch = 0;
    char host[OTA_URI_SIZE]; unsigned port = 0;
    if (ota.ready && id > 0 && id < MAX_OVERLAYS)
    {
        mutex_lock(MUTEX_SETTINGS);
        bool allowed = ota_policy(client->settings) &&
            get_settings()->cloud.cacheOtaV3BothSlots &&
            ota_copy(offer.owner, sizeof(offer.owner), client->settings->internal.overlayUniqueId) &&
            ota_copy(host, sizeof(host), client->settings->cloud.remote_hostname_tb2);
        port = client->settings->cloud.remote_port_tb2;
        if (!port) port = HTTPS_PORT;
        mutex_unlock(MUTEX_SETTINGS);
        osAcquireMutex(&ota.mutex);
        if (allowed && ota.enabled && !ota.stopping)
        {
            offer.revision = ++ota.offers[id].revision;
            ota.offers[id].valid = false;
            epoch = ota.epoch;
            observer.body = malloc(OTA_MANIFEST_LIMIT + 1);
            if (observer.body) ota.handlers++;
        }
        osReleaseMutex(&ota.mutex);
    }
    if (observer.body) { callbacks.ctx = &observer; callbacks.body = ota_observe_body; }
    error_t error = cloud_request_tb2_post(client->settings->cloud.remote_hostname_tb2,
        0, uri, query, body, length, NULL, &callbacks);
    if (observer.body)
    {
        observer.body[observer.length] = 0;
        offer.valid = !error && !observer.failed && observer.eof &&
            ota_parse(observer.body, observer.length, host, port, &offer);
        osAcquireMutex(&ota.mutex);
        if (ota.enabled && !ota.stopping && epoch == ota.epoch &&
            offer.revision == ota.offers[id].revision) ota.offers[id] = offer;
        ota.handlers--;
        osReleaseMutex(&ota.mutex);
        free(observer.body);
    }
    return error;
}

static error_t tb2_ota_cache_request(HttpConnection *connection, const char *uri,
                              const char *query, client_ctx_t *client, bool *handled)
{
    *handled = false;
    ota_image_t requested = {0};
    if (!ota.ready || !ota_resource(uri, &requested)) return NO_ERROR;
    ota_offer_t offer = {0};
    bool known = false, paired = false;
    unsigned selected = 0;
    ota_job_t *job = NULL, *other = NULL;
    uint64_t epoch;
    mutex_lock(MUTEX_SETTINGS);
    settings_t *settings = client->settings;
    unsigned id = settings->internal.overlayNumber;
    bool cache = settings->cloud.cacheOta &&
        settings->toniebox.boxGeneration == GENERATION_TB2;
    char path[PATH_LEN];
    int path_length = snprintf(path, sizeof(path), "%s%cota%ctb2%c%u%c%s.bin",
        settings->internal.firmwaredirfull, PATH_SEPARATOR, PATH_SEPARATOR,
        PATH_SEPARATOR, requested.type, PATH_SEPARATOR, requested.id);
    cache = cache && path_length > 0 && (size_t)path_length < sizeof(path);
    osAcquireMutex(&ota.mutex);
    epoch = ota.epoch;
    if (cache && !ota.stopping)
    {
        if (ota.enabled && ota_policy(settings) && id > 0 && id < MAX_OVERLAYS &&
            ota.offers[id].valid && settings->internal.overlayUniqueId &&
            !strcmp(ota.offers[id].owner, settings->internal.overlayUniqueId))
        {
            offer = ota.offers[id];
            for (unsigned i = 0; i < 2; i++)
                if (ota_same_resource(&requested, &offer.images[i]))
                { requested = offer.images[i]; selected = i; known = paired = true; break; }
        }
        /* A different box (or a disabled opt-in) must not race an active writer
         * through the legacy handler's .tmp file. Join the known resource. */
        if (!known)
            for (size_t i = 0; i < OTA_ACTIVE_MAX; i++)
                if (ota.active[i].users && !strcmp(path, ota.active[i].path))
                { requested = ota.active[i].image; known = true; break; }
        if (known) ota.handlers++;
    }
    osReleaseMutex(&ota.mutex);
    bool local = settings->cloud.localOta;
    bool cloud = settings->cloud.tb2_v3_enabled && settings->cloud.enableV3Ota;
    if (known)
    {
        if (ota_copy(requested.query, sizeof(requested.query), query ? query : ""))
            job = ota_job_copy(settings, connection->request.userAgent, &requested);
        if (paired) other = ota_job_copy(settings, connection->request.userAgent, &offer.images[1 - selected]);
    }
    mutex_unlock(MUTEX_SETTINGS);
    if (!known) return NO_ERROR;
    if (!job || (paired && !other))
    {
        TRACE_WARNING("TB2 OTA request snapshot unavailable; counterpart cache skipped\r\n");
    }
    *handled = true;
    error_t error = job ? (cloud ? ota_download(job) :
        (ota_verified_file(job->path, job->image.digest) ? NO_ERROR : ERROR_NOT_FOUND)) : ERROR_OUT_OF_MEMORY;
    bool cached = error == NO_ERROR;
    if (!error && local)
        error = httpSendResponseStreamUnsafe(connection, uri, job->path, false);
    else
    {
        httpPrepareHeader(connection, NULL, 0);
        connection->response.statusCode = 404;
        error_t response = httpWriteResponse(connection, NULL, 0, false);
        if (!error) error = response;
    }
    osAcquireMutex(&ota.mutex);
    if (cached && other && ota.enabled && !ota.stopping && epoch == ota.epoch)
    {
        ota_job_free(ota.pending[id]); ota.pending[id] = other; other = NULL;
        osSetEvent(&ota.wake);
    }
    ota.handlers--;
    osReleaseMutex(&ota.mutex);
    ota_job_free(job); ota_job_free(other);
    /* An HTTP response was already produced, including cache-only 404. */
    return NO_ERROR;
}

error_t tb2_ota_cache_serve(HttpConnection *connection, const char *uri,
                           const char *query, client_ctx_t *client,
                           tb2_ota_fallback_t fallback)
{
    bool handled;
    error_t error = tb2_ota_cache_request(connection, uri, query, client, &handled);
    if (handled) return error;
    ota_image_t resource = {0};
    if (!ota.ready || !client->settings->cloud.cacheOta ||
        client->settings->toniebox.boxGeneration != GENERATION_TB2 ||
        !ota_resource(uri, &resource)) return fallback(connection, uri, query, client);
    char path[PATH_LEN];
    int n = snprintf(path, sizeof(path), "%s%cota%ctb2%c%u%c%s.bin",
        client->settings->internal.firmwaredirfull, PATH_SEPARATOR, PATH_SEPARATOR,
        PATH_SEPARATOR, resource.type, PATH_SEPARATOR, resource.id);
    if (n < 0 || (size_t)n >= sizeof(path)) return ERROR_INVALID_PARAMETER;
    OsMutex *lock = ota_resource_lock(path);
    osAcquireMutex(lock);
    /* A worker may have completed while a request without its own offer waited.
     * Use only an unambiguous manifest digest to reuse that finished file. */
    bool known = false, conflict = false;
    osAcquireMutex(&ota.mutex);
    for (size_t i = 1; i < MAX_OVERLAYS; i++)
        for (size_t slot = 0; ota.offers[i].valid && slot < 2; slot++)
        {
            const ota_image_t *image = &ota.offers[i].images[slot];
            if (!ota_same_resource(&resource, image)) continue;
            if (known && memcmp(resource.digest, image->digest, SHA256_DIGEST_SIZE)) conflict = true;
            memcpy(resource.digest, image->digest, SHA256_DIGEST_SIZE); known = true;
        }
    osReleaseMutex(&ota.mutex);
    if (known && !conflict && ota_verified_file(path, resource.digest))
    {
        if (client->settings->cloud.localOta)
            error = httpSendResponseStreamUnsafe(connection, uri, path, false);
        else
        {
            httpPrepareHeader(connection, NULL, 0); connection->response.statusCode = 404;
            error = httpWriteResponse(connection, NULL, 0, false);
        }
    }
    else error = fallback(connection, uri, query, client);
    osReleaseMutex(lock);
    return error;
}
