#include "v3_local_content.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>

#include "debug.h"
#include "cJSON.h"
#include "fs_ext.h"
#include "fs_port_config.h"
#include "mutex_manager.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "os_port.h"
#include "rand.h"
#include "toniefile.h"

#define V3_LOCAL_CONTENT_CACHE_DIR "v3-local"
#define V3_LOCAL_CONTENT_GENERATIONS_DIR "generations"
#define V3_LOCAL_CONTENT_TAP_STATE_DIR "tap-state"
#define V3_LOCAL_CONTENT_TEMP_ATTEMPTS 8
#define V3_LOCAL_CONTENT_OGG_HEADER_SIZE 27
#define V3_LOCAL_CONTENT_OGG_MAX_SEGMENTS 255
#define V3_LOCAL_CONTENT_OGG_MAX_PAGE_BODY (255 * 255)
#define V3_LOCAL_CONTENT_DESCRIPTOR_MAX_SIZE (TONIEFILE_MAX_CHAPTERS * 1024 + 4096)
#define V3_LOCAL_CONTENT_OGG_SERIAL_NAMESPACE 0x54430000U
#define V3_LOCAL_CONTENT_TAP_OGG_SERIAL 0x54415031U
#define V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_OFFSET 10U
#define V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_SIZE 2U

typedef struct
{
    uint8_t *data;
    size_t length;
    size_t capacity;
} v3_local_buffer_t;

typedef struct
{
    uint8_t *opus_head;
    size_t opus_head_length;
    uint8_t *opus_tags;
    size_t opus_tags_length;
} v3_local_headers_t;

typedef struct
{
    char *temp_path;
    char *final_path;
    v3_local_content_chapter_t descriptor;
} v3_local_prepared_chapter_t;

typedef struct
{
    FsFile *file;
    ogg_stream_state stream;
    bool_t stream_initialized;
    Sha256Context sha256;
    uint64_t bytes_written;
    uint64_t granule_position;
    int64_t packet_number;
    v3_local_buffer_t pending_audio;
    uint32_t pending_samples;
    size_t source_packet_index;
    size_t chapter_index;
    v3_local_headers_t *headers;
} v3_local_writer_t;

static char *v3_local_format_alloc(const char *format, ...)
{
    va_list args;
    va_start(args, format);

    va_list count_args;
    va_copy(count_args, args);
    int length = osVsnprintf(NULL, 0, format, count_args);
    va_end(count_args);
    if (length < 0)
    {
        va_end(args);
        return NULL;
    }

    char *value = osAllocMem((size_t)length + 1);
    if (value != NULL)
    {
        osVsnprintf(value, (size_t)length + 1, format, args);
    }
    va_end(args);
    return value;
}

static void v3_local_buffer_free(v3_local_buffer_t *buffer)
{
    if (buffer == NULL)
    {
        return;
    }
    osFreeMem(buffer->data);
    osMemset(buffer, 0, sizeof(*buffer));
}

static error_t v3_local_buffer_assign(v3_local_buffer_t *buffer, const uint8_t *data, size_t length)
{
    if (buffer == NULL || (length > 0 && data == NULL))
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (length > buffer->capacity)
    {
        uint8_t *replacement = osAllocMem(length);
        if (replacement == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        osFreeMem(buffer->data);
        buffer->data = replacement;
        buffer->capacity = length;
    }

    if (length > 0)
    {
        osMemcpy(buffer->data, data, length);
    }
    buffer->length = length;
    return NO_ERROR;
}

static error_t v3_local_buffer_append(v3_local_buffer_t *buffer, const uint8_t *data, size_t length)
{
    if (buffer == NULL || (length > 0 && data == NULL) || SIZE_MAX - buffer->length < length)
    {
        return ERROR_INVALID_PARAMETER;
    }

    size_t required = buffer->length + length;
    if (required > buffer->capacity)
    {
        size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
        while (capacity < required)
        {
            if (capacity > SIZE_MAX / 2)
            {
                capacity = required;
                break;
            }
            capacity *= 2;
        }

        uint8_t *replacement = osAllocMem(capacity);
        if (replacement == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        if (buffer->length > 0)
        {
            osMemcpy(replacement, buffer->data, buffer->length);
        }
        osFreeMem(buffer->data);
        buffer->data = replacement;
        buffer->capacity = capacity;
    }

    if (length > 0)
    {
        osMemcpy(buffer->data + buffer->length, data, length);
    }
    buffer->length = required;
    return NO_ERROR;
}

static error_t v3_local_copy_packet(uint8_t **target, size_t *target_length, const uint8_t *packet, size_t packet_length)
{
    if (target == NULL || target_length == NULL || packet == NULL || packet_length == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint8_t *copy = osAllocMem(packet_length);
    if (copy == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    osMemcpy(copy, packet, packet_length);
    osFreeMem(*target);
    *target = copy;
    *target_length = packet_length;
    return NO_ERROR;
}

static void v3_local_headers_free(v3_local_headers_t *headers)
{
    if (headers == NULL)
    {
        return;
    }
    osFreeMem(headers->opus_head);
    osFreeMem(headers->opus_tags);
    osMemset(headers, 0, sizeof(*headers));
}

static void v3_local_digest_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char hex[V3_LOCAL_CONTENT_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    hex[SHA256_DIGEST_SIZE * 2] = '\0';
}

static int v3_local_hex_value(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    return -1;
}

static bool_t v3_local_parse_digest(const char *hex,
                                    uint8_t digest[SHA256_DIGEST_SIZE],
                                    char canonical[V3_LOCAL_CONTENT_SHA256_HEX_SIZE])
{
    if (hex == NULL || osStrlen(hex) != SHA256_DIGEST_SIZE * 2)
    {
        return FALSE;
    }

    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        int high = v3_local_hex_value(hex[i * 2]);
        int low = v3_local_hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
        {
            return FALSE;
        }
        digest[i] = (uint8_t)((high << 4) | low);
    }
    v3_local_digest_to_hex(digest, canonical);
    return TRUE;
}

static bool_t v3_local_json_u32(const cJSON *item, uint32_t *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX)
    {
        return FALSE;
    }
    uint32_t parsed = (uint32_t)item->valuedouble;
    if ((double)parsed != item->valuedouble)
    {
        return FALSE;
    }
    *value = parsed;
    return TRUE;
}

static bool_t v3_local_json_size(const cJSON *item, size_t maximum, size_t *value)
{
    uint32_t parsed = 0;
    if (!v3_local_json_u32(item, &parsed) || parsed > maximum)
    {
        return FALSE;
    }
    *value = parsed;
    return TRUE;
}

static bool_t v3_local_string_valid(const char *value, size_t maximum)
{
    if (value == NULL || osStrlen(value) > maximum)
    {
        return FALSE;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; cursor++)
    {
        if (*cursor < 0x20)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static char *v3_local_object_path(const char *cache_root, const char *sha256_hex)
{
    return v3_local_format_alloc("%s%c%s%c%s.opus",
                                 cache_root,
                                 PATH_SEPARATOR,
                                 V3_LOCAL_CONTENT_CACHE_DIR,
                                 PATH_SEPARATOR,
                                 sha256_hex);
}

static char *v3_local_descriptor_dir(const char *cache_root,
                                     uint8_t overlay_id,
                                     const char *canonical_ruid)
{
    return v3_local_format_alloc("%s%c%s%c%s%c%" PRIu8 "%c%s",
                                 cache_root,
                                 PATH_SEPARATOR,
                                 V3_LOCAL_CONTENT_CACHE_DIR,
                                 PATH_SEPARATOR,
                                 V3_LOCAL_CONTENT_GENERATIONS_DIR,
                                 PATH_SEPARATOR,
                                 overlay_id,
                                 PATH_SEPARATOR,
                                 canonical_ruid);
}

static char *v3_local_descriptor_path(const char *descriptor_dir, uint32_t effective_version)
{
    return v3_local_format_alloc("%s%c%" PRIu32 ".json",
                                 descriptor_dir,
                                 PATH_SEPARATOR,
                                 effective_version);
}

static char *v3_local_tap_state_dir(const char *cache_root, uint8_t overlay_id)
{
    return v3_local_format_alloc("%s%c%s%c%s%c%" PRIu8,
                                 cache_root,
                                 PATH_SEPARATOR,
                                 V3_LOCAL_CONTENT_CACHE_DIR,
                                 PATH_SEPARATOR,
                                 V3_LOCAL_CONTENT_TAP_STATE_DIR,
                                 PATH_SEPARATOR,
                                 overlay_id);
}

static char *v3_local_tap_state_path(const char *state_dir, const char *canonical_ruid)
{
    return v3_local_format_alloc("%s%c%s.json", state_dir, PATH_SEPARATOR,
                                 canonical_ruid);
}

static error_t v3_local_ensure_dir(const char *path)
{
    if (fsDirExists(path))
    {
        return NO_ERROR;
    }
    error_t error = fsCreateDirEx(path, true);
    if (error != NO_ERROR && fsDirExists(path))
    {
        return NO_ERROR;
    }
    return error;
}

static error_t v3_local_validate_object(const char *cache_root,
                                        v3_local_content_chapter_t *chapter,
                                        const char *canonical_ruid)
{
    if (chapter == NULL || chapter->index >= TONIEFILE_MAX_CHAPTERS || chapter->file_size == 0)
    {
        return ERROR_INVALID_FILE;
    }

    char digest_hex[V3_LOCAL_CONTENT_SHA256_HEX_SIZE];
    v3_local_digest_to_hex(chapter->sha256, digest_hex);
    if (osStrcmp(digest_hex, chapter->sha256_hex) != 0)
    {
        return ERROR_INVALID_FILE;
    }

    char expected_name[V3_LOCAL_CONTENT_NAME_SIZE];
    int name_length = osSnprintf(expected_name,
                                 sizeof(expected_name),
                                 "teddycloud_%.20s_%02" PRIuSIZE "_%s.opus",
                                 digest_hex,
                                 chapter->index,
                                 canonical_ruid);
    if (name_length < 0 || (size_t)name_length >= sizeof(expected_name) ||
        osStrcasecmp(expected_name, chapter->name) != 0)
    {
        return ERROR_INVALID_FILE;
    }
    osStrcpy(chapter->name, expected_name);

    char *expected_path = v3_local_object_path(cache_root, digest_hex);
    if (expected_path == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    if (chapter->path != NULL && osStrcmp(chapter->path, expected_path) != 0)
    {
        osFreeMem(expected_path);
        return ERROR_INVALID_PATH;
    }

    uint32_t actual_size = 0;
    error_t error = fsGetFileSize(expected_path, &actual_size);
    if (error == NO_ERROR && actual_size != chapter->file_size)
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR && chapter->path == NULL)
    {
        chapter->path = expected_path;
        expected_path = NULL;
    }
    osFreeMem(expected_path);
    return error;
}

static uint32_t v3_local_chapter_serial(size_t chapter_index)
{
    return V3_LOCAL_CONTENT_OGG_SERIAL_NAMESPACE | ((uint32_t)chapter_index + 1U);
}

static error_t v3_local_writer_write_page(v3_local_writer_t *writer, const ogg_page *page)
{
    if (writer == NULL || page == NULL || page->header_len < 0 || page->body_len < 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint64_t page_size = (uint64_t)page->header_len + (uint64_t)page->body_len;
    if (writer->bytes_written + page_size > UINT32_MAX)
    {
        return ERROR_OUT_OF_RANGE;
    }

    error_t error = fsWriteFile(writer->file, page->header, (size_t)page->header_len);
    if (error == NO_ERROR && page->body_len > 0)
    {
        error = fsWriteFile(writer->file, page->body, (size_t)page->body_len);
    }
    if (error != NO_ERROR)
    {
        return error;
    }

    sha256Update(&writer->sha256, page->header, (size_t)page->header_len);
    if (page->body_len > 0)
    {
        sha256Update(&writer->sha256, page->body, (size_t)page->body_len);
    }
    writer->bytes_written += page_size;
    return NO_ERROR;
}

static error_t v3_local_writer_drain(v3_local_writer_t *writer, bool_t flush)
{
    ogg_page page;
    int result = 0;
    do
    {
        result = flush ? ogg_stream_flush(&writer->stream, &page) : ogg_stream_pageout(&writer->stream, &page);
        if (result > 0)
        {
            error_t error = v3_local_writer_write_page(writer, &page);
            if (error != NO_ERROR)
            {
                return error;
            }
        }
    } while (result > 0);
    return NO_ERROR;
}

static error_t v3_local_writer_packet_in(v3_local_writer_t *writer,
                                         const uint8_t *packet,
                                         size_t packet_length,
                                         uint64_t granule_position,
                                         bool_t bos,
                                         bool_t eos,
                                         bool_t flush)
{
    if (packet_length > LONG_MAX)
    {
        return ERROR_OUT_OF_RANGE;
    }

    ogg_packet ogg_packet_data;
    osMemset(&ogg_packet_data, 0, sizeof(ogg_packet_data));
    ogg_packet_data.packet = (unsigned char *)packet;
    ogg_packet_data.bytes = (long)packet_length;
    ogg_packet_data.b_o_s = bos;
    ogg_packet_data.e_o_s = eos;
    ogg_packet_data.granulepos = (ogg_int64_t)granule_position;
    ogg_packet_data.packetno = writer->packet_number++;

    if (ogg_stream_packetin(&writer->stream, &ogg_packet_data) != 0)
    {
        return ERROR_FAILURE;
    }
    return v3_local_writer_drain(writer, flush);
}

static error_t v3_local_writer_write_headers(v3_local_writer_t *writer)
{
    if (writer->headers->opus_head == NULL || writer->headers->opus_tags == NULL)
    {
        return ERROR_INVALID_FILE;
    }

    const uint8_t *opus_head = writer->headers->opus_head;
    uint8_t *adjusted_opus_head = NULL;
    if (writer->chapter_index > 0)
    {
        if (writer->headers->opus_head_length <
            V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_OFFSET +
                V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_SIZE)
        {
            return ERROR_INVALID_FILE;
        }
        adjusted_opus_head = osAllocMem(writer->headers->opus_head_length);
        if (adjusted_opus_head == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        osMemcpy(adjusted_opus_head, writer->headers->opus_head,
                 writer->headers->opus_head_length);
        adjusted_opus_head[V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_OFFSET] = 0;
        adjusted_opus_head[V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_OFFSET + 1] = 0;
        opus_head = adjusted_opus_head;
    }

    error_t error = v3_local_writer_packet_in(writer,
                                              opus_head,
                                              writer->headers->opus_head_length,
                                              0,
                                              TRUE,
                                              FALSE,
                                              TRUE);
    osFreeMem(adjusted_opus_head);
    if (error == NO_ERROR)
    {
        error = v3_local_writer_packet_in(writer,
                                          writer->headers->opus_tags,
                                          writer->headers->opus_tags_length,
                                          0,
                                          FALSE,
                                          FALSE,
                                          TRUE);
    }
    return error;
}

static error_t v3_local_writer_emit_pending(v3_local_writer_t *writer, bool_t eos)
{
    if (writer->pending_audio.length == 0 || writer->pending_samples == 0)
    {
        return ERROR_INVALID_FILE;
    }

    writer->granule_position += writer->pending_samples;
    error_t error = v3_local_writer_packet_in(writer,
                                              writer->pending_audio.data,
                                              writer->pending_audio.length,
                                              writer->granule_position,
                                              FALSE,
                                              eos,
                                              FALSE);
    if (error == NO_ERROR)
    {
        writer->pending_audio.length = 0;
        writer->pending_samples = 0;
    }
    return error;
}

static error_t v3_local_writer_process_packet(v3_local_writer_t *writer, const uint8_t *packet, size_t packet_length)
{
    if (writer->chapter_index == 0 && writer->source_packet_index < 2)
    {
        error_t error = NO_ERROR;
        if (writer->source_packet_index == 0)
        {
            if (packet_length < 8 || osMemcmp(packet, "OpusHead", 8) != 0)
            {
                return ERROR_INVALID_FILE;
            }
            error = v3_local_copy_packet(&writer->headers->opus_head,
                                         &writer->headers->opus_head_length,
                                         packet,
                                         packet_length);
        }
        else
        {
            if (packet_length < 8 || osMemcmp(packet, "OpusTags", 8) != 0)
            {
                return ERROR_INVALID_FILE;
            }
            error = v3_local_copy_packet(&writer->headers->opus_tags,
                                         &writer->headers->opus_tags_length,
                                         packet,
                                         packet_length);
        }
        if (error == NO_ERROR)
        {
            error = v3_local_writer_packet_in(writer,
                                              packet,
                                              packet_length,
                                              0,
                                              writer->source_packet_index == 0,
                                              FALSE,
                                              TRUE);
        }
        writer->source_packet_index++;
        return error;
    }

    if (packet_length > INT32_MAX)
    {
        return ERROR_OUT_OF_RANGE;
    }

    int samples_per_frame = opus_packet_get_samples_per_frame(packet, OPUS_SAMPLING_RATE);
    int frame_count = opus_packet_get_nb_frames(packet, (opus_int32)packet_length);
    if (samples_per_frame <= 0 || frame_count <= 0 || samples_per_frame > INT_MAX / frame_count)
    {
        return ERROR_INVALID_FILE;
    }

    error_t error = NO_ERROR;
    if (writer->pending_audio.length > 0)
    {
        error = v3_local_writer_emit_pending(writer, FALSE);
    }
    if (error == NO_ERROR)
    {
        error = v3_local_buffer_assign(&writer->pending_audio, packet, packet_length);
        writer->pending_samples = (uint32_t)(samples_per_frame * frame_count);
    }
    writer->source_packet_index++;
    return error;
}

static error_t v3_local_parse_source_range(FsFile *source,
                                           uint64_t start,
                                           uint64_t end,
                                           v3_local_writer_t *writer)
{
    if (source == NULL || writer == NULL || start >= end || end > INT_MAX)
    {
        return ERROR_INVALID_PARAMETER;
    }

    error_t error = fsSeekFile(source, (int_t)start, FS_SEEK_SET);
    if (error != NO_ERROR)
    {
        return error;
    }

    uint64_t position = start;
    v3_local_buffer_t packet = {0};
    bool_t packet_continues = FALSE;
    while (position < end && error == NO_ERROR)
    {
        uint8_t page_header[V3_LOCAL_CONTENT_OGG_HEADER_SIZE + V3_LOCAL_CONTENT_OGG_MAX_SEGMENTS];
        size_t read_length = 0;
        if (end - position < V3_LOCAL_CONTENT_OGG_HEADER_SIZE)
        {
            error = ERROR_INVALID_FILE;
            break;
        }

        error = fsReadFile(source, page_header, V3_LOCAL_CONTENT_OGG_HEADER_SIZE, &read_length);
        if (error != NO_ERROR || read_length != V3_LOCAL_CONTENT_OGG_HEADER_SIZE || osMemcmp(page_header, "OggS", 4) != 0 || page_header[4] != 0)
        {
            error = ERROR_INVALID_FILE;
            break;
        }
        position += V3_LOCAL_CONTENT_OGG_HEADER_SIZE;

        bool_t continued_flag = (page_header[5] & 0x01) != 0;
        if (continued_flag != packet_continues)
        {
            error = ERROR_INVALID_FILE;
            break;
        }

        uint8_t segment_count = page_header[26];
        if (end - position < segment_count)
        {
            error = ERROR_INVALID_FILE;
            break;
        }
        error = fsReadFile(source, page_header + V3_LOCAL_CONTENT_OGG_HEADER_SIZE, segment_count, &read_length);
        if (error != NO_ERROR || read_length != segment_count)
        {
            error = ERROR_INVALID_FILE;
            break;
        }
        position += segment_count;

        size_t body_length = 0;
        for (size_t i = 0; i < segment_count; i++)
        {
            body_length += page_header[V3_LOCAL_CONTENT_OGG_HEADER_SIZE + i];
        }
        if (body_length > V3_LOCAL_CONTENT_OGG_MAX_PAGE_BODY || end - position < body_length)
        {
            error = ERROR_INVALID_FILE;
            break;
        }

        uint8_t *body = osAllocMem(body_length == 0 ? 1 : body_length);
        if (body == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
            break;
        }
        if (body_length > 0)
        {
            error = fsReadFile(source, body, body_length, &read_length);
            if (error != NO_ERROR || read_length != body_length)
            {
                osFreeMem(body);
                error = ERROR_INVALID_FILE;
                break;
            }
        }
        position += body_length;

        size_t body_position = 0;
        for (size_t i = 0; i < segment_count && error == NO_ERROR; i++)
        {
            size_t segment_length = page_header[V3_LOCAL_CONTENT_OGG_HEADER_SIZE + i];
            error = v3_local_buffer_append(&packet, body + body_position, segment_length);
            body_position += segment_length;
            if (error == NO_ERROR && segment_length < 255)
            {
                if (packet.length == 0)
                {
                    error = ERROR_INVALID_FILE;
                    break;
                }
                error = v3_local_writer_process_packet(writer, packet.data, packet.length);
                packet.length = 0;
                packet_continues = FALSE;
            }
            else if (error == NO_ERROR)
            {
                packet_continues = TRUE;
            }
        }
        osFreeMem(body);
    }

    if (error == NO_ERROR && (position != end || packet.length != 0 || packet_continues))
    {
        error = ERROR_INVALID_FILE;
    }
    v3_local_buffer_free(&packet);
    return error;
}

static error_t v3_local_create_temp_path(const char *cache_dir, size_t chapter_index, char **path)
{
    if (cache_dir == NULL || path == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    for (size_t attempt = 0; attempt < V3_LOCAL_CONTENT_TEMP_ATTEMPTS; attempt++)
    {
        uint8_t nonce[8];
        error_t error = rand_get_algo()->read(rand_get_context(), nonce, sizeof(nonce));
        if (error != NO_ERROR)
        {
            return error;
        }
        char nonce_hex[sizeof(nonce) * 2 + 1];
        for (size_t i = 0; i < sizeof(nonce); i++)
        {
            osSnprintf(nonce_hex + i * 2, 3, "%02x", nonce[i]);
        }

        char *candidate = v3_local_format_alloc("%s%c.tmp-%02" PRIuSIZE "-%s.opus",
                                                cache_dir,
                                                PATH_SEPARATOR,
                                                chapter_index,
                                                nonce_hex);
        if (candidate == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        if (!fsFileExists(candidate))
        {
            *path = candidate;
            return NO_ERROR;
        }
        osFreeMem(candidate);
    }
    return ERROR_FAILURE;
}

static error_t v3_local_create_descriptor_temp_path(const char *descriptor_dir,
                                                    uint32_t effective_version,
                                                    char **path)
{
    if (descriptor_dir == NULL || path == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    for (size_t attempt = 0; attempt < V3_LOCAL_CONTENT_TEMP_ATTEMPTS; attempt++)
    {
        uint8_t nonce[8];
        error_t error = rand_get_algo()->read(rand_get_context(), nonce, sizeof(nonce));
        if (error != NO_ERROR)
        {
            return error;
        }
        char nonce_hex[sizeof(nonce) * 2 + 1];
        for (size_t i = 0; i < sizeof(nonce); i++)
        {
            osSnprintf(nonce_hex + i * 2, 3, "%02x", nonce[i]);
        }

        char *candidate = v3_local_format_alloc("%s%c.%" PRIu32 "-%s.tmp",
                                                descriptor_dir,
                                                PATH_SEPARATOR,
                                                effective_version,
                                                nonce_hex);
        if (candidate == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        if (!fsFileExists(candidate))
        {
            *path = candidate;
            return NO_ERROR;
        }
        osFreeMem(candidate);
    }
    return ERROR_FAILURE;
}

static void v3_local_writer_close(v3_local_writer_t *writer)
{
    if (writer == NULL)
    {
        return;
    }
    if (writer->file != NULL)
    {
        fsCloseFile(writer->file);
        writer->file = NULL;
    }
    if (writer->stream_initialized)
    {
        ogg_stream_clear(&writer->stream);
        writer->stream_initialized = FALSE;
    }
    v3_local_buffer_free(&writer->pending_audio);
}

static error_t v3_local_remux_chapter(FsFile *source,
                                      uint64_t start,
                                      uint64_t end,
                                      size_t chapter_index,
                                      uint32_t serial,
                                      const char *temp_path,
                                      v3_local_headers_t *headers,
                                      v3_local_content_chapter_t *descriptor)
{
    v3_local_writer_t writer;
    osMemset(&writer, 0, sizeof(writer));
    writer.chapter_index = chapter_index;
    writer.headers = headers;
    sha256Init(&writer.sha256);

    writer.file = fsOpenFile(temp_path, FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    if (writer.file == NULL)
    {
        return ERROR_FILE_OPENING_FAILED;
    }
    if (ogg_stream_init(&writer.stream, (int)serial) != 0)
    {
        v3_local_writer_close(&writer);
        return ERROR_FAILURE;
    }
    writer.stream_initialized = TRUE;

    error_t error = NO_ERROR;
    if (chapter_index > 0)
    {
        error = v3_local_writer_write_headers(&writer);
    }
    if (error == NO_ERROR)
    {
        error = v3_local_parse_source_range(source, start, end, &writer);
    }
    if (error == NO_ERROR && (headers->opus_head == NULL || headers->opus_tags == NULL))
    {
        error = ERROR_INVALID_FILE;
    }
    if (error == NO_ERROR)
    {
        error = v3_local_writer_emit_pending(&writer, TRUE);
    }
    if (error == NO_ERROR)
    {
        error = v3_local_writer_drain(&writer, TRUE);
    }
    if (error == NO_ERROR)
    {
        error = fsFlushFile(writer.file);
    }
    if (error == NO_ERROR)
    {
        descriptor->file_size = (uint32_t)writer.bytes_written;
        sha256Final(&writer.sha256, descriptor->sha256);
        v3_local_digest_to_hex(descriptor->sha256, descriptor->sha256_hex);
    }

    v3_local_writer_close(&writer);
    return error;
}

static void v3_local_prepared_free(v3_local_prepared_chapter_t *prepared, size_t count, bool_t delete_temps)
{
    if (prepared == NULL)
    {
        return;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (delete_temps && prepared[i].temp_path != NULL && fsFileExists(prepared[i].temp_path))
        {
            fsDeleteFile(prepared[i].temp_path);
        }
        osFreeMem(prepared[i].temp_path);
        osFreeMem(prepared[i].final_path);
    }
    osFreeMem(prepared);
}

static error_t v3_local_publish_prepared(v3_local_prepared_chapter_t *prepared, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        error_t error = NO_ERROR;
        if (fsFileExists(prepared[i].final_path))
        {
            error = fsCompareFiles(prepared[i].temp_path, prepared[i].final_path, NULL);
            if (error == NO_ERROR)
            {
                error = fsDeleteFile(prepared[i].temp_path);
            }
        }
        else
        {
            error = fsRenameFile(prepared[i].temp_path, prepared[i].final_path);
            if (error != NO_ERROR && fsFileExists(prepared[i].final_path))
            {
                error = fsCompareFiles(prepared[i].temp_path, prepared[i].final_path, NULL);
                if (error == NO_ERROR)
                {
                    error = fsDeleteFile(prepared[i].temp_path);
                }
            }
        }

        if (error != NO_ERROR)
        {
            TRACE_ERROR("Could not atomically publish V3 local chapter %s: %s\r\n",
                        prepared[i].descriptor.sha256_hex,
                        error2text(error));
            return error;
        }
    }
    return NO_ERROR;
}

static error_t v3_local_validate_taf(const char *taf_path,
                                     const TonieboxAudioFileHeader *taf_header,
                                     uint32_t chapter_starts[TONIEFILE_MAX_CHAPTERS],
                                     size_t *chapter_count)
{
    if (taf_path == NULL || taf_header == NULL || chapter_starts == NULL ||
        chapter_count == NULL ||
        taf_header->num_bytes == 0 || taf_header->sha1_hash.data == NULL ||
        taf_header->sha1_hash.len != SHA1_DIGEST_SIZE || taf_header->num_bytes > INT_MAX)
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint32_t file_size = 0;
    error_t error = fsGetFileSize(taf_path, &file_size);
    if (error != NO_ERROR || file_size < TONIEFILE_FRAME_SIZE ||
        (uint64_t)file_size - TONIEFILE_FRAME_SIZE < taf_header->num_bytes)
    {
        return ERROR_INVALID_FILE;
    }

    *chapter_count = 0;
    if (taf_header->n_track_page_nums == 0)
    {
        chapter_starts[(*chapter_count)++] = 0;
        return NO_ERROR;
    }
    if (taf_header->n_track_page_nums > TONIEFILE_MAX_CHAPTERS ||
        taf_header->track_page_nums == NULL || taf_header->track_page_nums[0] != 0)
    {
        return ERROR_INVALID_FILE;
    }

    uint32_t previous_start = 0;
    for (size_t i = 0; i < taf_header->n_track_page_nums; i++)
    {
        uint32_t start_page = taf_header->track_page_nums[i];
        uint64_t start = (uint64_t)start_page * TONIEFILE_FRAME_SIZE;
        if ((i > 0 && start_page < previous_start) || start > taf_header->num_bytes)
        {
            return ERROR_INVALID_FILE;
        }
        previous_start = start_page;

        if (start == taf_header->num_bytes ||
            (*chapter_count > 0 && chapter_starts[*chapter_count - 1] == start_page))
        {
            continue;
        }
        chapter_starts[(*chapter_count)++] = start_page;
    }
    return *chapter_count > 0 ? NO_ERROR : ERROR_INVALID_FILE;
}

static error_t v3_local_content_prepare_internal(const char *taf_path,
                                                 const TonieboxAudioFileHeader *taf_header,
                                                 const char *cache_root,
                                                 const char *ruid,
                                                 bool_t tap_serial,
                                                 v3_local_content_generation_t *generation)
{
    if (cache_root == NULL || cache_root[0] == '\0' || generation == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(generation, 0, sizeof(*generation));
    if (!tb2_ruid_canonicalize(ruid, generation->ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    uint32_t chapter_starts[TONIEFILE_MAX_CHAPTERS];
    size_t chapter_count = 0;
    error_t error = v3_local_validate_taf(taf_path, taf_header,
                                          chapter_starts, &chapter_count);
    if (error != NO_ERROR)
    {
        osMemset(generation, 0, sizeof(*generation));
        return error;
    }

    char *cache_dir = v3_local_format_alloc("%s%c%s", cache_root, PATH_SEPARATOR, V3_LOCAL_CONTENT_CACHE_DIR);
    if (cache_dir == NULL)
    {
        osMemset(generation, 0, sizeof(*generation));
        return ERROR_OUT_OF_MEMORY;
    }
    if (!fsDirExists(cache_dir))
    {
        error = fsCreateDirEx(cache_dir, true);
        if (error != NO_ERROR && fsDirExists(cache_dir))
        {
            error = NO_ERROR;
        }
    }

    v3_local_prepared_chapter_t *prepared = NULL;
    FsFile *source = NULL;
    v3_local_headers_t headers = {0};
    if (error == NO_ERROR)
    {
        prepared = osAllocMem(chapter_count * sizeof(*prepared));
        if (prepared == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            osMemset(prepared, 0, chapter_count * sizeof(*prepared));
        }
    }
    if (error == NO_ERROR)
    {
        source = fsOpenFile(taf_path, FS_FILE_MODE_READ);
        if (source == NULL)
        {
            error = ERROR_FILE_OPENING_FAILED;
        }
    }

    for (size_t i = 0; i < chapter_count && error == NO_ERROR; i++)
    {
        uint64_t start_block = chapter_starts[i];
        uint64_t end = i + 1 < chapter_count
                           ? (uint64_t)chapter_starts[i + 1] * TONIEFILE_FRAME_SIZE
                           : taf_header->num_bytes;
        uint64_t start = TONIEFILE_FRAME_SIZE + start_block * TONIEFILE_FRAME_SIZE;
        end += TONIEFILE_FRAME_SIZE;

        error = v3_local_create_temp_path(cache_dir, i, &prepared[i].temp_path);
        if (error == NO_ERROR)
        {
            prepared[i].descriptor.index = i;
            error = v3_local_remux_chapter(source,
                                           start,
                                           end,
                                           i,
                                           tap_serial ? V3_LOCAL_CONTENT_TAP_OGG_SERIAL
                                                      : v3_local_chapter_serial(i),
                                           prepared[i].temp_path,
                                           &headers,
                                           &prepared[i].descriptor);
        }
        if (error == NO_ERROR)
        {
            int length = osSnprintf(prepared[i].descriptor.name,
                                    sizeof(prepared[i].descriptor.name),
                                    "teddycloud_%.20s_%02" PRIuSIZE "_%s.opus",
                                    prepared[i].descriptor.sha256_hex,
                                    i,
                                    generation->ruid);
            if (length < 0 || (size_t)length >= sizeof(prepared[i].descriptor.name))
            {
                error = ERROR_FAILURE;
            }
        }
        if (error == NO_ERROR)
        {
            prepared[i].final_path = v3_local_format_alloc("%s%c%s.opus",
                                                           cache_dir,
                                                           PATH_SEPARATOR,
                                                           prepared[i].descriptor.sha256_hex);
            if (prepared[i].final_path == NULL)
            {
                error = ERROR_OUT_OF_MEMORY;
            }
        }
    }

    if (source != NULL)
    {
        fsCloseFile(source);
    }
    if (error == NO_ERROR)
    {
        error = v3_local_publish_prepared(prepared, chapter_count);
    }
    if (error == NO_ERROR)
    {
        generation->chapters = osAllocMem(chapter_count * sizeof(*generation->chapters));
        if (generation->chapters == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
        else
        {
            osMemset(generation->chapters, 0, chapter_count * sizeof(*generation->chapters));
            generation->chapter_count = chapter_count;
            for (size_t i = 0; i < chapter_count; i++)
            {
                osMemcpy(&generation->chapters[i], &prepared[i].descriptor, sizeof(prepared[i].descriptor));
                generation->chapters[i].path = prepared[i].final_path;
                prepared[i].final_path = NULL;
            }
        }
    }

    v3_local_headers_free(&headers);
    v3_local_prepared_free(prepared, chapter_count, TRUE);
    osFreeMem(cache_dir);
    if (error != NO_ERROR)
    {
        v3_local_content_generation_free(generation);
    }
    return error;
}

error_t v3_local_content_prepare(const char *taf_path,
                                 const TonieboxAudioFileHeader *taf_header,
                                 const char *cache_root,
                                 const char *ruid,
                                 v3_local_content_generation_t *generation)
{
    return v3_local_content_prepare_internal(taf_path, taf_header, cache_root,
                                             ruid, FALSE, generation);
}

error_t v3_local_content_prepare_tap(const char *taf_path,
                                     const TonieboxAudioFileHeader *taf_header,
                                     const char *cache_root,
                                     const char *ruid,
                                     v3_local_content_generation_t *generation)
{
    error_t error = v3_local_content_prepare_internal(taf_path, taf_header,
                                                      cache_root, ruid, TRUE,
                                                      generation);
    if (error == NO_ERROR)
    {
        generation->source_kind = V3_LOCAL_CONTENT_SOURCE_TAP;
    }
    return error;
}

void v3_local_content_generation_free(v3_local_content_generation_t *generation)
{
    if (generation == NULL)
    {
        return;
    }
    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        osFreeMem(generation->chapters[i].path);
        osFreeMem(generation->chapters[i].title);
    }
    osFreeMem(generation->chapters);
    osFreeMem(generation->source_path);
    osFreeMem(generation->title);
    osMemset(generation, 0, sizeof(*generation));
}

const v3_local_content_chapter_t *v3_local_content_find_chapter(const v3_local_content_generation_t *generation,
                                                               const char *name)
{
    if (generation == NULL || name == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        if (osStrcmp(generation->chapters[i].name, name) == 0)
        {
            return &generation->chapters[i];
        }
    }
    return NULL;
}

static error_t v3_local_validate_generation(const char *cache_root,
                                            const v3_local_content_generation_t *generation)
{
    if (cache_root == NULL || cache_root[0] == '\0' || generation == NULL ||
        generation->chapter_count == 0 || generation->chapter_count > TONIEFILE_MAX_CHAPTERS ||
        generation->chapters == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    char canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (!tb2_ruid_canonicalize(generation->ruid, canonical_ruid) ||
        osStrcmp(generation->ruid, canonical_ruid) != 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        if (generation->chapters[i].index != i)
        {
            return ERROR_INVALID_FILE;
        }
        error_t error = v3_local_validate_object(cache_root,
                                                 &generation->chapters[i],
                                                 canonical_ruid);
        if (error != NO_ERROR)
        {
            return error;
        }
    }
    if (generation->source_kind == V3_LOCAL_CONTENT_SOURCE_TAP)
    {
        if (!v3_local_string_valid(generation->source_path,
                                   V3_LOCAL_CONTENT_SOURCE_PATH_MAX) ||
            !v3_local_string_valid(generation->title,
                                   V3_LOCAL_CONTENT_TITLE_MAX) ||
            generation->shuffle_mode > 2)
        {
            return ERROR_INVALID_FILE;
        }
        for (size_t i = 0; i < generation->chapter_count; i++)
        {
            if (!v3_local_string_valid(generation->chapters[i].title,
                                       V3_LOCAL_CONTENT_TITLE_MAX))
            {
                return ERROR_INVALID_FILE;
            }
        }
    }
    return NO_ERROR;
}

static cJSON *v3_local_generation_to_json(uint8_t overlay_id,
                                          uint32_t effective_version,
                                          const v3_local_content_generation_t *generation)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *chapters = NULL;
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "schemaVersion", V3_LOCAL_CONTENT_DESCRIPTOR_SCHEMA_VERSION) == NULL ||
        cJSON_AddNumberToObject(root, "overlay", overlay_id) == NULL ||
        cJSON_AddStringToObject(root, "ruid", generation->ruid) == NULL ||
        cJSON_AddNumberToObject(root, "effectiveVersion", effective_version) == NULL ||
        (chapters = cJSON_AddArrayToObject(root, "chapters")) == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    if (generation->source_kind == V3_LOCAL_CONTENT_SOURCE_TAP)
    {
        if (cJSON_AddStringToObject(root, "sourceKind", "tap") == NULL ||
            cJSON_AddStringToObject(root, "sourcePath", generation->source_path) == NULL ||
            cJSON_AddStringToObject(root, "title", generation->title) == NULL ||
            cJSON_AddNumberToObject(root, "tapAudioId", generation->tap_audio_id) == NULL ||
            cJSON_AddNumberToObject(root, "shuffleMode", generation->shuffle_mode) == NULL ||
            cJSON_AddNumberToObject(root, "selectionGeneration", generation->selection_generation) == NULL)
        {
            cJSON_Delete(root);
            return NULL;
        }
    }

    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        const v3_local_content_chapter_t *chapter = &generation->chapters[i];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddNumberToObject(entry, "index", chapter->index) == NULL ||
            cJSON_AddStringToObject(entry, "name", chapter->name) == NULL ||
            cJSON_AddStringToObject(entry, "sha256", chapter->sha256_hex) == NULL ||
            cJSON_AddNumberToObject(entry, "fileSize", chapter->file_size) == NULL ||
            (generation->source_kind == V3_LOCAL_CONTENT_SOURCE_TAP &&
             (cJSON_AddNumberToObject(entry, "sourceIndex", chapter->source_index) == NULL ||
              cJSON_AddStringToObject(entry, "title", chapter->title) == NULL ||
              cJSON_AddNumberToObject(entry, "duration", chapter->duration_seconds) == NULL)) ||
            !cJSON_AddItemToArray(chapters, entry))
        {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

static error_t v3_local_publish_descriptor(const char *temp_path, const char *descriptor_path)
{
    if (fsFileExists(descriptor_path))
    {
        error_t error = fsCompareFiles(temp_path, descriptor_path, NULL);
        if (error == NO_ERROR)
        {
            return fsDeleteFile(temp_path);
        }
        return ERROR_ABORTED;
    }

    error_t error = fsRenameFile(temp_path, descriptor_path);
    if (error != NO_ERROR && fsFileExists(descriptor_path))
    {
        error = fsCompareFiles(temp_path, descriptor_path, NULL);
        if (error == NO_ERROR)
        {
            error = fsDeleteFile(temp_path);
        }
        else
        {
            error = ERROR_ABORTED;
        }
    }
    return error;
}

error_t v3_local_content_generation_save(const char *cache_root,
                                         uint8_t overlay_id,
                                         uint32_t effective_version,
                                         v3_local_content_generation_t *generation)
{
    if (effective_version == 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    error_t error = v3_local_validate_generation(cache_root, generation);
    if (error != NO_ERROR)
    {
        return error;
    }

    char *descriptor_dir = v3_local_descriptor_dir(cache_root, overlay_id, generation->ruid);
    char *descriptor_path = NULL;
    char *temp_path = NULL;
    cJSON *json = NULL;
    char *serialized = NULL;
    if (descriptor_dir == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
    }
    if (error == NO_ERROR)
    {
        error = v3_local_ensure_dir(descriptor_dir);
    }
    if (error == NO_ERROR)
    {
        descriptor_path = v3_local_descriptor_path(descriptor_dir, effective_version);
        if (descriptor_path == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }
    if (error == NO_ERROR)
    {
        error = v3_local_create_descriptor_temp_path(descriptor_dir,
                                                     effective_version,
                                                     &temp_path);
    }
    if (error == NO_ERROR)
    {
        json = v3_local_generation_to_json(overlay_id, effective_version, generation);
        if (json == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }
    if (error == NO_ERROR)
    {
        serialized = cJSON_PrintUnformatted(json);
        if (serialized == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }
    if (error == NO_ERROR && osStrlen(serialized) > V3_LOCAL_CONTENT_DESCRIPTOR_MAX_SIZE)
    {
        error = ERROR_OUT_OF_RANGE;
    }
    if (error == NO_ERROR)
    {
        FsFile *file = fsOpenFile(temp_path,
                                  FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
        if (file == NULL)
        {
            error = ERROR_FILE_OPENING_FAILED;
        }
        else
        {
            error = fsWriteFile(file, serialized, osStrlen(serialized));
            if (error == NO_ERROR)
            {
                error = fsFlushFile(file);
            }
            fsCloseFile(file);
        }
    }
    if (error == NO_ERROR)
    {
        error = v3_local_publish_descriptor(temp_path, descriptor_path);
    }
    if (error == NO_ERROR)
    {
        generation->overlay_id = overlay_id;
        generation->effective_version = effective_version;
    }
    else if (temp_path != NULL && fsFileExists(temp_path))
    {
        fsDeleteFile(temp_path);
    }

    cJSON_free(serialized);
    cJSON_Delete(json);
    osFreeMem(temp_path);
    osFreeMem(descriptor_path);
    osFreeMem(descriptor_dir);
    return error;
}

static error_t v3_local_read_descriptor(const char *path, char **data, uint32_t *size)
{
    uint32_t file_size = 0;
    error_t error = fsGetFileSize(path, &file_size);
    if (error != NO_ERROR)
    {
        return error;
    }
    if (file_size == 0 || file_size > V3_LOCAL_CONTENT_DESCRIPTOR_MAX_SIZE)
    {
        return ERROR_INVALID_FILE;
    }

    char *buffer = osAllocMem((size_t)file_size + 1);
    if (buffer == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (file == NULL)
    {
        osFreeMem(buffer);
        return ERROR_FILE_OPENING_FAILED;
    }
    size_t read_length = 0;
    error = fsReadFile(file, buffer, file_size, &read_length);
    fsCloseFile(file);
    if (error != NO_ERROR || read_length != file_size)
    {
        osFreeMem(buffer);
        return ERROR_INVALID_FILE;
    }
    buffer[file_size] = '\0';
    *data = buffer;
    *size = file_size;
    return NO_ERROR;
}

static error_t v3_local_generation_from_json(const char *cache_root,
                                             uint8_t overlay_id,
                                             const char *canonical_ruid,
                                             uint32_t effective_version,
                                             const cJSON *root,
                                             v3_local_content_generation_t *generation)
{
    uint32_t schema = 0;
    uint32_t stored_overlay = 0;
    uint32_t stored_version = 0;
    const cJSON *schema_json = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    const cJSON *overlay_json = cJSON_GetObjectItemCaseSensitive(root, "overlay");
    const cJSON *ruid_json = cJSON_GetObjectItemCaseSensitive(root, "ruid");
    const cJSON *version_json = cJSON_GetObjectItemCaseSensitive(root, "effectiveVersion");
    const cJSON *chapters_json = cJSON_GetObjectItemCaseSensitive(root, "chapters");
    const cJSON *source_kind_json = cJSON_GetObjectItemCaseSensitive(root, "sourceKind");
    char stored_canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (!cJSON_IsObject(root) ||
        !v3_local_json_u32(schema_json, &schema) || schema != V3_LOCAL_CONTENT_DESCRIPTOR_SCHEMA_VERSION ||
        !v3_local_json_u32(overlay_json, &stored_overlay) || stored_overlay != overlay_id ||
        !cJSON_IsString(ruid_json) || ruid_json->valuestring == NULL ||
        !tb2_ruid_canonicalize(ruid_json->valuestring, stored_canonical_ruid) ||
        osStrcmp(stored_canonical_ruid, canonical_ruid) != 0 ||
        !v3_local_json_u32(version_json, &stored_version) || stored_version != effective_version ||
        !cJSON_IsArray(chapters_json))
    {
        return ERROR_INVALID_FILE;
    }

    generation->source_kind = V3_LOCAL_CONTENT_SOURCE_DIRECT_TAF;
    if (source_kind_json != NULL)
    {
        uint32_t tap_audio_id = 0;
        uint32_t shuffle_mode = 0;
        uint32_t selection_generation = 0;
        const cJSON *source_path_json = cJSON_GetObjectItemCaseSensitive(root, "sourcePath");
        const cJSON *title_json = cJSON_GetObjectItemCaseSensitive(root, "title");
        const cJSON *tap_audio_id_json = cJSON_GetObjectItemCaseSensitive(root, "tapAudioId");
        const cJSON *shuffle_json = cJSON_GetObjectItemCaseSensitive(root, "shuffleMode");
        const cJSON *selection_json = cJSON_GetObjectItemCaseSensitive(root, "selectionGeneration");
        if (!cJSON_IsString(source_kind_json) || source_kind_json->valuestring == NULL ||
            osStrcmp(source_kind_json->valuestring, "tap") != 0 ||
            !cJSON_IsString(source_path_json) ||
            !v3_local_string_valid(source_path_json->valuestring,
                                   V3_LOCAL_CONTENT_SOURCE_PATH_MAX) ||
            !cJSON_IsString(title_json) ||
            !v3_local_string_valid(title_json->valuestring,
                                   V3_LOCAL_CONTENT_TITLE_MAX) ||
            !v3_local_json_u32(tap_audio_id_json, &tap_audio_id) ||
            !v3_local_json_u32(shuffle_json, &shuffle_mode) || shuffle_mode > 2 ||
            !v3_local_json_u32(selection_json, &selection_generation))
        {
            return ERROR_INVALID_FILE;
        }
        generation->source_path = strdup(source_path_json->valuestring);
        generation->title = strdup(title_json->valuestring);
        if (generation->source_path == NULL || generation->title == NULL)
        {
            return ERROR_OUT_OF_MEMORY;
        }
        generation->source_kind = V3_LOCAL_CONTENT_SOURCE_TAP;
        generation->tap_audio_id = tap_audio_id;
        generation->shuffle_mode = (uint8_t)shuffle_mode;
        generation->selection_generation = selection_generation;
    }

    int chapter_count = cJSON_GetArraySize(chapters_json);
    if (chapter_count <= 0 || chapter_count > TONIEFILE_MAX_CHAPTERS)
    {
        return ERROR_INVALID_FILE;
    }
    generation->chapters = osAllocMem((size_t)chapter_count * sizeof(*generation->chapters));
    if (generation->chapters == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }
    osMemset(generation->chapters, 0, (size_t)chapter_count * sizeof(*generation->chapters));
    generation->chapter_count = (size_t)chapter_count;
    generation->overlay_id = overlay_id;
    generation->effective_version = effective_version;
    osStrcpy(generation->ruid, canonical_ruid);

    for (size_t i = 0; i < generation->chapter_count; i++)
    {
        const cJSON *entry = cJSON_GetArrayItem(chapters_json, (int)i);
        const cJSON *index_json = cJSON_GetObjectItemCaseSensitive(entry, "index");
        const cJSON *name_json = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const cJSON *sha_json = cJSON_GetObjectItemCaseSensitive(entry, "sha256");
        const cJSON *size_json = cJSON_GetObjectItemCaseSensitive(entry, "fileSize");
        size_t index = 0;
        uint32_t file_size = 0;
        v3_local_content_chapter_t *chapter = &generation->chapters[i];
        if (!cJSON_IsObject(entry) ||
            !v3_local_json_size(index_json, TONIEFILE_MAX_CHAPTERS - 1, &index) || index != i ||
            !cJSON_IsString(name_json) || name_json->valuestring == NULL ||
            osStrlen(name_json->valuestring) >= sizeof(chapter->name) ||
            !cJSON_IsString(sha_json) || sha_json->valuestring == NULL ||
            !v3_local_json_u32(size_json, &file_size) || file_size == 0)
        {
            return ERROR_INVALID_FILE;
        }

        if (generation->source_kind == V3_LOCAL_CONTENT_SOURCE_TAP)
        {
            size_t source_index = 0;
            uint32_t duration = 0;
            const cJSON *source_index_json = cJSON_GetObjectItemCaseSensitive(entry, "sourceIndex");
            const cJSON *title_json = cJSON_GetObjectItemCaseSensitive(entry, "title");
            const cJSON *duration_json = cJSON_GetObjectItemCaseSensitive(entry, "duration");
            if (!v3_local_json_size(source_index_json,
                                    UINT32_MAX,
                                    &source_index) ||
                !cJSON_IsString(title_json) ||
                !v3_local_string_valid(title_json->valuestring,
                                       V3_LOCAL_CONTENT_TITLE_MAX) ||
                !v3_local_json_u32(duration_json, &duration))
            {
                return ERROR_INVALID_FILE;
            }
            chapter->source_index = source_index;
            chapter->duration_seconds = duration;
            chapter->title = strdup(title_json->valuestring);
            if (chapter->title == NULL)
            {
                return ERROR_OUT_OF_MEMORY;
            }
        }

        chapter->index = index;
        chapter->file_size = file_size;
        osStrcpy(chapter->name, name_json->valuestring);
        if (!v3_local_parse_digest(sha_json->valuestring,
                                   chapter->sha256,
                                   chapter->sha256_hex) ||
            osStrcmp(sha_json->valuestring, chapter->sha256_hex) != 0)
        {
            return ERROR_INVALID_FILE;
        }
        error_t error = v3_local_validate_object(cache_root, chapter, canonical_ruid);
        if (error != NO_ERROR)
        {
            return error;
        }
    }
    return NO_ERROR;
}

error_t v3_local_content_generation_load(const char *cache_root,
                                         uint8_t overlay_id,
                                         const char *ruid,
                                         uint32_t effective_version,
                                         v3_local_content_generation_t *generation)
{
    if (cache_root == NULL || cache_root[0] == '\0' ||
        effective_version == 0 || generation == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(generation, 0, sizeof(*generation));

    char canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (!tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    char *descriptor_dir = v3_local_descriptor_dir(cache_root, overlay_id, canonical_ruid);
    char *descriptor_path = descriptor_dir != NULL
                                ? v3_local_descriptor_path(descriptor_dir, effective_version)
                                : NULL;
    char *data = NULL;
    uint32_t data_size = 0;
    error_t error = descriptor_dir == NULL || descriptor_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_local_read_descriptor(descriptor_path, &data, &data_size);
    cJSON *json = NULL;
    if (error == NO_ERROR)
    {
        const char *parse_end = NULL;
        json = cJSON_ParseWithLengthOpts(data, data_size, &parse_end, 0);
        if (json == NULL || parse_end != data + data_size)
        {
            error = ERROR_INVALID_FILE;
        }
    }
    if (error == NO_ERROR)
    {
        error = v3_local_generation_from_json(cache_root,
                                              overlay_id,
                                              canonical_ruid,
                                              effective_version,
                                              json,
                                              generation);
    }
    if (error != NO_ERROR)
    {
        v3_local_content_generation_free(generation);
    }

    cJSON_Delete(json);
    osFreeMem(data);
    osFreeMem(descriptor_path);
    osFreeMem(descriptor_dir);
    return error;
}

error_t v3_local_tap_state_load(const char *cache_root,
                                uint8_t overlay_id,
                                const char *ruid,
                                v3_local_tap_state_t *state)
{
    if (cache_root == NULL || cache_root[0] == '\0' || state == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    osMemset(state, 0, sizeof(*state));

    char canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (!tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    char *state_dir = v3_local_tap_state_dir(cache_root, overlay_id);
    char *state_path = state_dir != NULL
                           ? v3_local_tap_state_path(state_dir, canonical_ruid)
                           : NULL;
    char *data = NULL;
    uint32_t data_size = 0;
    error_t error = state_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_local_read_descriptor(state_path, &data, &data_size);
    cJSON *json = NULL;
    if (error == NO_ERROR)
    {
        const char *parse_end = NULL;
        json = cJSON_ParseWithLengthOpts(data, data_size, &parse_end, FALSE);
        if (!cJSON_IsObject(json) || parse_end != data + data_size)
        {
            error = ERROR_INVALID_FILE;
        }
    }

    uint32_t schema = 0;
    uint32_t stored_overlay = 0;
    uint32_t shuffle_mode = 0;
    if (error == NO_ERROR)
    {
        const cJSON *stored_ruid = cJSON_GetObjectItemCaseSensitive(json, "ruid");
        char stored_canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
        if (!v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "schema"), &schema) || schema != 1 ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "overlay"), &stored_overlay) || stored_overlay != overlay_id ||
            !cJSON_IsString(stored_ruid) || stored_ruid->valuestring == NULL ||
            !tb2_ruid_canonicalize(stored_ruid->valuestring, stored_canonical_ruid) ||
            osStrcmp(stored_canonical_ruid, canonical_ruid) != 0 ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "tapAudioId"), &state->tap_audio_id) ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "shuffleMode"), &shuffle_mode) || shuffle_mode > 2 ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "selectionGeneration"), &state->selection_generation) ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "desiredVersion"), &state->desired_version) ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "preparedVersion"), &state->prepared_version) ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "playingVersion"), &state->playing_version) ||
            !v3_local_json_u32(cJSON_GetObjectItemCaseSensitive(json, "previousVersion"), &state->previous_version) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(json, "regenerationPending")))
        {
            error = ERROR_INVALID_FILE;
        }
        else
        {
            state->shuffle_mode = (uint8_t)shuffle_mode;
            state->regeneration_pending = cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(json, "regenerationPending"));
            state->valid = state->prepared_version != 0;
            if (!state->valid)
            {
                error = ERROR_INVALID_FILE;
            }
        }
    }

    if (error != NO_ERROR)
    {
        osMemset(state, 0, sizeof(*state));
    }
    cJSON_Delete(json);
    osFreeMem(data);
    osFreeMem(state_path);
    osFreeMem(state_dir);
    return error;
}

error_t v3_local_tap_state_save(const char *cache_root,
                                uint8_t overlay_id,
                                const char *ruid,
                                const v3_local_tap_state_t *state)
{
    char canonical_ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    if (cache_root == NULL || cache_root[0] == '\0' || state == NULL ||
        !state->valid || state->prepared_version == 0 ||
        state->shuffle_mode > 2 ||
        !tb2_ruid_canonicalize(ruid, canonical_ruid))
    {
        return ERROR_INVALID_PARAMETER;
    }

    char *state_dir = v3_local_tap_state_dir(cache_root, overlay_id);
    char *state_path = state_dir != NULL
                           ? v3_local_tap_state_path(state_dir, canonical_ruid)
                           : NULL;
    char *temp_path = state_path != NULL
                          ? v3_local_format_alloc("%s.tmp", state_path)
                          : NULL;
    error_t error = state_dir == NULL || state_path == NULL || temp_path == NULL
                        ? ERROR_OUT_OF_MEMORY
                        : v3_local_ensure_dir(state_dir);
    cJSON *json = NULL;
    char *serialized = NULL;
    if (error == NO_ERROR)
    {
        json = cJSON_CreateObject();
        if (json == NULL ||
            cJSON_AddNumberToObject(json, "schema", 1) == NULL ||
            cJSON_AddNumberToObject(json, "overlay", overlay_id) == NULL ||
            cJSON_AddStringToObject(json, "ruid", canonical_ruid) == NULL ||
            cJSON_AddNumberToObject(json, "tapAudioId", state->tap_audio_id) == NULL ||
            cJSON_AddNumberToObject(json, "shuffleMode", state->shuffle_mode) == NULL ||
            cJSON_AddNumberToObject(json, "selectionGeneration", state->selection_generation) == NULL ||
            cJSON_AddNumberToObject(json, "desiredVersion", state->desired_version) == NULL ||
            cJSON_AddNumberToObject(json, "preparedVersion", state->prepared_version) == NULL ||
            cJSON_AddNumberToObject(json, "playingVersion", state->playing_version) == NULL ||
            cJSON_AddNumberToObject(json, "previousVersion", state->previous_version) == NULL ||
            cJSON_AddBoolToObject(json, "regenerationPending", state->regeneration_pending) == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }
    if (error == NO_ERROR)
    {
        serialized = cJSON_PrintUnformatted(json);
        if (serialized == NULL)
        {
            error = ERROR_OUT_OF_MEMORY;
        }
    }
    if (error == NO_ERROR)
    {
        mutex_lock_id(state_path);
        FsFile *file = fsOpenFile(temp_path,
                                  FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
        error = file != NULL
                    ? fsWriteFile(file, serialized, osStrlen(serialized))
                    : ERROR_FILE_OPENING_FAILED;
        if (file != NULL)
        {
            if (error == NO_ERROR)
            {
                error = fsFlushFile(file);
            }
            fsCloseFile(file);
        }
        if (error == NO_ERROR)
        {
            error = fsMoveFile(temp_path, state_path, TRUE);
        }
        if (error != NO_ERROR && fsFileExists(temp_path))
        {
            fsDeleteFile(temp_path);
        }
        mutex_unlock_id(state_path);
    }

    cJSON_free(serialized);
    cJSON_Delete(json);
    osFreeMem(temp_path);
    osFreeMem(state_path);
    osFreeMem(state_dir);
    return error;
}
