#define TRACE_LEVEL TRACE_LEVEL_INFO

#include "encoder_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "encoder_limits.h"
#include "fs_ext.h"
#include "mqtt_time.h"
#include "mutex_manager.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "rand.h"
#include "server_helpers.h"
#include "v3_native_cache.h"

#define ENCODER_UPLOAD_PREFIX "/api/library/native/import/"
#define ENCODER_UPLOAD_ID_BYTES 16U
#define ENCODER_UPLOAD_ID_LENGTH (ENCODER_UPLOAD_ID_BYTES * 2U)
#define ENCODER_IO_BYTES 8192U
#define ENCODER_OGG_PACKET_LIMIT (64U * 1024U)
#define ENCODER_OPUS_RATE 48000U

typedef struct
{
    bool_t used;
    bool_t busy;
    char id[ENCODER_UPLOAD_ID_LENGTH + 1U];
    char *library_root;
    char *directory;
    uint32_t last_activity;
    size_t received;
    v3_native_library_collection_t collection;
} encoder_upload_t;

static encoder_upload_t encoder_uploads[ENCODER_UPLOAD_SLOTS];

static error_t encoder_json_reply(HttpConnection *connection, uint_t status, cJSON *json)
{
    char *body = json != NULL ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (body == NULL)
        return ERROR_OUT_OF_MEMORY;
    httpPrepareHeader(connection, "application/json; charset=utf-8", osStrlen(body));
    connection->response.statusCode = status;
    if (status >= 400)
        connection->response.keepAlive = FALSE; /* An early refusal may leave unread input. */
    return httpWriteResponse(connection, body, osStrlen(body), TRUE);
}

static error_t encoder_failure(HttpConnection *connection, uint_t status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    if (json != NULL)
    {
        cJSON_AddBoolToObject(json, "ok", FALSE);
        cJSON_AddStringToObject(json, "error", message);
    }
    return encoder_json_reply(connection, status, json);
}

static cJSON *encoder_read_json(HttpConnection *connection)
{
    size_t expected = connection->request.byteCount;
    if (expected == 0 || expected > ENCODER_MAX_METADATA_BYTES)
        return NULL;
    char *body = osAllocMem(expected + 1U);
    if (body == NULL)
        return NULL;
    size_t received = 0;
    while (received < expected)
    {
        size_t count = 0;
        error_t error = httpReadStream(connection, body + received, expected - received, &count, 0);
        if (error != NO_ERROR || count == 0 || count > expected - received)
            break;
        received += count;
    }
    body[received] = '\0';
    const char *end = NULL;
    cJSON *json = received == expected ? cJSON_ParseWithLengthOpts(body, received, &end, FALSE) : NULL;
    while (end != NULL && end < body + received && isspace((unsigned char)*end))
        end++;
    if (!cJSON_IsObject(json) || end != body + received)
    {
        cJSON_Delete(json);
        json = NULL;
    }
    osFreeMem(body);
    return json;
}

static bool_t encoder_id_valid(const char *id)
{
    if (id == NULL || osStrlen(id) != ENCODER_UPLOAD_ID_LENGTH)
        return FALSE;
    for (size_t i = 0; i < ENCODER_UPLOAD_ID_LENGTH; i++)
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f'))
            return FALSE;
    return TRUE;
}

/** This directory only contains server-generated names; never accepts a user path. */
static error_t encoder_remove_directory(const char *path)
{
    if (path == NULL || !fsDirExists(path))
        return NO_ERROR;
    FsDir *dir = fsOpenDir(path);
    if (dir == NULL)
        return ERROR_FILE_OPENING_FAILED;
    error_t error = NO_ERROR;
    FsDirEntry entry;
    while (error == NO_ERROR && fsReadDir(dir, &entry) == NO_ERROR)
    {
        if (!osStrcmp(entry.name, ".") || !osStrcmp(entry.name, ".."))
            continue;
        char *child = custom_asprintf("%s%c%s", path, PATH_SEPARATOR, entry.name);
        if (child == NULL)
            error = ERROR_OUT_OF_MEMORY;
        else if (entry.attributes & FS_FILE_ATTR_DIRECTORY)
            error = encoder_remove_directory(child);
        else
            error = fsDeleteFile(child);
        osFreeMem(child);
    }
    fsCloseDir(dir);
    return error == NO_ERROR ? fsRemoveDir(path) : error;
}

/** The caller owns the busy slot. No file I/O while holding the registry lock. */
static void encoder_discard(encoder_upload_t *upload)
{
    error_t error = encoder_remove_directory(upload->directory);
    if (error != NO_ERROR)
        TRACE_WARNING("Encoder upload cleanup failed: %d\r\n", error);
    osFreeMem(upload->directory);
    osFreeMem(upload->library_root);
    v3_native_library_collection_free(&upload->collection);
    mutex_lock(MUTEX_ENCODER_UPLOAD);
    osMemset(upload, 0, sizeof(*upload));
    mutex_unlock(MUTEX_ENCODER_UPLOAD);
}

static void encoder_expire_uploads(uint32_t now)
{
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++)
    {
        mutex_lock(MUTEX_ENCODER_UPLOAD);
        encoder_upload_t *upload = &encoder_uploads[i];
        bool_t expired = upload->used && !upload->busy &&
            (uint32_t)(now - upload->last_activity) >= ENCODER_UPLOAD_IDLE_SECONDS * 1000U;
        if (expired)
            upload->busy = TRUE;
        mutex_unlock(MUTEX_ENCODER_UPLOAD);
        if (expired)
            encoder_discard(upload);
    }
}

void encoder_uploads_init(void)
{
    char *root = custom_asprintf("%s%c%s", get_settings()->internal.librarydirfull,
        PATH_SEPARATOR, ENCODER_UPLOAD_DIRECTORY);
    FsDir *directory = root != NULL ? fsOpenDir(root) : NULL;
    if (directory != NULL)
    {
        FsDirEntry entry;
        while (fsReadDir(directory, &entry) == NO_ERROR)
        {
            if (!(entry.attributes & FS_FILE_ATTR_DIRECTORY) || !encoder_id_valid(entry.name))
                continue;
            char *path = custom_asprintf("%s%c%s", root, PATH_SEPARATOR, entry.name);
            if (path != NULL && encoder_remove_directory(path) != NO_ERROR)
                TRACE_WARNING("Could not remove abandoned encoder upload\r\n");
            osFreeMem(path);
        }
        fsCloseDir(directory);
    }
    osFreeMem(root);
}

static uint32_t encoder_le32(const unsigned char *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static bool_t encoder_valid_tags(const ogg_packet *packet)
{
    if (packet->bytes < 16 || osMemcmp(packet->packet, "OpusTags", 8))
        return FALSE;
    size_t position = 12;
    uint32_t vendor_length = encoder_le32(packet->packet + 8);
    if (vendor_length > (size_t)packet->bytes - position - 4U)
        return FALSE;
    position += vendor_length;
    uint32_t count = encoder_le32(packet->packet + position);
    position += 4;
    if (count > ((size_t)packet->bytes - position) / 4U)
        return FALSE;
    for (uint32_t i = 0; i < count; i++)
    {
        if (position + 4U > (size_t)packet->bytes)
            return FALSE;
        uint32_t length = encoder_le32(packet->packet + position);
        position += 4;
        if (length > (size_t)packet->bytes - position)
            return FALSE;
        position += length;
    }
    return TRUE;
}

error_t encoder_validate_opus_file(const char *path, double *duration_seconds)
{
    if (path == NULL || duration_seconds == NULL)
        return ERROR_INVALID_PARAMETER;
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL)
        return ERROR_FILE_OPENING_FAILED;
    ogg_sync_state sync;
    ogg_stream_state stream;
    osMemset(&stream, 0, sizeof(stream));
    if (ogg_sync_init(&sync) != 0)
    {
        fsCloseFile(file);
        return ERROR_OUT_OF_MEMORY;
    }
    bool_t initialized = FALSE;
    bool_t eos = FALSE;
    uint32_t page_number = 0;
    size_t packet_number = 0;
    size_t pending_packet_bytes = 0;
    uint64_t decoded_samples = 0;
    uint32_t preskip = 0;
    ogg_int64_t final_granule = -1;
    ogg_int64_t previous_granule = 0;
    error_t error = NO_ERROR;
    for (;;)
    {
        char *buffer = ogg_sync_buffer(&sync, ENCODER_IO_BYTES);
        size_t received = 0;
        if (buffer == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
            break;
        }
        error = fsReadFile(file, buffer, ENCODER_IO_BYTES, &received);
        if (error == ERROR_END_OF_FILE && received == 0)
            error = NO_ERROR;
        if (error != NO_ERROR || ogg_sync_wrote(&sync, (long)received) != 0)
        {
            if (error == NO_ERROR)
                error = ERROR_INVALID_FILE;
            break;
        }
        ogg_page page;
        int page_result;
        while ((page_result = ogg_sync_pageout(&sync, &page)) > 0)
        {
            if (eos || ogg_page_version(&page) != 0 ||
                (uint32_t)ogg_page_pageno(&page) != page_number ||
                (page_number == 0 ? !ogg_page_bos(&page) : ogg_page_bos(&page)))
            {
                error = ERROR_INVALID_FILE;
                break;
            }
            if (!initialized)
            {
                if (ogg_stream_init(&stream, ogg_page_serialno(&page)) != 0)
                {
                    error = ERROR_OUT_OF_MEMORY;
                    break;
                }
                initialized = TRUE;
            }
            pending_packet_bytes += (size_t)page.body_len;
            if (pending_packet_bytes > 2U * ENCODER_OGG_PACKET_LIMIT ||
                ogg_stream_pagein(&stream, &page) != 0)
            {
                error = ERROR_INVALID_FILE;
                break;
            }
            page_number++;
            ogg_packet packet;
            int packet_result;
            while ((packet_result = ogg_stream_packetout(&stream, &packet)) > 0)
            {
                if (packet.bytes <= 0 || (size_t)packet.bytes > ENCODER_OGG_PACKET_LIMIT ||
                    (size_t)packet.bytes > pending_packet_bytes)
                {
                    error = ERROR_INVALID_FILE;
                    break;
                }
                pending_packet_bytes -= (size_t)packet.bytes;
                if (packet_number == 0)
                {
                    if (packet.bytes != 19 || osMemcmp(packet.packet, "OpusHead", 8) ||
                        packet.packet[8] != 1 || packet.packet[9] < 1 ||
                        packet.packet[9] > 2 || packet.packet[18] != 0)
                    {
                        error = ERROR_INVALID_FILE;
                        break;
                    }
                    preskip = packet.packet[10] | ((uint32_t)packet.packet[11] << 8);
                }
                else if (packet_number == 1)
                {
                    if (!encoder_valid_tags(&packet))
                    {
                        error = ERROR_INVALID_FILE;
                        break;
                    }
                }
                else
                {
                    const unsigned char *frames[48];
                    opus_int16 sizes[48];
                    unsigned char toc;
                    int payload_offset = 0;
                    int samples = opus_packet_get_nb_samples(packet.packet,
                        (opus_int32)packet.bytes, ENCODER_OPUS_RATE);
                    if (samples <= 0 || opus_packet_parse(packet.packet,
                        (opus_int32)packet.bytes, &toc, frames, sizes, &payload_offset) <= 0)
                    {
                        error = ERROR_INVALID_FILE;
                        break;
                    }
                    decoded_samples += (uint32_t)samples;
                }
                packet_number++;
            }
            if (packet_result < 0)
                error = ERROR_INVALID_FILE;
            if (error != NO_ERROR)
                break;
            ogg_int64_t granule = ogg_page_granulepos(&page);
            if (granule >= 0)
            {
                if (granule < previous_granule || (uint64_t)granule > decoded_samples)
                {
                    error = ERROR_INVALID_FILE;
                    break;
                }
                previous_granule = granule;
            }
            if (ogg_page_eos(&page))
            {
                eos = TRUE;
                final_granule = ogg_page_granulepos(&page);
            }
        }
        if (page_result < 0)
            error = ERROR_INVALID_FILE;
        if (error != NO_ERROR || received == 0)
            break;
    }
    if (error == NO_ERROR && (!eos || packet_number < 3 || pending_packet_bytes != 0 ||
        sync.fill != sync.returned || final_granule <= (ogg_int64_t)preskip ||
        (uint64_t)final_granule > decoded_samples))
        error = ERROR_INVALID_FILE;
    if (error == NO_ERROR)
        *duration_seconds = (double)((uint64_t)final_granule - preskip) / ENCODER_OPUS_RATE;
    if (initialized)
        ogg_stream_clear(&stream);
    ogg_sync_clear(&sync);
    fsCloseFile(file);
    return error;
}

error_t handleApiEncoderCapabilities(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx)
{
    (void)uri; (void)query; (void)client_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *formats = root != NULL ? cJSON_AddObjectToObject(root, "formats") : NULL;
    cJSON *taf = formats != NULL ? cJSON_AddObjectToObject(formats, "taf") : NULL;
    cJSON *native = formats != NULL ? cJSON_AddObjectToObject(formats, "native") : NULL;
    if (root == NULL || formats == NULL || taf == NULL || native == NULL)
    {
        cJSON_Delete(root);
        return ERROR_OUT_OF_MEMORY;
    }
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "maxPcmFrames", ENCODER_PCM_FRAMES);
    cJSON_AddBoolToObject(taf, "enabled", TRUE);
    cJSON_AddNumberToObject(taf, "maxFiles", ENCODER_TAF_MAX_CHAPTERS);
    cJSON_AddNumberToObject(taf, "maxUploadBytes", ENCODER_MAX_TAF_UPLOAD_BYTES);
    cJSON_AddNumberToObject(taf, "headerBytes", ENCODER_TAF_PROTO_BYTES);
    cJSON_AddBoolToObject(native, "enabled", TRUE);
    cJSON_AddNullToObject(native, "maxFiles");
    cJSON_AddNumberToObject(native, "maxChapterBytes", ENCODER_MAX_FILE_BYTES);
    cJSON_AddNumberToObject(native, "maxMetadataBytes", ENCODER_MAX_METADATA_BYTES);
    return encoder_json_reply(connection, 200, root);
}

error_t handleApiNativeImportStart(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx)
{
    (void)uri; (void)query; (void)client_ctx;
    uint32_t now = 0;
    if (!mqtt_monotonic_ms(&now))
        return encoder_failure(connection, 503, "Monotonic clock unavailable");
    encoder_expire_uploads(now);
    cJSON *json = encoder_read_json(connection);
    cJSON *title = cJSON_GetObjectItemCaseSensitive(json, "title");
    cJSON *chapters = cJSON_GetObjectItemCaseSensitive(json, "chapters");
    int count = cJSON_IsArray(chapters) ? cJSON_GetArraySize(chapters) : 0;
    if (!cJSON_IsString(title) || title->valuestring == NULL || count <= 0 ||
        (size_t)count > ENCODER_MAX_METADATA_BYTES / (2U * V3_NATIVE_LIBRARY_HASH_HEX_LENGTH))
    {
        cJSON_Delete(json);
        return encoder_failure(connection, 400, "Invalid or oversized native collection metadata");
    }
    encoder_upload_t *upload = NULL;
    mutex_lock(MUTEX_ENCODER_UPLOAD);
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++)
    {
        if (!encoder_uploads[i].used)
        {
            upload = &encoder_uploads[i];
            upload->used = TRUE;
            upload->busy = TRUE;
            break;
        }
    }
    mutex_unlock(MUTEX_ENCODER_UPLOAD);
    if (upload == NULL)
    {
        cJSON_Delete(json);
        return encoder_failure(connection, 503, "All native import slots are busy");
    }
    upload->collection.title = strdup(title->valuestring);
    upload->collection.chapters = osAllocMem((size_t)count * sizeof(*upload->collection.chapters));
    if (upload->collection.chapters != NULL)
    {
        osMemset(upload->collection.chapters, 0, (size_t)count * sizeof(*upload->collection.chapters));
        upload->collection.chapter_count = (size_t)count;
    }
    error_t error = upload->collection.title == NULL || upload->collection.chapters == NULL
        ? ERROR_OUT_OF_MEMORY : NO_ERROR;
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, chapters)
    {
        if (error != NO_ERROR)
            break;
        cJSON *chapter_title = cJSON_GetObjectItemCaseSensitive(item, "title");
        if (!cJSON_IsObject(item) || !cJSON_IsString(chapter_title) || chapter_title->valuestring == NULL)
        {
            error = ERROR_INVALID_PARAMETER;
            break;
        }
        upload->collection.chapters[index].index = index;
        upload->collection.chapters[index].title = strdup(chapter_title->valuestring);
        if (upload->collection.chapters[index].title == NULL)
            error = ERROR_OUT_OF_MEMORY;
        index++;
    }
    cJSON_Delete(json);
    if (error == NO_ERROR && !v3_native_library_custom_metadata_fits(&upload->collection))
        error = ERROR_BUFFER_OVERFLOW;
    uint8_t nonce[ENCODER_UPLOAD_ID_BYTES];
    if (error == NO_ERROR)
        error = rand_get_algo()->read(rand_get_context(), nonce, sizeof(nonce));
    if (error == NO_ERROR)
    {
        mutex_lock(MUTEX_ENCODER_UPLOAD);
        for (size_t i = 0; i < sizeof(nonce); i++)
            osSnprintf(upload->id + i * 2U, 3, "%02x", nonce[i]);
        for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++)
            if (&encoder_uploads[i] != upload && encoder_uploads[i].used &&
                !osStrcmp(encoder_uploads[i].id, upload->id))
                error = ERROR_FAILURE;
        mutex_unlock(MUTEX_ENCODER_UPLOAD);
    }
    if (error == NO_ERROR)
    {
        upload->library_root = strdup(get_settings()->internal.librarydirfull);
        upload->directory = upload->library_root != NULL
            ? custom_asprintf("%s%c%s%c%s", upload->library_root, PATH_SEPARATOR,
                ENCODER_UPLOAD_DIRECTORY, PATH_SEPARATOR, upload->id) : NULL;
        if (upload->directory == NULL)
            error = ERROR_OUT_OF_MEMORY;
        else if (fsDirExists(upload->directory))
        {
            /* Never clean up a collision that belongs to another request. */
            osFreeMem(upload->directory);
            upload->directory = NULL;
            error = ERROR_FAILURE;
        }
    }
    if (error == NO_ERROR)
    {
        char *directory = custom_asprintf("%s%cchapters", upload->directory, PATH_SEPARATOR);
        error = directory != NULL ? fsCreateDirEx(directory, TRUE) : ERROR_OUT_OF_MEMORY;
        osFreeMem(directory);
    }
    if (error != NO_ERROR)
    {
        encoder_discard(upload);
        return encoder_failure(connection, error == ERROR_BUFFER_OVERFLOW ? 413 : 400,
            "Native import could not be created; check metadata size and library storage");
    }
    cJSON *response = cJSON_CreateObject();
    if (response != NULL)
        cJSON_AddStringToObject(response, "id", upload->id);
    mutex_lock(MUTEX_ENCODER_UPLOAD);
    upload->last_activity = now;
    upload->busy = FALSE;
    mutex_unlock(MUTEX_ENCODER_UPLOAD);
    error = encoder_json_reply(connection, 201, response);
    return error;
}

static error_t encoder_receive_chapter(HttpConnection *connection, encoder_upload_t *upload)
{
    const size_t expected = connection->request.byteCount;
    if (expected == 0 || expected > ENCODER_MAX_FILE_BYTES)
        return ERROR_INVALID_PARAMETER;
    char *path = custom_asprintf("%s%cchapters%cchapter-%" PRIuSIZE ".opus", upload->directory,
        PATH_SEPARATOR, PATH_SEPARATOR, upload->received);
    char *temporary = path != NULL ? custom_asprintf("%s.part", path) : NULL;
    FsFile *file = temporary != NULL ? fsOpenFile(temporary,
        FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC) : NULL;
    uint8_t *buffer = osAllocMem(ENCODER_IO_BYTES);
    error_t error = path == NULL || temporary == NULL || buffer == NULL ? ERROR_OUT_OF_MEMORY :
        (file == NULL ? ERROR_FILE_OPENING_FAILED : NO_ERROR);
    size_t written = 0;
    while (error == NO_ERROR && written < expected)
    {
        size_t received = 0;
        size_t requested = MIN((size_t)ENCODER_IO_BYTES, expected - written);
        error = httpReadStream(connection, buffer, requested, &received, 0);
        if (error == NO_ERROR && (received == 0 || received > requested))
            error = ERROR_READ_FAILED;
        if (error == NO_ERROR)
            error = fsWriteFile(file, buffer, received);
        if (error == NO_ERROR)
            written += received;
    }
    if (error == NO_ERROR)
        error = fsFlushFile(file);
    if (file != NULL)
        fsCloseFile(file);
    double duration = 0;
    if (error == NO_ERROR)
        error = encoder_validate_opus_file(temporary, &duration);
    if (error == NO_ERROR)
        error = fsRenameFile(temporary, path);
    if (error == NO_ERROR)
    {
        v3_native_library_collection_chapter_t *chapter = &upload->collection.chapters[upload->received];
        chapter->path = path;
        path = NULL;
        chapter->file_size = (uint32_t)written;
        chapter->duration_seconds = duration;
        chapter->has_duration = TRUE;
        upload->received++;
    }
    if (temporary != NULL && fsFileExists(temporary))
        fsDeleteFile(temporary);
    osFreeMem(buffer);
    osFreeMem(temporary);
    osFreeMem(path);
    return error;
}

error_t handleApiNativeImportOperation(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx)
{
    (void)query; (void)client_ctx;
    if (uri == NULL || osStrncmp(uri, ENCODER_UPLOAD_PREFIX, sizeof(ENCODER_UPLOAD_PREFIX) - 1U))
        return encoder_failure(connection, 404, "Unknown native import");
    const char *suffix = uri + sizeof(ENCODER_UPLOAD_PREFIX) - 1U;
    if (osStrlen(suffix) < ENCODER_UPLOAD_ID_LENGTH)
        return encoder_failure(connection, 404, "Unknown native import");
    char id[ENCODER_UPLOAD_ID_LENGTH + 1U];
    osMemcpy(id, suffix, ENCODER_UPLOAD_ID_LENGTH);
    id[ENCODER_UPLOAD_ID_LENGTH] = '\0';
    suffix += ENCODER_UPLOAD_ID_LENGTH;
    if (!encoder_id_valid(id))
        return encoder_failure(connection, 404, "Unknown native import");
    bool_t abort = !osStrcmp(connection->request.method, "DELETE") && *suffix == '\0';
    bool_t finish = !osStrcmp(connection->request.method, "POST") && !osStrcmp(suffix, "/finish");
    size_t chapter_index = 0;
    bool_t chapter = !osStrcmp(connection->request.method, "POST") && !osStrncmp(suffix, "/chapter/", 9);
    if (chapter)
    {
        const char *digit = suffix + 9;
        chapter = *digit != '\0' && !(digit[0] == '0' && digit[1] != '\0');
        for (; chapter && *digit != '\0'; digit++)
        {
            if (!isdigit((unsigned char)*digit) || chapter_index > ENCODER_NATIVE_MAX_CHAPTERS / 10U)
                chapter = FALSE;
            else
                chapter_index = chapter_index * 10U + (size_t)(*digit - '0');
        }
    }
    if (!abort && !finish && !chapter)
        return encoder_failure(connection, 404, "Unknown native import operation");
    uint32_t now = 0;
    if (!mqtt_monotonic_ms(&now))
        return encoder_failure(connection, 503, "Monotonic clock unavailable");
    encoder_expire_uploads(now);
    encoder_upload_t *upload = NULL;
    bool_t busy = FALSE;
    mutex_lock(MUTEX_ENCODER_UPLOAD);
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++)
    {
        if (encoder_uploads[i].used && !osStrcmp(encoder_uploads[i].id, id))
        {
            busy = encoder_uploads[i].busy;
            if (!busy)
            {
                upload = &encoder_uploads[i];
                upload->busy = TRUE;
            }
            break;
        }
    }
    mutex_unlock(MUTEX_ENCODER_UPLOAD);
    if (upload == NULL)
        return encoder_failure(connection, busy ? 409 : 404, busy ? "Native import is busy" : "Unknown or expired native import");
    bool_t discard = abort;
    uint_t status = 200;
    cJSON *response = cJSON_CreateObject();
    const char *failure = NULL;
    if (osStrcmp(upload->library_root, get_settings()->internal.librarydirfull))
    {
        discard = TRUE;
        status = 409;
        failure = "Library configuration changed; start the import again";
    }
    else if (abort)
        cJSON_AddBoolToObject(response, "ok", TRUE);
    else if (chapter)
    {
        if (chapter_index != upload->received || upload->received >= upload->collection.chapter_count)
        {
            status = 409;
            failure = "Chapter index does not match the next expected chapter";
        }
        else if (connection->request.byteCount == 0 || connection->request.byteCount > ENCODER_MAX_FILE_BYTES)
        {
            status = 413;
            failure = "Chapter exceeds the transport size limit or is empty";
        }
        else
        {
            error_t error = encoder_receive_chapter(connection, upload);
            if (error != NO_ERROR)
            {
                TRACE_WARNING("Native chapter upload failed: %d\r\n", error);
                discard = TRUE;
                status = 422;
                failure = "Chapter upload, storage or Ogg/Opus validation failed; import discarded";
            }
            else
            {
                cJSON_AddBoolToObject(response, "ok", TRUE);
                cJSON_AddNumberToObject(response, "nextIndex", upload->received);
            }
        }
    }
    else if (finish)
    {
        if (upload->received != upload->collection.chapter_count)
        {
            status = 409;
            failure = "Not all chapters have been received";
        }
        else
        {
            char *source = NULL;
            bool_t reused = FALSE;
            error_t error = v3_native_library_publish_custom(upload->library_root,
                upload->directory, &upload->collection, &source, &reused);
            discard = TRUE;
            if (error != NO_ERROR)
            {
                TRACE_WARNING("Native collection publication failed: %d\r\n", error);
                status = 500;
                failure = "Native collection publication failed; import discarded";
            }
            else
            {
                cJSON_AddStringToObject(response, "source", source);
                const char *hash = source + sizeof("lib://by/contentHash/") - 1U;
                char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
                osMemcpy(content_hash, hash, V3_NATIVE_LIBRARY_HASH_HEX_LENGTH);
                content_hash[V3_NATIVE_LIBRARY_HASH_HEX_LENGTH] = '\0';
                cJSON_AddStringToObject(response, "contentHash", content_hash);
                cJSON_AddBoolToObject(response, "reused", reused);
                status = reused ? 200 : 201;
            }
            osFreeMem(source);
        }
    }
    if (discard)
        encoder_discard(upload);
    else
    {
        mqtt_monotonic_ms(&now);
        mutex_lock(MUTEX_ENCODER_UPLOAD);
        upload->last_activity = now;
        upload->busy = FALSE;
        mutex_unlock(MUTEX_ENCODER_UPLOAD);
    }
    if (failure != NULL)
    {
        cJSON_Delete(response);
        return encoder_failure(connection, status, failure);
    }
    return encoder_json_reply(connection, status, response);
}
