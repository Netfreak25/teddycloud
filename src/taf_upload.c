#include "taf_upload.h"
#include "encoder_limits.h"
#include "fs_ext.h"
#include "fs_port.h"
#include "hash/sha1.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"
#include "rand.h"
#include "server_helpers.h"
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct
{
    const char *path;
    FsFile *file;
    size_t size;
    bool_t started;
    bool_t completed;
} taf_upload_t;

bool_t taf_upload_validate(const char *path)
{
    uint32_t size;
    if (fsGetFileSize(path, &size) != NO_ERROR || size <= ENCODER_TAF_HEADER_SIZE ||
        size > ENCODER_MAX_TAF_UPLOAD_BYTES)
        return FALSE;
    FsFile *file = fsOpenFile(path, FS_FILE_MODE_READ);
    if (!file)
        return FALSE;
    uint8_t buffer[ENCODER_TAF_HEADER_SIZE] = {0};
    size_t read = 0;
    error_t error = fsReadFile(file, buffer, sizeof(buffer), &read);
    uint32_t length = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                      ((uint32_t)buffer[2] << 8) | buffer[3];
    TonieboxAudioFileHeader *header = NULL;
    if (!error && read == sizeof(buffer) && length > 0 && length <= ENCODER_TAF_PROTO_BYTES)
        header = toniebox_audio_file_header__unpack(NULL, length, buffer + 4);
    bool_t valid = header && header->num_bytes == size - sizeof(buffer) &&
                   header->sha1_hash.len == SHA1_DIGEST_SIZE &&
                   header->n_track_page_nums <= ENCODER_TAF_MAX_CHAPTERS;
    for (size_t i = 0; valid && i < header->n_track_page_nums; i++)
    {
        uint32_t page = header->track_page_nums[i];
        valid = (i ? page >= header->track_page_nums[i - 1] : page == 0) &&
                (uint64_t)page * ENCODER_TAF_HEADER_SIZE < header->num_bytes;
    }
    Sha1Context hash;
    sha1Init(&hash);
    size_t total = 0;
    while (valid && total < header->num_bytes)
    {
        read = 0;
        error = fsReadFile(file, buffer, sizeof(buffer), &read);
        if ((error && error != ERROR_END_OF_FILE) || !read)
        {
            valid = FALSE;
            break;
        }
        total += read;
        sha1Update(&hash, buffer, read);
    }
    uint8_t digest[SHA1_DIGEST_SIZE];
    sha1Final(&hash, digest);
    valid = valid && total == header->num_bytes && !osMemcmp(digest, header->sha1_hash.data, sizeof(digest));
    if (header)
        toniebox_audio_file_header__free_unpacked(header, NULL);
    fsCloseFile(file);
    return valid;
}

static error_t taf_upload_start(void *context, const char *name, const char *filename)
{
    (void)name;
    (void)filename;
    taf_upload_t *upload = context;
    if (upload->started)
        return ERROR_INVALID_REQUEST;
    upload->started = TRUE;
    upload->file = fsOpenFile(upload->path, FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    return upload->file ? NO_ERROR : ERROR_FILE_OPENING_FAILED;
}

static error_t taf_upload_add(void *context, void *data, size_t size)
{
    taf_upload_t *upload = context;
    if (!upload->file || size > ENCODER_MAX_TAF_UPLOAD_BYTES - upload->size)
        return ERROR_INVALID_LENGTH;
    error_t error = fsWriteFile(upload->file, data, size);
    if (!error)
        upload->size += size;
    return error;
}

static error_t taf_upload_end(void *context)
{
    taf_upload_t *upload = context;
    error_t error = fsFlushFile(upload->file);
    fsCloseFile(upload->file);
    upload->file = NULL;
    upload->completed = !error;
    return error;
}

error_t taf_upload_receive(HttpConnection *connection, const char *target)
{
    uint8_t nonce[16];
    error_t error = rand_get_algo()->read(rand_get_context(), nonce, sizeof(nonce));
    if (error)
        return error;
    char hex[sizeof(nonce) * 2 + 1];
    for (size_t i = 0; i < sizeof(nonce); i++)
        osSnprintf(hex + i * 2, 3, "%02x", nonce[i]);
    char *temporary = custom_asprintf("%s.upload-%s.tmp", target, hex);
    if (!temporary)
        return ERROR_OUT_OF_MEMORY;
    if (fsFileExists(temporary))
    {
        osFreeMem(temporary);
        return ERROR_FAILURE;
    }
    taf_upload_t upload = {.path = temporary};
    multipart_cbr_t callbacks = {taf_upload_start, taf_upload_add, taf_upload_end};
    error = multipart_handle_strict(connection, &callbacks, &upload);
    if (upload.file)
        fsCloseFile(upload.file);
    if (!error && (!upload.completed || !taf_upload_validate(temporary)))
        error = ERROR_INVALID_FILE;
    if (!error)
    {
#ifdef _WIN32
        error = MoveFileExA(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? NO_ERROR : ERROR_FAILURE;
#else
        error = fsRenameFile(temporary, target);
#endif
    }
    if (fsFileExists(temporary))
        fsDeleteFile(temporary);
    osFreeMem(temporary);
    return error;
}
