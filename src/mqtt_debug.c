/* Opt-in, bounded MQTT diagnostics. The MQTT threads only enqueue sanitized
 * records. Filesystem work belongs to the writer or an explicit HTTP export. */
#include "mqtt_debug.h"
#include "os_port.h"
#include "fs_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define strncasecmp _strnicmp
#else
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#endif

#define DEBUG_SLOTS 32U
#define DEBUG_EVENTS 1024U
#define DEBUG_QUEUE_BYTES (8U * 1024U * 1024U)
#define DEBUG_EVENT_BYTES (64U * 1024U)
#define DEBUG_PACKET_ITEMS 64U
#define DEBUG_SEGMENT_BYTES (16U * 1024U * 1024U)
#define DEBUG_BOX_BYTES (512ULL * 1024U * 1024U)
#define DEBUG_TOTAL_BYTES (2ULL * 1024U * 1024U * 1024U)
#define DEBUG_AGE_MS (7ULL * 24U * 60U * 60U * 1000U)
#define DEBUG_PATH 1024U
#define DEBUG_PRELUDE 32U
#define DEBUG_LIST_MAX 128U
#define DEBUG_FILES_MAX 256U
#define DEBUG_SCAN_MAX 8192U
#define DEBUG_SETTINGS_SCAN_MAX 2048U
#define DEBUG_SETTINGS_BYTES 8192U

typedef struct {
    uint64_t owner, started, message, bytes, dropped, gap, sequence, quota_box_bytes;
    uint64_t ended, first_error_sequence, retention_gaps;
    size_t queued;
    char box[13], id[64], reason[64], error[64], first_error_stage[64];
    char settings_json[DEBUG_SETTINGS_BYTES];
    bool used, opening, accepting, closing, established, truncated, finished, release;
    uint8_t overlay;
    FILE *stream;
    uint32_t segment;
    uint64_t segment_bytes;
} debug_session_t;
typedef struct { debug_session_t *session; char *line; size_t length; } debug_event_t;
struct mqtt_debug_file { FILE *stream; uint64_t remaining, size; bool jsonl; };
static struct {
    OsMutex mutex, disk;
    OsEvent wake;
    OsTaskId task;
    bool ready, stopping, stopped;
    char root[DEBUG_PATH], error[64], process_id[64], build[80];
    debug_session_t sessions[DEBUG_SLOTS];
    debug_event_t events[DEBUG_EVENTS];
    size_t head, count, bytes;
    uint64_t sequence, recording_sequence, prelude_dropped;
    cJSON *prelude[DEBUG_PRELUDE];
    size_t prelude_count;
} debug;

static void debug_writer(void *context);
static void debug_write_event(debug_event_t *event);
static void debug_writer_maintenance(void);
static cJSON *debug_record(uint64_t epoch, uint64_t message, const char *origin,
                           const char *stage, cJSON *details);
static void debug_queue(debug_session_t *session, cJSON *record);
static bool debug_quota(debug_session_t *session, uint64_t additional, bool refresh);
static void debug_charge(debug_session_t *session, uint64_t bytes);

/* Directory handles pin the checked hierarchy until the file is opened. POSIX
 * uses openat/O_NOFOLLOW; Windows denies rename/delete sharing on every parent. */
typedef struct {
#ifdef _WIN32
    HANDLE handles[64]; size_t count;
#else
    int fd;
#endif
    char path[DEBUG_PATH];
} debug_directory_t;
static bool debug_list_directory(debug_directory_t *dir,
    bool (*callback)(const char *name, void *context), void *context);
static void debug_directory_close(debug_directory_t *dir)
{
#ifdef _WIN32
    while (dir->count) CloseHandle(dir->handles[--dir->count]);
#else
    if (dir->fd >= 0) close(dir->fd);
    dir->fd = -1;
#endif
}
static bool debug_directory_open(debug_directory_t *dir, const char *path, bool create)
{
    memset(dir, 0, sizeof(*dir));
    if (!path || strlen(path) >= sizeof(dir->path)) return false;
    strcpy(dir->path, path);
    for (char *p = dir->path; *p; p++) if (*p == '\\') *p = '/';
#ifdef _WIN32
    size_t start = strlen(dir->path) >= 3 && dir->path[1] == ':' ? 3 : 0;
    if (!start) return false;
    for (size_t i = start; ; i++) if (!dir->path[i] || dir->path[i] == '/') {
        char saved = dir->path[i]; dir->path[i] = 0;
        if (dir->count == 64) { dir->path[i] = saved; goto failed; }
        if (create) CreateDirectoryA(dir->path, NULL);
        HANDLE h = CreateFileA(dir->path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        bool safe = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        dir->path[i] = saved;
        if (!safe) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); goto failed; }
        dir->handles[dir->count++] = h;
        if (!saved) break;
    }
#else
    dir->fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir->fd < 0) return false;
    char copy[DEBUG_PATH]; strcpy(copy, dir->path); char *state = NULL;
    for (char *part = strtok_r(copy, "/", &state); part; part = strtok_r(NULL, "/", &state)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) goto failed;
        if (create && mkdirat(dir->fd, part, 0700) && errno != EEXIST) goto failed;
        int next = openat(dir->fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) goto failed;
        close(dir->fd); dir->fd = next;
    }
#endif
    return true;
failed:
    debug_directory_close(dir); return false;
}
static FILE *debug_file(const char *path, bool write_file, bool exclusive)
{
    char parent[DEBUG_PATH];
    if (!path || strlen(path) >= sizeof(parent)) return NULL;
    strcpy(parent, path); char *name = strrchr(parent, '/');
    if (!name || !name[1]) return NULL;
    *name++ = 0;
    debug_directory_t dir;
    if (!debug_directory_open(&dir, parent, write_file)) return NULL;
    FILE *file = NULL;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, write_file ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        write_file ? (exclusive ? CREATE_NEW : OPEN_ALWAYS) : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    if (h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks == 1) {
        int fd = _open_osfhandle((intptr_t)h, _O_BINARY | (write_file ? _O_RDWR : _O_RDONLY));
        if (fd >= 0) { file = _fdopen(fd, write_file ? "wb+" : "rb"); if (!file) _close(fd); }
        else CloseHandle(h);
    } else if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    int flags = O_NOFOLLOW | O_CLOEXEC | (write_file ? O_RDWR | O_CREAT : O_RDONLY);
    if (exclusive) flags |= O_EXCL;
    int fd = openat(dir.fd, name, flags, 0600); struct stat info;
    if (fd >= 0) {
        if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_nlink == 1) {
            if (write_file) fchmod(fd, 0600);
            file = fdopen(fd, write_file ? "wb+" : "rb");
        }
        if (!file) close(fd);
    }
#endif
    debug_directory_close(&dir); return file;
}
static bool debug_remove(const char *path)
{
    char parent[DEBUG_PATH]; if (strlen(path) >= sizeof(parent)) return false;
    strcpy(parent, path); char *name = strrchr(parent, '/'); if (!name) return false; *name++ = 0;
    debug_directory_t dir; if (!debug_directory_open(&dir, parent, false)) return false;
#ifdef _WIN32
    bool ok = DeleteFileA(path) != 0;
#else
    bool ok = unlinkat(dir.fd, name, 0) == 0;
#endif
    debug_directory_close(&dir); return ok;
}
static bool debug_path(char path[DEBUG_PATH], const char *box, const char *session, const char *name)
{
    return snprintf(path, DEBUG_PATH, "%s/%s/%s%s%s", debug.root, box, session,
        name ? "/" : "", name ? name : "") < (int)DEBUG_PATH;
}
static bool debug_session_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 16 || n >= sizeof(debug.sessions[0].id)) return false;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)name[i]) && name[i] != '-') return false;
    return true;
}
static bool debug_segment_name(const char *name)
{
    if (strlen(name) != 19 || strncmp(name, "events-", 7) || strcmp(name + 13, ".jsonl")) return false;
    for (size_t i = 7; i < 13; i++) if (!isdigit((unsigned char)name[i])) return false;
    return true;
}
static bool debug_filename(const char *name)
{ return name && (!strcmp(name, "session.json") || !strcmp(name, "summary.txt") || debug_segment_name(name)); }
static void debug_failure(debug_session_t *session, const char *error)
{
    osAcquireMutex(&debug.mutex);
    snprintf(session->error, sizeof(session->error), "%s", error);
    session->accepting = false; session->closing = true; session->truncated = true;
    osReleaseMutex(&debug.mutex);
}
static bool debug_atomic(const char *path, const char *data)
{
    char temporary[DEBUG_PATH];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return false;
    debug_remove(temporary);
    FILE *file = debug_file(temporary, true, true); if (!file) return false;
    size_t length = strlen(data);
    bool ok = fwrite(data, 1, length, file) == length && fflush(file) == 0;
    if (fclose(file)) ok = false;
    if (ok) {
        char parent[DEBUG_PATH]; strcpy(parent, path); char *name = strrchr(parent, '/'); *name++ = 0;
        debug_directory_t dir;
        if (!debug_directory_open(&dir, parent, false)) ok = false;
        else {
#ifdef _WIN32
            ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
            const char *temp_name = strrchr(temporary, '/') + 1;
            ok = renameat(dir.fd, temp_name, dir.fd, name) == 0;
#endif
            debug_directory_close(&dir);
        }
    }
    if (!ok) debug_remove(temporary);
    return ok;
}
typedef struct {
    char path[DEBUG_PATH], box[13], session[64];
    uint64_t modified, size;
    bool present;
} debug_retention_file_t;
typedef struct {
    uint64_t owner;
    char box[13], id[64];
    FILE *stream;
    uint32_t segment;
    bool used;
} debug_retention_session_t;
typedef struct {
    const char *target;
    char box[13], session[64];
    uint64_t total, target_bytes, now;
    uint64_t boxes[DEBUG_SLOTS];
    debug_retention_session_t sessions[DEBUG_SLOTS];
    size_t entries;
    bool failed;
    debug_retention_file_t oldest, target_oldest, expired;
} debug_retention_scan_t;
static uint64_t debug_usage_total, debug_usage_scanned;

static void debug_retention_candidate(debug_retention_file_t *candidate,
    debug_retention_scan_t *scan, const char *path, uint64_t modified, uint64_t size)
{
    if (candidate->present && (modified > candidate->modified ||
        (modified == candidate->modified && strcmp(path, candidate->path) >= 0))) return;
    snprintf(candidate->path, sizeof(candidate->path), "%s", path);
    strcpy(candidate->box, scan->box); strcpy(candidate->session, scan->session);
    candidate->size = size; candidate->modified = modified; candidate->present = true;
}
static bool debug_retention_file(const char *name, void *context)
{
    debug_retention_scan_t *scan = context;
    if (++scan->entries > DEBUG_SCAN_MAX) { scan->failed = true; return false; }
    if (!debug_filename(name)) return true;
    char path[DEBUG_PATH]; if (!debug_path(path, scan->box, scan->session, name)) { scan->failed = true; return false; }
    FILE *file = debug_file(path, false, false);
    if (!file) { scan->failed = true; return false; }
#ifdef _WIN32
    struct _stat64 info; bool valid = _fstat64(_fileno(file), &info) == 0;
#else
    struct stat info; bool valid = fstat(fileno(file), &info) == 0;
#endif
    fclose(file);
    if (!valid || info.st_size < 0) { scan->failed = true; return false; }
    uint64_t size = (uint64_t)info.st_size;
    scan->total += size;
    if (!strcmp(scan->box, scan->target)) scan->target_bytes += size;
    bool writing = false;
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (scan->sessions[i].used) {
        debug_retention_session_t *session = &scan->sessions[i];
        if (!strcmp(session->box, scan->box)) scan->boxes[i] += size;
        if (!strcmp(session->box, scan->box) && !strcmp(session->id, scan->session) && session->stream) {
            char current[32]; snprintf(current, sizeof(current), "events-%06u.jsonl", session->segment);
            if (!strcmp(name, current)) writing = true;
        }
    }
    if (writing || !debug_segment_name(name)) return true;
    uint64_t modified = info.st_mtime > 0 ? (uint64_t)info.st_mtime * 1000U : 0;
    debug_retention_candidate(&scan->oldest, scan, path, modified, size);
    if (!strcmp(scan->box, scan->target)) debug_retention_candidate(&scan->target_oldest, scan, path, modified, size);
    if (scan->now > modified && scan->now - modified > DEBUG_AGE_MS)
        debug_retention_candidate(&scan->expired, scan, path, modified, size);
    return true;
}
static bool debug_retention_session(const char *name, void *context)
{
    debug_retention_scan_t *scan = context;
    if (++scan->entries > DEBUG_SCAN_MAX) { scan->failed = true; return false; }
    if (!debug_session_name(name)) return true;
    snprintf(scan->session, sizeof(scan->session), "%s", name);
    char path[DEBUG_PATH];
    if (!debug_path(path, scan->box, name, NULL)) { scan->failed = true; return false; }
    debug_directory_t dir;
    if (!debug_directory_open(&dir, path, false)) { scan->failed = true; return false; }
    bool ok = debug_list_directory(&dir, debug_retention_file, scan);
    debug_directory_close(&dir);
    if (!ok) scan->failed = true;
    return !scan->failed;
}
static bool debug_retention_box(const char *name, void *context)
{
    debug_retention_scan_t *scan = context; char canonical[13];
    if (++scan->entries > DEBUG_SCAN_MAX) { scan->failed = true; return false; }
    if (!settings_canonicalize_box_id(name, canonical, sizeof(canonical)) || strcmp(name, canonical)) return true;
    strcpy(scan->box, name);
    char path[DEBUG_PATH];
    if (snprintf(path, sizeof(path), "%s/%s", debug.root, name) >= (int)sizeof(path)) { scan->failed = true; return false; }
    debug_directory_t dir;
    if (!debug_directory_open(&dir, path, false)) { scan->failed = true; return false; }
    bool ok = debug_list_directory(&dir, debug_retention_session, scan);
    debug_directory_close(&dir);
    if (!ok) scan->failed = true;
    return !scan->failed;
}
static bool debug_retention_mark(const debug_retention_file_t *removed)
{
    osAcquireMutex(&debug.mutex);
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (debug.sessions[i].used &&
        !strcmp(debug.sessions[i].box, removed->box) && !strcmp(debug.sessions[i].id, removed->session))
        debug.sessions[i].truncated = true;
    osReleaseMutex(&debug.mutex);
    char path[DEBUG_PATH];
    if (!debug_path(path, removed->box, removed->session, "session.json")) return false;
    FILE *file = debug_file(path, false, false); if (!file) return false;
    char *text = malloc(DEBUG_EVENT_BYTES + 1); if (!text) { fclose(file); return false; }
    size_t length = fread(text, 1, DEBUG_EVENT_BYTES, file); bool ok = !ferror(file) && fgetc(file) == EOF;
    fclose(file); text[length] = 0;
    cJSON *json = ok ? cJSON_ParseWithOpts(text, NULL, true) : NULL; free(text);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return false; }
    cJSON_DeleteItemFromObjectCaseSensitive(json, "truncated"); cJSON_AddBoolToObject(json, "truncated", true);
    cJSON *previous = cJSON_GetObjectItemCaseSensitive(json, "retentionGaps");
    double gaps = cJSON_IsNumber(previous) && previous->valuedouble >= 0 ? previous->valuedouble : 0;
    cJSON_DeleteItemFromObjectCaseSensitive(json, "retentionGaps"); cJSON_AddNumberToObject(json, "retentionGaps", gaps + 1);
    text = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (!text) return false;
    ok = debug_atomic(path, text); free(text);
    if (ok) {
        osAcquireMutex(&debug.mutex);
        for (size_t i = 0; i < DEBUG_SLOTS; i++) if (debug.sessions[i].used &&
            !strcmp(debug.sessions[i].box, removed->box) && !strcmp(debug.sessions[i].id, removed->session))
            debug.sessions[i].retention_gaps = (uint64_t)gaps + 1;
        osReleaseMutex(&debug.mutex);
    }
    return ok;
}
static bool debug_quota(debug_session_t *session, uint64_t additional, bool refresh)
{
    const uint64_t metadata_reserve = DEBUG_EVENT_BYTES;
    if (!refresh && debug_usage_scanned && mqtt_debug_now_ms() - debug_usage_scanned < 1000 &&
        debug_usage_total + additional + metadata_reserve < DEBUG_TOTAL_BYTES &&
        session->quota_box_bytes + additional + metadata_reserve < DEBUG_BOX_BYTES) return true;
    /* Only this writer modifies streams. Flush before calculating physical sizes;
     * producers never need the disk lock held throughout this scan. */
    FILE *streams[DEBUG_SLOTS];
    osAcquireMutex(&debug.mutex);
    for (size_t i = 0; i < DEBUG_SLOTS; i++) streams[i] = debug.sessions[i].stream;
    osReleaseMutex(&debug.mutex);
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (streams[i] && fflush(streams[i])) return false;
    debug_directory_t root;
    if (!debug_directory_open(&root, debug.root, true)) return false;
    bool ok = false;
    for (size_t attempt = 0; attempt < DEBUG_SCAN_MAX; attempt++) {
        debug_retention_scan_t scan = {.target = session->box, .now = mqtt_debug_wall_ms()};
        osAcquireMutex(&debug.mutex);
        for (size_t i = 0; i < DEBUG_SLOTS; i++) {
            debug_retention_session_t *target = &scan.sessions[i];
            const debug_session_t *source = &debug.sessions[i];
            target->owner = source->owner; target->stream = source->stream;
            target->used = source->used; target->segment = source->segment;
            strcpy(target->box, source->box); strcpy(target->id, source->id);
        }
        osReleaseMutex(&debug.mutex);
        if (!debug_list_directory(&root, debug_retention_box, &scan) || scan.failed) break;
        debug_retention_file_t *remove = scan.expired.present ? &scan.expired :
            scan.target_bytes + additional + metadata_reserve > DEBUG_BOX_BYTES ? &scan.target_oldest :
            scan.total + additional + metadata_reserve > DEBUG_TOTAL_BYTES ? &scan.oldest : NULL;
        if (!remove) {
            debug_usage_total = scan.total; debug_usage_scanned = mqtt_debug_now_ms();
            osAcquireMutex(&debug.mutex);
            for (size_t i = 0; i < DEBUG_SLOTS; i++) if (debug.sessions[i].owner == scan.sessions[i].owner)
                debug.sessions[i].quota_box_bytes = scan.boxes[i];
            osReleaseMutex(&debug.mutex);
            ok = true; break;
        }
        if (!remove->present || !debug_retention_mark(remove) || !debug_remove(remove->path)) break;
    }
    debug_directory_close(&root); return ok;
}
static void debug_charge(debug_session_t *session, uint64_t bytes)
{
    debug_usage_total += bytes;
    osAcquireMutex(&debug.mutex);
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (debug.sessions[i].used &&
        !strcmp(debug.sessions[i].box, session->box)) debug.sessions[i].quota_box_bytes += bytes;
    osReleaseMutex(&debug.mutex);
}

static bool debug_summary(debug_session_t *session, bool active)
{
    cJSON *json = cJSON_CreateObject(); if (!json) return false;
    osAcquireMutex(&debug.mutex);
    debug_session_t saved = *session;
    cJSON_AddNumberToObject(json, "schemaVersion", 1); cJSON_AddStringToObject(json, "id", session->id);
    cJSON_AddStringToObject(json, "box", session->box); cJSON_AddNumberToObject(json, "startedAt", (double)session->started);
    cJSON_AddBoolToObject(json, "active", active); cJSON_AddBoolToObject(json, "truncated", session->truncated);
    cJSON_AddNumberToObject(json, "bytes", (double)session->bytes); cJSON_AddNumberToObject(json, "droppedEvents", (double)session->dropped);
    cJSON_AddNumberToObject(json, "retentionGaps", (double)session->retention_gaps);
    cJSON_AddStringToObject(json, "error", session->error); cJSON_AddStringToObject(json, "reason", session->reason);
    cJSON_AddNumberToObject(json, "overlay", session->overlay);
    cJSON_AddStringToObject(json, "processStartId", debug.process_id);
    cJSON_AddStringToObject(json, "build", debug.build);
    cJSON_AddNumberToObject(json, "localConnectionId", (double)saved.owner);
    cJSON_AddNumberToObject(json, "endedAt", (double)saved.ended);
    cJSON_AddNumberToObject(json, "eventCount", (double)saved.sequence);
    cJSON_AddNumberToObject(json, "firstErrorSequence", (double)saved.first_error_sequence);
    cJSON_AddStringToObject(json, "firstErrorStage", saved.first_error_stage);
    cJSON *limits = cJSON_AddObjectToObject(json, "limits");
    cJSON_AddNumberToObject(limits, "queueEvents", DEBUG_EVENTS);
    cJSON_AddNumberToObject(limits, "queueBytes", DEBUG_QUEUE_BYTES);
    cJSON_AddNumberToObject(limits, "segmentBytes", DEBUG_SEGMENT_BYTES);
    cJSON_AddNumberToObject(limits, "boxBytes", (double)DEBUG_BOX_BYTES);
    cJSON_AddNumberToObject(limits, "totalBytes", (double)DEBUG_TOTAL_BYTES);
    cJSON_AddNumberToObject(limits, "retentionMs", (double)DEBUG_AGE_MS);
    cJSON *effective = cJSON_Parse(saved.settings_json);
    if (effective) cJSON_AddItemToObject(json, "effectiveSettings", effective);
    osReleaseMutex(&debug.mutex);
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json); if (!text) return false;
    char path[DEBUG_PATH]; bool ok = debug_path(path, session->box, session->id, "session.json") && debug_atomic(path, text);
    free(text);
    char summary[1024];
    snprintf(summary, sizeof(summary), "MQTT diagnostic session %s\nBox: %s\nBytes: %" PRIu64 "\nDropped events: %" PRIu64
        "\nTruncated: %s\nDiagnostic error: %s\nEnd reason: %s\nFirst error observation: %s (sequence %" PRIu64 ")"
        "\nEvidence: TC observations only; file order is not global network order."
        "\nNo firmware fault or application success is inferred from a write or MQTT ACK."
        "\nInspect linked events and transport_snapshot for queue/parser/keepalive state.\n",
        saved.id, saved.box, saved.bytes, saved.dropped, saved.truncated ? "yes" : "no",
        saved.error, saved.reason, saved.first_error_stage, saved.first_error_sequence);
    return debug_path(path, session->box, session->id, "summary.txt") && debug_atomic(path, summary) && ok;
}
static bool debug_open_segment(debug_session_t *session)
{
    if (!debug_quota(session, 8192, true)) return false;
    char name[32], path[DEBUG_PATH];
    snprintf(name, sizeof(name), "events-%06u.jsonl", session->segment);
    if (!debug_path(path, session->box, session->id, name)) return false;
    session->stream = debug_file(path, true, true); session->segment_bytes = 0;
    return session->stream != NULL;
}
static void debug_write_event(debug_event_t *event)
{
    debug_session_t *session = event->session;
    if (!session->error[0] && session->stream) {
        if (session->segment_bytes && session->segment_bytes + event->length + 1 > DEBUG_SEGMENT_BYTES) {
            if (fclose(session->stream)) debug_failure(session, "flush_failed");
            session->stream = NULL; session->segment++;
            if (!session->error[0] && !debug_open_segment(session)) debug_failure(session, "segment_open_failed");
        }
        if (session->stream && !session->error[0] && !debug_quota(session, event->length + 1, false))
            debug_failure(session, "quota_exhausted");
        if (session->stream && !session->error[0]) {
            bool ok = fwrite(event->line, 1, event->length, session->stream) == event->length && fputc('\n', session->stream) != EOF;
            if (!ok) debug_failure(session, "write_failed");
            else {
                session->segment_bytes += event->length + 1;
                debug_charge(session, event->length + 1);
                osAcquireMutex(&debug.mutex); session->bytes += event->length + 1; osReleaseMutex(&debug.mutex);
            }
        }
    }
    free(event->line);
    osAcquireMutex(&debug.mutex); session->queued--; osReleaseMutex(&debug.mutex);
}
static void debug_writer_maintenance(void)
{
    for (size_t i = 0; i < DEBUG_SLOTS; i++) {
        debug_session_t *session = &debug.sessions[i];
        osAcquireMutex(&debug.mutex);
        bool used = session->used, opening = session->opening;
        bool finished = session->finished;
        if (finished && session->release) { session->used = false; used = false; }
        bool closing = session->closing && !session->queued;
        uint64_t gap = session->gap;
        session->opening = false;
        if (gap && debug.count < DEBUG_EVENTS && debug.bytes < DEBUG_QUEUE_BYTES / 2 && !session->error[0]) {
            session->gap = 0;
            cJSON *details = cJSON_CreateObject();
            cJSON_AddNumberToObject(details, "droppedEvents", (double)gap);
            debug_queue(session, debug_record(0, 0, "teddycloud", "diagnostic_gap", details));
            closing = false;
        }
        osReleaseMutex(&debug.mutex);
        if (!used || finished) continue;
        if (opening && (!debug_open_segment(session) || !debug_summary(session, true))) debug_failure(session, "capture_open_failed");
        if (session->stream && fflush(session->stream)) debug_failure(session, "flush_failed");
        if (!session->error[0] && !debug_quota(session, 8192, false)) debug_failure(session, "quota_exhausted");
        if (closing) {
            if (session->stream && fclose(session->stream)) debug_failure(session, "flush_failed");
            session->stream = NULL;
            if (!debug_summary(session, false)) debug_failure(session, "summary_write_failed");
            osAcquireMutex(&debug.mutex);
            session->finished = true;
            if (!session->error[0] || session->release) session->used = false;
            osReleaseMutex(&debug.mutex);
        }
    }
}
static void debug_writer(void *context)
{
#ifndef _WIN32
    /* POSIX OS tasks are joinable by default; this process-owned writer has no
     * joining consumer and must release its thread resources after a reload. */
    pthread_detach(pthread_self());
#endif
    (void)context; uint64_t last_flush = 0;
    for (;;) {
        osAcquireMutex(&debug.disk);
        if (mqtt_debug_now_ms() - last_flush >= 1000) { debug_writer_maintenance(); last_flush = mqtt_debug_now_ms(); }
        osAcquireMutex(&debug.mutex);
        bool stop = debug.stopping && !debug.count;
        debug_event_t event = {0};
        if (debug.count) { event = debug.events[debug.head]; debug.head = (debug.head + 1) % DEBUG_EVENTS; debug.count--; debug.bytes -= event.length + 1; }
        osReleaseMutex(&debug.mutex);
        if (event.line) {
            if (event.session->opening) debug_writer_maintenance();
            debug_write_event(&event);
        }
        if (stop) {
            debug_writer_maintenance();
            /* Closing can enqueue a final gap record. Drain it before exit. */
            osAcquireMutex(&debug.mutex); stop = debug.count == 0; osReleaseMutex(&debug.mutex);
        }
        osReleaseMutex(&debug.disk);
        if (stop) break;
        if (!event.line) osWaitForEvent(&debug.wake, 100);
    }
    osAcquireMutex(&debug.mutex); debug.stopped = true; osReleaseMutex(&debug.mutex);
    osDeleteTask(OS_SELF_TASK_ID);
}
void mqtt_debug_deinit(void)
{
    if (!debug.ready) return;
    osAcquireMutex(&debug.mutex);
    debug.stopping = true;
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (debug.sessions[i].used) {
        debug.sessions[i].accepting = false; debug.sessions[i].closing = true;
        debug.sessions[i].release = true;
        debug.sessions[i].ended = mqtt_debug_wall_ms();
        strcpy(debug.sessions[i].reason, "server_shutdown");
    }
    osReleaseMutex(&debug.mutex); osSetEvent(&debug.wake);
    /* Do not wait indefinitely for a filesystem blocked by the operating system.
     * A still-running writer retains its state until process exit. */
    for (unsigned i = 0; i < 20; i++) {
        osAcquireMutex(&debug.mutex); bool stopped = debug.stopped; osReleaseMutex(&debug.mutex);
        if (stopped) return;
        osDelayTask(50);
    }
}

uint64_t mqtt_debug_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
#endif
}
uint64_t mqtt_debug_wall_ms(void)
{
#ifdef _WIN32
    FILETIME t; GetSystemTimeAsFileTime(&t);
    ULARGE_INTEGER n; n.LowPart = t.dwLowDateTime; n.HighPart = t.dwHighDateTime;
    return (n.QuadPart - 116444736000000000ULL) / 10000U;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
#endif
}
bool mqtt_debug_enabled(settings_t *settings)
{
    return settings && settings->internal.config_used && settings->internal.overlayNumber > 0 &&
        settings->toniebox.boxGeneration == GENERATION_TB2 &&
        settings_is_overlayed_id("mqtt_server.debug_enabled", settings->internal.overlayNumber) &&
        settings->mqtt_server.debug_enabled;
}
static debug_session_t *debug_find(uint64_t owner)
{
    for (size_t i = 0; i < DEBUG_SLOTS; i++)
        if (debug.sessions[i].used && debug.sessions[i].owner == owner) return &debug.sessions[i];
    return NULL;
}
static bool debug_box(settings_t *settings, char box[13])
{
    return settings && settings->internal.overlayNumber > 0 && settings->internal.config_used &&
        settings_canonicalize_box_id(settings->internal.overlayUniqueId, box, 13);
}
static bool debug_sensitive(const char *name)
{
    char lowered[128]; size_t n = name ? strlen(name) : 0;
    if (n >= sizeof(lowered)) return true;
    for (size_t i = 0; i <= n; i++) lowered[i] = (char)tolower((unsigned char)(name ? name[i] : 0));
    static const char *keys[] = {"auth", "password", "passwd", "secret", "token", "credential",
        "privatekey", "private_key", "certificate", "cookie", "authorization", "signature", "username"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strstr(lowered, keys[i])) return true;
    return false;
}
/* Preserve normal messages and URLs. Mask explicitly recognizable credential
 * assignments, Bearer credentials and URL userinfo; this is not a universal
 * classifier for secrets hidden in otherwise innocuous prose. */
static char *debug_sanitize_text(const char *text)
{
    size_t length = text ? strlen(text) : 0;
    if (length > DEBUG_EVENT_BYTES || (text && strstr(text, "-----BEGIN")))
        return strdup("[omitted: oversized text or PEM]");
    char *out = malloc(length * 3 + 32); if (!out) return NULL;
    size_t pos = 0, written = 0;
    while (pos < length) {
        if (pos + 3 <= length && !strncmp(text + pos, "://", 3)) {
            memcpy(out + written, "://", 3); written += 3; pos += 3;
            size_t end = pos;
            while (end < length && !strchr("/?# \t\r\n\"'", text[end])) end++;
            const char *at = memchr(text + pos, '@', end - pos);
            if (at) { memcpy(out + written, "[redacted]@", 11); written += 11; pos = (size_t)(at - text) + 1; }
            continue;
        }
        bool start = isalpha((unsigned char)text[pos]) &&
            (!pos || !isalnum((unsigned char)text[pos - 1]));
        if (start) {
            size_t end = pos;
            while (end < length && (isalnum((unsigned char)text[end]) || text[end] == '_' || text[end] == '-')) end++;
            char word[128]; size_t n = end - pos;
            if (n < sizeof(word)) {
                for (size_t i = 0; i < n; i++) word[i] = (char)tolower((unsigned char)text[pos + i]);
                word[n] = 0;
                bool bearer = !strcmp(word, "bearer");
                bool secret = debug_sensitive(word) || !strcmp(word, "key") || !strcmp(word, "sig") || !strcmp(word, "jwt");
                size_t value = end;
                while (value < length && (isspace((unsigned char)text[value]) || text[value] == '\"' || text[value] == '\'')) value++;
                bool assignment = value < length && (text[value] == ':' || text[value] == '=');
                if (secret && assignment) {
                    value++;
                    while (value < length && (isspace((unsigned char)text[value]) || text[value] == '\"' || text[value] == '\'')) value++;
                }
                if ((secret && assignment) || (bearer && value > end)) {
                    memcpy(out + written, text + pos, value - pos); written += value - pos;
                    memcpy(out + written, "[redacted]", 10); written += 10;
                    pos = value;
                    if (!bearer && length - pos >= 7 && !strncasecmp(text + pos, "Bearer ", 7)) pos += 7;
                    while (pos < length && !strchr(" &;,\t\r\n\"'}]", text[pos])) pos++;
                    continue;
                }
            }
        }
        out[written++] = text[pos++];
    }
    out[written] = 0; return out;
}
static bool debug_redact(cJSON *node, unsigned depth, size_t *budget)
{
    if (!node || depth > 24 || ++*budget > 4096) return false;
    for (cJSON *item = node->child; item; item = item->next) {
        if (++*budget > 4096) return false;
        /* Only TC's top-level correlation counter is exempt. A payload field
         * with this name is untrusted and must still be masked. */
        bool worker_token = depth == 0 && item->string && !strcmp(item->string, "worker_token") && cJSON_IsNumber(item);
        bool secret = debug_sensitive(item->string) && !worker_token;
        if (secret || cJSON_IsString(item)) {
            char *text = secret ? strdup("[redacted]") : debug_sanitize_text(item->valuestring);
            cJSON *replacement = text ? cJSON_CreateString(text) : NULL; free(text);
            if (!replacement) return false;
            if (item->string) {
                replacement->string = strdup(item->string);
                if (!replacement->string) { cJSON_Delete(replacement); return false; }
            }
            if (!cJSON_ReplaceItemViaPointer(node, item, replacement)) {
                cJSON_Delete(replacement); return false;
            }
            item = replacement;
        } else if (!debug_redact(item, depth + 1, budget)) return false;
    }
    return true;
}
static cJSON *debug_record(uint64_t epoch, uint64_t message, const char *origin,
                           const char *stage, cJSON *details)
{
    cJSON *record = cJSON_CreateObject(); size_t budget = 0;
    if (!record) { cJSON_Delete(details); return NULL; }
    cJSON_AddNumberToObject(record, "schemaVersion", 1);
    cJSON_AddNumberToObject(record, "monotonicMs", (double)mqtt_debug_now_ms());
    cJSON_AddNumberToObject(record, "wallTimeMs", (double)mqtt_debug_wall_ms());
    cJSON_AddNumberToObject(record, "epoch", (double)epoch);
    cJSON_AddNumberToObject(record, "messageId", (double)message);
    cJSON_AddStringToObject(record, "origin", origin ? origin : "unknown");
    cJSON_AddStringToObject(record, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(record, "build", debug.build);
    cJSON_AddStringToObject(record, "processStartId", debug.process_id);
    const char *direction = "local";
    if (origin && !strcmp(origin, "box")) direction = "BOX_TO_TC";
    else if (origin && !strcmp(origin, "tonies")) direction = "TONIES_TO_TC";
    else if (origin && !strcmp(origin, "box_transport")) direction = "BOX_TC_TRANSPORT";
    else if (origin && !strcmp(origin, "tonies_transport")) direction = "TC_TONIES_TRANSPORT";
    const cJSON *explicit_direction = cJSON_GetObjectItemCaseSensitive(details, "direction");
    if (cJSON_IsString(explicit_direction)) direction = explicit_direction->valuestring;
    else if (stage && !strcmp(stage, "upstream_queued")) direction = "TC_TO_TONIES";
    else if (stage && !strcmp(stage, "box_queued")) direction = "TC_TO_BOX";
    cJSON_AddStringToObject(record, "direction", direction);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(details, "error");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(details, "action");
    cJSON_AddStringToObject(record, "result", cJSON_IsNumber(error) && error->valuedouble != 0 ?
        "error_observed" : cJSON_IsString(action) ? action->valuestring : "observed");
    if ((cJSON_IsObject(details) || cJSON_IsArray(details)) && debug_redact(details, 0, &budget)) cJSON_AddItemToObject(record, "details", details);
    else { cJSON_Delete(details); cJSON_AddStringToObject(record, "omitted", "unsafe_or_oversized_details"); }
    return record;
}
static void debug_queue(debug_session_t *session, cJSON *record)
{
    session->sequence++;
    if (record) {
        if (!cJSON_GetObjectItemCaseSensitive(record, "sequence"))
            cJSON_AddNumberToObject(record, "sequence", (double)++debug.sequence);
        cJSON_AddNumberToObject(record, "recordingSequence", (double)session->sequence);
        cJSON_AddStringToObject(record, "recordingId", session->id);
        const cJSON *origin = cJSON_GetObjectItemCaseSensitive(record, "origin");
        bool unassigned = cJSON_IsString(origin) && !strcmp(origin->valuestring, "unassigned");
        cJSON_AddStringToObject(record, "box", unassigned ? "unassigned" : session->box);
        cJSON_AddNumberToObject(record, "overlay", unassigned ? 0 : session->overlay);
        cJSON_AddNumberToObject(record, "localConnectionId", unassigned ? 0 : (double)session->owner);
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(record, "result");
        if (!session->first_error_sequence && cJSON_IsString(result) && !strcmp(result->valuestring, "error_observed")) {
            const cJSON *stage = cJSON_GetObjectItemCaseSensitive(record, "stage");
            session->first_error_sequence = session->sequence;
            snprintf(session->first_error_stage, sizeof(session->first_error_stage), "%s",
                cJSON_IsString(stage) ? stage->valuestring : "unknown");
        }
    }
    char *line = record ? cJSON_PrintUnformatted(record) : NULL;
    cJSON_Delete(record);
    size_t length = line ? strlen(line) : 0;
    if (!line || length > DEBUG_EVENT_BYTES || debug.count == DEBUG_EVENTS ||
        debug.bytes + length + 1 > DEBUG_QUEUE_BYTES) {
        free(line); session->dropped++; session->gap++; session->truncated = true; return;
    }
    size_t index = (debug.head + debug.count) % DEBUG_EVENTS;
    debug.events[index] = (debug_event_t){session, line, length};
    debug.count++; debug.bytes += length + 1; session->queued++;
}
void mqtt_debug_init(void)
{
    if (debug.ready) {
        osAcquireMutex(&debug.mutex);
        bool restart = debug.stopped;
        if (restart) { debug.stopping = false; debug.stopped = false; debug.error[0] = 0; }
        osReleaseMutex(&debug.mutex);
        if (!restart) return;
    } else {
        memset(&debug, 0, sizeof(debug));
        if (!osCreateMutex(&debug.mutex)) return;
        if (!osCreateMutex(&debug.disk)) { osDeleteMutex(&debug.mutex); return; }
        if (!osCreateEvent(&debug.wake)) { osDeleteMutex(&debug.disk); osDeleteMutex(&debug.mutex); return; }
#ifdef _WIN32
        uint64_t pid = GetCurrentProcessId();
#else
        uint64_t pid = (uint64_t)getpid();
#endif
        snprintf(debug.process_id, sizeof(debug.process_id), "%013" PRIu64 "-%08" PRIx64 "-%012" PRIx64,
            mqtt_debug_wall_ms(), pid, mqtt_debug_now_ms());
        const char *build = get_settings()->internal.version.git_sha;
        snprintf(debug.build, sizeof(debug.build), "%s", build && build[0] ? build : "unknown");
    }
    const char *base = get_settings()->internal.datadirfull;
    if (!base || snprintf(debug.root, sizeof(debug.root), "%s/diagnostics/tb2-mqtt-debug", base) >= (int)sizeof(debug.root))
        snprintf(debug.error, sizeof(debug.error), "invalid_debug_directory");
    debug.ready = true;
    OsTaskParameters parameters = OS_TASK_DEFAULT_PARAMS; parameters.stackSize = 64U * 1024U;
    debug.task = osCreateTask("mqtt-debug-writer", debug_writer, NULL, &parameters);
    if (debug.task == (OsTaskId)OS_INVALID_TASK_ID) {
        snprintf(debug.error, sizeof(debug.error), "writer_start_failed"); debug.stopped = true;
    }
}

/* Only non-secret configuration; no option pointers survive a settings reload. */
static void debug_settings_snapshot(settings_t *settings, char output[DEBUG_SETTINGS_BYTES])
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "upstreamEnabled", settings->mqtt_client_upstream.enabled);
    /* This branch applies individual filters without a manual master switch. */
    cJSON_AddBoolToObject(json, "manualFiltersEnabled", true);
    cJSON_AddStringToObject(json, "manualFilterMaster", "not_available");
    cJSON_AddBoolToObject(json, "localControlEnabled", settings->mqtt_client_upstream.local_control_enabled);
    cJSON_AddNumberToObject(json, "boxGeneration", settings->toniebox.boxGeneration);
    cJSON *rules = cJSON_AddObjectToObject(json, "forwardRules");
    unsigned count = MIN(settings_get_size(), DEBUG_SETTINGS_SCAN_MAX);
    for (unsigned i = 0; i < count; i++) {
        setting_item_t *item = settings_get_ovl((int)i, settings->internal.overlayUniqueId);
        if (!item) break;
        if (item->option_name && item->type == TYPE_BOOL && item->ptr &&
            !strncmp(item->option_name, "mqtt_client_upstream.forward.", 29))
            cJSON_AddBoolToObject(rules, item->option_name, *(bool *)item->ptr);
    }
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    if (text && strlen(text) < DEBUG_SETTINGS_BYTES) strcpy(output, text);
    else strcpy(output, "{\"omitted\":\"settings_snapshot_unavailable\"}");
    free(text); cJSON_Delete(json);
}

void mqtt_debug_sync(uint64_t owner, settings_t *settings, bool established)
{
    if (!debug.ready || !owner) return;
    bool enabled = mqtt_debug_enabled(settings); char box[13];
    if (!enabled || !debug_box(settings, box)) { mqtt_debug_close(owner, "disabled"); return; }
    osAcquireMutex(&debug.mutex);
    debug_session_t *session = debug_find(owner);
    if (session) { session->established = established; osReleaseMutex(&debug.mutex); return; }
    if (debug.error[0] || debug.stopping) { osReleaseMutex(&debug.mutex); return; }
    for (size_t i = 0; i < DEBUG_SLOTS; i++) if (!debug.sessions[i].used) { session = &debug.sessions[i]; break; }
    if (session) {
        memset(session, 0, sizeof(*session)); session->owner = owner;
        session->used = session->opening = session->accepting = true;
        session->segment = 1;
        session->established = established; session->started = mqtt_debug_wall_ms();
        session->overlay = settings->internal.overlayNumber;
        strcpy(session->box, box);
        snprintf(session->id, sizeof(session->id), "%013" PRIu64 "-%.*s-%08" PRIx64,
                 session->started, 22, debug.process_id + 14, ++debug.recording_sequence);
        debug_settings_snapshot(settings, session->settings_json);
        cJSON *details = cJSON_CreateObject(); cJSON_AddBoolToObject(details, "established", established);
        cJSON_AddNumberToObject(details, "unassignedDroppedEvents", (double)debug.prelude_dropped);
        debug_queue(session, debug_record(0, 0, "teddycloud", established ? "mid_session_start" : "recording_started", details));
        for (size_t i = 0; i < debug.prelude_count; i++) debug_queue(session, cJSON_Duplicate(debug.prelude[i], true));
    }
    osReleaseMutex(&debug.mutex); osSetEvent(&debug.wake);
}
void mqtt_debug_close(uint64_t owner, const char *reason)
{
    if (!debug.ready) return;
    osAcquireMutex(&debug.mutex); debug_session_t *session = debug_find(owner);
    if (session) session->release = true;
    if (session && !session->closing) {
        session->accepting = false; session->closing = true;
        session->ended = mqtt_debug_wall_ms();
        snprintf(session->reason, sizeof(session->reason), "%s", reason ? reason : "closed");
        cJSON *details = cJSON_CreateObject(); cJSON_AddStringToObject(details, "reason", session->reason);
        debug_queue(session, debug_record(0, 0, "teddycloud", "recording_stopped", details));
    }
    osReleaseMutex(&debug.mutex); osSetEvent(&debug.wake);
}
bool mqtt_debug_active(uint64_t owner)
{
    if (!debug.ready) return false;
    osAcquireMutex(&debug.mutex); debug_session_t *session = debug_find(owner);
    bool result = session && session->accepting && !session->error[0];
    osReleaseMutex(&debug.mutex); return result;
}
uint64_t mqtt_debug_next_message(uint64_t owner)
{
    if (!debug.ready) return 0;
    osAcquireMutex(&debug.mutex); debug_session_t *session = debug_find(owner);
    uint64_t id = session && session->accepting ? ++session->message : 0;
    osReleaseMutex(&debug.mutex); return id;
}
void mqtt_debug_event(uint64_t owner, uint64_t epoch, uint64_t message,
                      const char *origin, const char *stage, cJSON *details)
{
    if (!debug.ready) { cJSON_Delete(details); return; }
    cJSON *record = debug_record(epoch, message, origin, stage, details);
    osAcquireMutex(&debug.mutex); debug_session_t *session = debug_find(owner);
    if (session && session->accepting) debug_queue(session, record);
    else cJSON_Delete(record);
    osReleaseMutex(&debug.mutex);
}
void mqtt_debug_unassigned(const char *stage, cJSON *details)
{
    bool selected = false;
    for (uint8_t i = 1; i < MAX_OVERLAYS && !selected; i++) selected = mqtt_debug_enabled(get_settings_id(i));
    if (!debug.ready || !selected) { cJSON_Delete(details); return; }
    cJSON *record = debug_record(0, 0, "unassigned", stage, details);
    osAcquireMutex(&debug.mutex);
    if (record) cJSON_AddNumberToObject(record, "sequence", (double)++debug.sequence);
    if (debug.prelude_count == DEBUG_PRELUDE) {
        cJSON_Delete(debug.prelude[0]); memmove(debug.prelude, debug.prelude + 1, sizeof(debug.prelude[0]) * (DEBUG_PRELUDE - 1));
        debug.prelude_count--; debug.prelude_dropped++;
    }
    debug.prelude[debug.prelude_count++] = record;
    osReleaseMutex(&debug.mutex);
}
/* cJSON accepts arbitrary non-ASCII bytes; only persist valid UTF-8 as text. */
static bool debug_utf8(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint32_t c = data[i++], minimum = 0; unsigned extra = 0;
        if (c < 0x80) {
            if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) return false;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; minimum = 0x10000; c &= 7; }
        else return false;
        if (extra > length - i) return false;
        while (extra--) {
            if ((data[i] & 0xc0) != 0x80) return false;
            c = (c << 6) | (data[i++] & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}

static bool debug_packet_string(const uint8_t *data, size_t length, size_t *pos,
                                char *output, size_t capacity)
{
    if (*pos > length || length - *pos < 2) return false;
    size_t count = ((size_t)data[*pos] << 8) | data[*pos + 1]; *pos += 2;
    if (count > length - *pos) return false;
    if (output) {
        if (count >= capacity || !debug_utf8(data + *pos, count)) return false;
        memcpy(output, data + *pos, count); output[count] = 0;
    }
    *pos += count;
    return true;
}

void mqtt_debug_packet(uint64_t owner, uint64_t epoch, uint64_t message,
    const char *origin, const char *stage, const uint8_t *data, size_t length)
{
    if (!mqtt_debug_active(owner)) return;
    cJSON *details = cJSON_CreateObject();
    if (!details) return;
    cJSON_AddNumberToObject(details, "length", (double)length);
    if (!data || length < 2) { cJSON_AddStringToObject(details, "omitted", "incomplete_packet"); goto done; }
    unsigned type = data[0] >> 4, qos = (data[0] >> 1) & 3;
    cJSON_AddNumberToObject(details, "packetType", type);
    static const char *names[] = {"INVALID", "CONNECT", "CONNACK", "PUBLISH", "PUBACK",
        "PUBREC", "PUBREL", "PUBCOMP", "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK",
        "PINGREQ", "PINGRESP", "DISCONNECT"};
    cJSON_AddStringToObject(details, "packetName", type < sizeof(names) / sizeof(names[0]) ? names[type] : "UNKNOWN");
    if (type == 3) {
        cJSON_AddBoolToObject(details, "dup", (data[0] & 8) != 0);
        cJSON_AddBoolToObject(details, "retain", (data[0] & 1) != 0);
        cJSON_AddNumberToObject(details, "qos", qos);
    }
    size_t pos = 1, remaining = 0, multiplier = 1;
    for (unsigned i = 0; ; i++) {
        if (i == 4 || pos >= length) goto invalid;
        uint8_t byte = data[pos++]; remaining += (byte & 127U) * multiplier; multiplier *= 128U;
        if (!(byte & 128)) break;
    }
    if (remaining != length - pos) goto invalid;
    if (type == 1) {
        char protocol[16], client[256];
        if (!debug_packet_string(data, length, &pos, protocol, sizeof(protocol)) || length - pos < 4) goto invalid;
        cJSON_AddStringToObject(details, "protocol", protocol);
        cJSON_AddNumberToObject(details, "protocolLevel", data[pos++]);
        uint8_t flags = data[pos++];
        cJSON_AddBoolToObject(details, "cleanSession", (flags & 2) != 0);
        cJSON_AddNumberToObject(details, "keepaliveSeconds", ((unsigned)data[pos] << 8) | data[pos + 1]); pos += 2;
        if (!debug_packet_string(data, length, &pos, client, sizeof(client))) goto invalid;
        cJSON_AddStringToObject(details, "clientId", client);
        cJSON_AddStringToObject(details, "identityEvidence", "client_id_is_not_box_identity");
        if (flags & 4) {
            /* Will content can contain credentials, so preserve only its presence. */
            if (!debug_packet_string(data, length, &pos, NULL, 0) ||
                !debug_packet_string(data, length, &pos, NULL, 0)) goto invalid;
            cJSON_AddStringToObject(details, "willPayload", "[omitted: connect_will_content]");
        }
        if (flags & 0x80) {
            if (!debug_packet_string(data, length, &pos, NULL, 0)) goto invalid;
            cJSON_AddStringToObject(details, "username", "[redacted]");
        }
        if (flags & 0x40) {
            if (!debug_packet_string(data, length, &pos, NULL, 0)) goto invalid;
            cJSON_AddStringToObject(details, "password", "[redacted]");
        }
        if (pos != length) goto invalid;
        goto done;
    }
    if (type == 2) {
        if (remaining != 2) goto invalid;
        cJSON_AddBoolToObject(details, "sessionPresent", (data[pos] & 1) != 0);
        cJSON_AddNumberToObject(details, "returnCode", data[pos + 1]);
        goto done;
    }
    if (type == 3) {
        if (qos == 3) goto invalid;
        char topic[256];
        if (!debug_packet_string(data, length, &pos, topic, sizeof(topic)) || !topic[0]) goto invalid;
        cJSON_AddStringToObject(details, "topic", topic);
        if (qos) { if (length - pos < 2) goto invalid; cJSON_AddNumberToObject(details, "packetId", ((unsigned)data[pos] << 8) | data[pos+1]); pos += 2; }
        size_t n = length - pos;
        if (n && n <= DEBUG_EVENT_BYTES / 2 && debug_utf8(data + pos, n)) {
            char *text = malloc(n + 1); if (!text) goto done;
            memcpy(text, data + pos, n); text[n] = 0;
            cJSON *payload = cJSON_ParseWithOpts(text, NULL, true);
            if (payload && (cJSON_IsObject(payload) || cJSON_IsArray(payload))) cJSON_AddItemToObject(details, "payload", payload);
            else {
                const char *first = text;
                while (*first && isspace((unsigned char)*first)) first++;
                cJSON_Delete(payload);
                if (*first == '{' || *first == '[')
                    cJSON_AddStringToObject(details, "omitted", "unparseable_json_payload");
                else
                    cJSON_AddStringToObject(details, "payloadText", text);
            }
            free(text);
        } else cJSON_AddStringToObject(details, "omitted", n ? "binary_or_oversized_payload" : "empty_payload");
    } else if (type >= 4 && type <= 11 && remaining >= 2) {
        cJSON_AddNumberToObject(details, "packetId", ((unsigned)data[pos] << 8) | data[pos+1]);
        pos += 2;
        if (type == 8 || type == 10) {
            cJSON *topics = cJSON_AddArrayToObject(details, "subscriptions");
            unsigned count = 0;
            while (pos < length && count++ < DEBUG_PACKET_ITEMS) {
                char topic[256];
                if (!debug_packet_string(data, length, &pos, topic, sizeof(topic)) || !topic[0]) goto invalid;
                cJSON *entry = cJSON_CreateObject();
                if (!entry || !topics) { cJSON_Delete(entry); goto done; }
                cJSON_AddStringToObject(entry, "topic", topic);
                if (type == 8) {
                    if (pos == length) { cJSON_Delete(entry); goto invalid; }
                    cJSON_AddNumberToObject(entry, "qos", data[pos++]);
                }
                cJSON_AddItemToArray(topics, entry);
            }
            if (pos < length) cJSON_AddStringToObject(details, "omitted", "subscription_list_limit");
        } else if (type == 9) {
            cJSON *codes = cJSON_AddArrayToObject(details, "returnCodes");
            size_t count = MIN(length - pos, DEBUG_PACKET_ITEMS);
            for (size_t i = 0; codes && i < count; i++)
                cJSON_AddItemToArray(codes, cJSON_CreateNumber(data[pos++]));
            if (pos < length) cJSON_AddStringToObject(details, "omitted", "subscription_list_limit");
        }
    }
    goto done;
invalid:
    cJSON_AddStringToObject(details, "omitted", "malformed_packet");
done:
    mqtt_debug_event(owner, epoch, message, origin, stage, details);
}

/* Enumeration stays beneath a pinned directory. False means an OS error or a
 * scan cap, never a claim that the directory was completely visited. */
static bool debug_list_directory(debug_directory_t *dir,
    bool (*callback)(const char *name, void *context), void *context)
{
    size_t count = 0;
    bool complete = true;
#ifdef _WIN32
    char pattern[DEBUG_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s/*", dir->path) >= (int)sizeof(pattern)) return false;
    WIN32_FIND_DATAA entry;
    HANDLE scan = FindFirstFileA(pattern, &entry);
    if (scan == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool stopped = false;
    do {
        const char *name = entry.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (++count > DEBUG_SCAN_MAX) { complete = false; break; }
        if (!callback(name, context)) { stopped = true; break; }
    } while (FindNextFileA(scan, &entry));
    if (!stopped && GetLastError() != ERROR_NO_MORE_FILES && count <= DEBUG_SCAN_MAX) complete = false;
    FindClose(scan);
#else
    int copy = dup(dir->fd);
    if (copy < 0) return false;
    DIR *scan = fdopendir(copy);
    if (!scan) { close(copy); return false; }
    rewinddir(scan);
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(scan);
        if (!entry) { if (errno) complete = false; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (++count > DEBUG_SCAN_MAX) { complete = false; break; }
        if (!callback(name, context)) break;
    }
    closedir(scan);
#endif
    return complete;
}

static bool debug_export_seek(FILE *file, uint64_t offset)
{
    if (offset > INT64_MAX) return false;
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static bool debug_export_size(FILE *file, uint64_t *size)
{
#ifdef _WIN32
    struct _stat64 info;
    if (_fstat64(_fileno(file), &info) || info.st_size < 0) return false;
#else
    struct stat info;
    if (fstat(fileno(file), &info) || info.st_size < 0) return false;
#endif
    *size = (uint64_t)info.st_size;
    return true;
}

/* Only whole records are exported, including after an unclean process exit. */
static bool debug_export_jsonl_size(FILE *file, uint64_t *size)
{
    unsigned char tail[1024];
    uint64_t cursor = *size, scanned = 0;
    while (cursor && scanned <= DEBUG_EVENT_BYTES) {
        size_t count = (size_t)MIN(cursor, sizeof(tail));
        if (!debug_export_seek(file, cursor - count) || fread(tail, 1, count, file) != count) return false;
        for (size_t i = count; i > 0; i--) {
            if (tail[i - 1] == '\n') { *size = cursor - count + i; return debug_export_seek(file, 0); }
        }
        cursor -= count;
        scanned += count;
    }
    if (cursor) return false;
    *size = 0;
    return debug_export_seek(file, 0);
}

static cJSON *debug_export_metadata(const char *box, const char *session)
{
    char path[DEBUG_PATH];
    if (!debug_path(path, box, session, "session.json")) return NULL;
    FILE *file = debug_file(path, false, false);
    uint64_t size = 0;
    if (!file) return NULL;
    if (!debug_export_size(file, &size) || !size || size > DEBUG_EVENT_BYTES) { fclose(file); return NULL; }
    char *text = malloc((size_t)size + 1);
    if (!text) { fclose(file); return NULL; }
    bool read = fread(text, 1, (size_t)size, file) == size;
    fclose(file);
    text[size] = 0;
    cJSON *json = read ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    free(text);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    cJSON *owner = cJSON_GetObjectItemCaseSensitive(json, "box");
    if (!cJSON_IsObject(json) || !cJSON_IsString(id) || strcmp(id->valuestring, session) ||
        !cJSON_IsString(owner) || strcmp(owner->valuestring, box)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

error_t mqtt_debug_file_open(settings_t *settings, const char *session,
    const char *name, mqtt_debug_file_t **output, uint64_t *length)
{
    if (!output || !length) return ERROR_INVALID_PARAMETER;
    *output = NULL; *length = 0;
    char box[13], path[DEBUG_PATH];
    if (!debug.ready || !settings || settings->toniebox.boxGeneration != GENERATION_TB2 ||
        !debug_box(settings, box) || !debug_session_name(session) || !debug_filename(name) ||
        !debug_path(path, box, session, name)) return ERROR_NOT_FOUND;
    osAcquireMutex(&debug.disk);
    cJSON *metadata = debug_export_metadata(box, session);
    FILE *stream = metadata ? debug_file(path, false, false) : NULL;
    cJSON_Delete(metadata);
    mqtt_debug_file_t *file = stream ? calloc(1, sizeof(*file)) : NULL;
    if (!file) {
        if (stream) fclose(stream);
        osReleaseMutex(&debug.disk);
        return stream ? ERROR_OUT_OF_MEMORY : ERROR_NOT_FOUND;
    }
    file->stream = stream; file->jsonl = debug_segment_name(name);
    bool valid = debug_export_size(stream, &file->size) &&
        (!file->jsonl || debug_export_jsonl_size(stream, &file->size));
    osReleaseMutex(&debug.disk);
    if (!valid) { fclose(stream); free(file); return ERROR_FAILURE; }
    file->remaining = file->size;
    *length = file->size; *output = file;
    return NO_ERROR;
}

error_t mqtt_debug_file_limit(mqtt_debug_file_t *file, uint64_t length)
{
    if (!file || length > file->size || file->remaining != file->size) return ERROR_INVALID_LENGTH;
    if (!file->jsonl && length != file->size) return ERROR_INVALID_LENGTH;
    if (file->jsonl && length) {
        if (!debug_export_seek(file->stream, length - 1) || fgetc(file->stream) != '\n')
            return ERROR_INVALID_LENGTH;
    }
    if (!debug_export_seek(file->stream, 0)) return ERROR_FAILURE;
    file->remaining = length;
    return NO_ERROR;
}

error_t mqtt_debug_file_read(mqtt_debug_file_t *file, void *buffer, size_t length, size_t *received)
{
    if (!file || !buffer || !received) return ERROR_INVALID_PARAMETER;
    *received = 0;
    size_t count = (size_t)MIN((uint64_t)length, file->remaining);
    if (!count) return NO_ERROR;
    *received = fread(buffer, 1, count, file->stream);
    file->remaining -= *received;
    return *received == count ? NO_ERROR : ERROR_UNEXPECTED_END_OF_FILE;
}

void mqtt_debug_file_close(mqtt_debug_file_t *file)
{
    if (!file) return;
    fclose(file->stream); free(file);
}

typedef struct {
    char (*names)[64];
    size_t count, limit, omitted;
    bool sessions;
} debug_export_names_t;

/* Session IDs start with wall time: keep newest sessions, and stable ascending
 * file names inside them. No allocation depends on directory contents. */
static bool debug_export_collect(const char *name, void *context)
{
    debug_export_names_t *list = context;
    if (!(list->sessions ? debug_session_name(name) : debug_filename(name))) return true;
    size_t position = 0;
    while (position < list->count) {
        int compare = strcmp(name, list->names[position]);
        if ((list->sessions && compare > 0) || (!list->sessions && compare < 0)) break;
        position++;
    }
    if (list->count == list->limit) list->omitted++;
    if (position >= list->limit) return true;
    if (list->count < list->limit) list->count++;
    if (position + 1 < list->count)
        memmove(list->names[position + 1], list->names[position], (list->count - position - 1) * sizeof(list->names[0]));
    snprintf(list->names[position], sizeof(list->names[0]), "%s", name);
    return true;
}

static uint64_t debug_export_counter(cJSON *json, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 9007199254740991.0 ?
        (uint64_t)value->valuedouble : 0;
}

static void debug_export_started(uint64_t milliseconds, char output[32])
{
    time_t seconds = (time_t)(milliseconds / 1000U);
    struct tm utc;
    memset(&utc, 0, sizeof(utc));
#ifdef _WIN32
    if (gmtime_s(&utc, &seconds)) { output[0] = 0; return; }
#else
    if (!gmtime_r(&seconds, &utc)) { output[0] = 0; return; }
#endif
    strftime(output, 32, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static bool debug_export_directory_missing(void)
{
#ifdef _WIN32
    DWORD code = GetLastError();
    return code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND;
#else
    return errno == ENOENT;
#endif
}

cJSON *mqtt_debug_status(settings_t *settings)
{
    cJSON *status = cJSON_CreateObject();
    if (!status) return NULL;
    cJSON *sessions = cJSON_AddArrayToObject(status, "sessions");
    cJSON *unassigned = cJSON_AddArrayToObject(status, "recentUnassigned");
    if (!sessions || !unassigned) { cJSON_Delete(status); return NULL; }
    char box[13] = {0}, error[96] = {0};
    bool enabled = mqtt_debug_enabled(settings), recording = false;
    bool valid_box = settings && settings->toniebox.boxGeneration == GENERATION_TB2 && debug_box(settings, box);
    uint64_t bytes = 0, dropped = 0, unassigned_dropped = 0;
    size_t omitted_sessions = 0, omitted_files = 0, listed_files = 0;
    bool incomplete = false;
    debug_session_t *runtime = calloc(DEBUG_SLOTS, sizeof(*runtime));
    char (*session_names)[64] = calloc(DEBUG_LIST_MAX, sizeof(*session_names));
    char (*file_names)[64] = calloc(DEBUG_FILES_MAX, sizeof(*file_names));
    if (!runtime || !session_names || !file_names) {
        free(runtime); free(session_names); free(file_names); cJSON_Delete(status); return NULL;
    }
    if (!valid_box) snprintf(error, sizeof(error), "unknown_box");
    else if (!debug.ready) {
        if (enabled) snprintf(error, sizeof(error), "writer_unavailable");
    } else {
        /* Writer lock order is disk -> state. The state snapshot ends before
         * any directory or file access; MQTT producers never wait for disk. */
        osAcquireMutex(&debug.disk);
        osAcquireMutex(&debug.mutex);
        memcpy(runtime, debug.sessions, DEBUG_SLOTS * sizeof(*runtime));
        snprintf(error, sizeof(error), "%s", debug.error);
        unassigned_dropped = debug.prelude_dropped;
        for (size_t i = 0; i < debug.prelude_count && i < DEBUG_PRELUDE; i++) {
            cJSON *event = cJSON_Duplicate(debug.prelude[i], true);
            if (!event) { incomplete = true; break; }
            cJSON_AddItemToArray(unassigned, event);
        }
        osReleaseMutex(&debug.mutex);
        for (size_t i = 0; i < DEBUG_SLOTS; i++) {
            if (!runtime[i].used || strcmp(runtime[i].box, box)) continue;
            if (runtime[i].accepting) recording = true;
            dropped += runtime[i].dropped;
            if (runtime[i].error[0]) snprintf(error, sizeof(error), "%s", runtime[i].error);
        }
        char path[DEBUG_PATH];
        debug_directory_t directory;
        if (!debug_path(path, box, "", NULL)) {
            snprintf(error, sizeof(error), "invalid_debug_directory");
        } else if (!debug_directory_open(&directory, path, false)) {
            if (!debug_export_directory_missing()) snprintf(error, sizeof(error), "diagnostic_directory_unavailable");
        } else {
            debug_export_names_t names = {session_names, 0, DEBUG_LIST_MAX, 0, true};
            if (!debug_list_directory(&directory, debug_export_collect, &names)) incomplete = true;
            debug_directory_close(&directory);
            omitted_sessions = names.omitted;
            for (size_t n = 0; n < names.count; n++) {
                const char *id = session_names[n];
                cJSON *metadata = debug_export_metadata(box, id);
                if (!metadata) { omitted_sessions++; incomplete = true; continue; }
                debug_session_t *current = NULL;
                for (size_t i = 0; i < DEBUG_SLOTS; i++)
                    if (runtime[i].used && !strcmp(runtime[i].box, box) && !strcmp(runtime[i].id, id)) { current = &runtime[i]; break; }
                cJSON *entry = cJSON_CreateObject();
                cJSON *files = entry ? cJSON_AddArrayToObject(entry, "files") : NULL;
                if (!files) { cJSON_Delete(entry); cJSON_Delete(metadata); incomplete = true; omitted_sessions++; break; }
                char started[32];
                debug_export_started(current ? current->started : debug_export_counter(metadata, "startedAt"), started);
                cJSON_AddStringToObject(entry, "id", id);
                cJSON_AddStringToObject(entry, "startedAt", started);
                cJSON_AddBoolToObject(entry, "active", current != NULL && !current->finished);
                bool truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(metadata, "truncated")) ||
                    (current && current->truncated);
                /* A persisted active flag without a live writer identifies an
                 * interrupted recording, not an eternally active connection. */
                if (!current && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(metadata, "active"))) {
                    truncated = true;
                    cJSON_AddStringToObject(entry, "gap", "unclean_process_exit");
                }
                if (!current) dropped += debug_export_counter(metadata, "droppedEvents");
                cJSON *session_error = cJSON_GetObjectItemCaseSensitive(metadata, "error");
                if (cJSON_IsString(session_error) && session_error->valuestring[0]) {
                    cJSON_AddStringToObject(entry, "error", session_error->valuestring);
                    if (n == 0 && !recording && !error[0])
                        snprintf(error, sizeof(error), "%s", session_error->valuestring);
                }
                cJSON *gaps = cJSON_GetObjectItemCaseSensitive(metadata, "retentionGaps");
                if (gaps) cJSON_AddItemToObject(entry, "retentionGaps", cJSON_Duplicate(gaps, true));
                debug_export_names_t file_list = {file_names, 0, DEBUG_FILES_MAX - listed_files, 0, false};
                if (!debug_path(path, box, id, NULL) || !debug_directory_open(&directory, path, false)) {
                    incomplete = truncated = true;
                } else {
                    if (!debug_list_directory(&directory, debug_export_collect, &file_list)) incomplete = truncated = true;
                    debug_directory_close(&directory);
                    for (size_t f = 0; f < file_list.count; f++) {
                        if (!debug_path(path, box, id, file_names[f])) { file_list.omitted++; continue; }
                        FILE *file = debug_file(path, false, false);
                        uint64_t size = 0;
                        bool valid = file && debug_export_size(file, &size) &&
                            (!debug_segment_name(file_names[f]) || debug_export_jsonl_size(file, &size));
                        if (file) fclose(file);
                        if (!valid) { file_list.omitted++; incomplete = truncated = true; continue; }
                        cJSON *item = cJSON_CreateObject();
                        if (!item) { file_list.omitted++; incomplete = truncated = true; continue; }
                        cJSON_AddStringToObject(item, "name", file_names[f]);
                        cJSON_AddNumberToObject(item, "size", (double)size);
                        cJSON_AddItemToArray(files, item);
                        bytes += size; listed_files++;
                    }
                }
                if (file_list.omitted) {
                    omitted_files += file_list.omitted;
                    truncated = true;
                    cJSON_AddNumberToObject(entry, "omittedFiles", (double)file_list.omitted);
                }
                cJSON_AddBoolToObject(entry, "truncated", truncated);
                cJSON_AddItemToArray(sessions, entry);
                cJSON_Delete(metadata);
            }
        }
        osReleaseMutex(&debug.disk);
    }
    if ((omitted_sessions || omitted_files || incomplete) && !error[0])
        snprintf(error, sizeof(error), "diagnostic_listing_incomplete");
    cJSON_AddBoolToObject(status, "enabled", enabled);
    cJSON_AddStringToObject(status, "state", enabled ? (error[0] ? "error" : recording ? "recording" : "armed") : "disabled");
    cJSON_AddStringToObject(status, "error", error);
    cJSON_AddNumberToObject(status, "bytes", (double)bytes);
    cJSON_AddNumberToObject(status, "droppedEvents", (double)dropped);
    cJSON_AddNumberToObject(status, "unassignedDroppedEvents", (double)unassigned_dropped);
    cJSON_AddNumberToObject(status, "omittedSessions", (double)omitted_sessions);
    cJSON_AddNumberToObject(status, "omittedFiles", (double)omitted_files);
    cJSON_AddBoolToObject(status, "listingIncomplete", incomplete || omitted_sessions || omitted_files);
    free(runtime); free(session_names); free(file_names);
    return status;
}
