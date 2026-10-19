/* Real production module, files, protobuf, SHA and mutex; fault injection at I/O. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define TRACE_LEVEL 0
#include "fs_ext.h"

static unsigned writes, fail_write_number;
static bool fail_flush, fail_rename, fail_delete;
static error_t test_write(FsFile *file, void *data, size_t size)
{
    if (++writes == fail_write_number)
        return ERROR_WRITE_FAILED;
    return fsWriteFile(file, data, size);
}
static error_t test_flush(FsFile *file)
{
    return fail_flush ? ERROR_WRITE_FAILED : fsFlushFile(file);
}
static error_t test_rename(const char *from, const char *to)
{
    return fail_rename ? ERROR_WRITE_FAILED : fsRenameFile(from, to);
}
static error_t test_delete(const char *path)
{
    return fail_delete && !strcmp(path, "content/8194F21C/500304E0") ? ERROR_WRITE_FAILED : fsDeleteFile(path);
}
#define fsWriteFile test_write
#define fsFlushFile test_flush
#define fsRenameFile test_rename
#define fsDeleteFile test_delete
#include "../src/taf_original_cache.c"
#undef fsWriteFile
#undef fsFlushFile
#undef fsRenameFile
#undef fsDeleteFile

static settings_t global_settings;
static OsMutex import_mutex;
static const char *raw_path = "content/8194F21C/500304E0";
static const char *library_path = "library/by/audioID/123456.taf";

settings_t *get_settings(void) { return &global_settings; }
void mutex_lock(mutex_id_t id) { assert(id == MUTEX_TAF_ORIGINAL_CACHE); osAcquireMutex(&import_mutex); }
void mutex_unlock(mutex_id_t id) { assert(id == MUTEX_TAF_ORIGINAL_CACHE); osReleaseMutex(&import_mutex); }

static void write_taf(const char *path, uint32_t audio_id, uint8_t seed)
{
    char *directory = strdup(path);
    assert(directory);
    pathRemoveFilename(directory);
    assert(fsDirExists(directory) || fsCreateDirEx(directory, true) == NO_ERROR);
    free(directory);
    uint8_t payload[TONIEFILE_FRAME_SIZE * 2], digest[SHA1_DIGEST_SIZE];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i + seed);
    assert(!sha1Compute(payload, sizeof(payload), digest));
    uint32_t page = 0;
    TonieboxAudioFileHeader header = TONIEBOX_AUDIO_FILE_HEADER__INIT;
    header.sha1_hash = (ProtobufCBinaryData){sizeof(digest), digest};
    header.num_bytes = sizeof(payload);
    header.audio_id = audio_id;
    header.n_track_page_nums = 1;
    header.track_page_nums = &page;
    uint8_t bytes[TONIEFILE_FRAME_SIZE] = {0};
    size_t length = toniebox_audio_file_header__pack(&header, bytes + 4);
    bytes[0] = (uint8_t)(length >> 24); bytes[1] = (uint8_t)(length >> 16);
    bytes[2] = (uint8_t)(length >> 8); bytes[3] = (uint8_t)length;
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    assert(fwrite(payload, 1, sizeof(payload), file) == sizeof(payload));
    assert(!fclose(file));
}

static void expect_resolved(const char *raw, const settings_t *settings, const char *expected)
{
    char *resolved = taf_original_cache_resolve(raw, settings);
    assert(expected ? resolved && !strcmp(resolved, expected) : !resolved);
    osFreeMem(resolved);
}

static char *read_reference(void)
{
    char *reference = taf_original_reference_path(raw_path, &global_settings);
    assert(reference);
    FILE *file = fopen(reference, "rb");
    assert(file);
    char *body = calloc(TAF_REFERENCE_MAX_BYTES + 1, 1);
    assert(body && fread(body, 1, TAF_REFERENCE_MAX_BYTES, file));
    fclose(file);
    free(reference);
    return body;
}

static void write_reference(const char *body)
{
    char *reference = taf_original_reference_path(raw_path, &global_settings);
    FILE *file = fopen(reference, "wb");
    assert(file && fwrite(body, 1, strlen(body), file) == strlen(body));
    assert(!fclose(file));
    free(reference);
}

static void import_and_check(void)
{
    write_taf(raw_path, 123456, 2);
    assert(!taf_original_cache_import(raw_path, &global_settings));
    assert(!fsFileExists(raw_path) && fsFileExists(library_path));
    expect_resolved(raw_path, &global_settings, library_path);
    char *body = read_reference();
    assert(strstr(body, "lib://by/audioID/123456.taf"));
    free(body);
}

static void *import_thread(void *path)
{
    assert(!taf_original_cache_import(path, &global_settings));
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && osCreateMutex(&import_mutex));
    global_settings.internal.cachedirfull = "cache";
    global_settings.internal.contentdirfull = "content";
    global_settings.internal.librarydirfull = "library";
    const char *test = argv[1];
    if (!strcmp(test, "readonly"))
    {
        assert(!taf_original_file_complete(NULL));
        assert(!taf_original_file_complete(""));
        assert(!taf_original_file_complete(raw_path));
        expect_resolved(raw_path, &global_settings, NULL);
        assert(access("cache", F_OK) && access("library", F_OK));
        assert(taf_original_cache_import("outside/8194F21C/500304E0", &global_settings) == ERROR_INVALID_PATH);
        assert(taf_original_cache_import("content/8194F21C/500304E0.tmp", &global_settings) == ERROR_INVALID_PATH);
        assert(access("cache", F_OK) && access("library", F_OK));
    }
    else if (!strcmp(test, "import"))
    {
        import_and_check();
        settings_t overlay = global_settings;
        overlay.internal.cachedirfull = "unused-overlay-cache";
        overlay.internal.contentdirfull = "content/./";
        expect_resolved("content/8194f21c/500304e0", &overlay, library_path);
        assert(access("unused-overlay-cache", F_OK));
        write_taf(raw_path, 123456, 2);
        assert(!taf_original_cache_import(raw_path, &overlay));
        assert(!fsFileExists(raw_path));
    }
    else if (!strcmp(test, "restart"))
    {
        assert(!fsFileExists(raw_path));
        expect_resolved(raw_path, &global_settings, library_path);
    }
    else if (!strcmp(test, "collision"))
    {
        import_and_check();
        char *before = read_reference();
        write_taf(raw_path, 123456, 3);
        assert(taf_original_cache_import(raw_path, &global_settings) == ERROR_WRONG_CHECKSUM);
        assert(fsFileExists(raw_path));
        char *after = read_reference();
        assert(!strcmp(before, after));
        free(before); free(after);
        expect_resolved(raw_path, &global_settings, library_path);
    }
    else if (!strcmp(test, "corrupt"))
    {
        write_taf(raw_path, 123456, 2);
        assert(taf_original_file_complete(raw_path));
        FILE *file = fopen(raw_path, "r+b");
        assert(file && !fseek(file, TONIEFILE_FRAME_SIZE, SEEK_SET));
        assert(fputc(99, file) != EOF && !fclose(file));
        assert(taf_original_file_complete(raw_path)); /* Cheap probe deliberately does not hash audio. */
        assert(taf_original_cache_import(raw_path, &global_settings) == ERROR_WRONG_CHECKSUM);
        assert(!truncate(raw_path, TONIEFILE_FRAME_SIZE + 1));
        assert(!taf_original_file_complete(raw_path));
        assert(taf_original_cache_import(raw_path, &global_settings) == ERROR_INVALID_FILE);
        assert(fsFileExists(raw_path) && !fsFileExists(library_path));
        write_taf(raw_path, 123456, 2);
        file = fopen(raw_path, "r+b");
        assert(file && fputc(255, file) != EOF && !fclose(file));
        assert(!taf_original_file_complete(raw_path)); /* Invalid protobuf-header length. */
        write_taf(raw_path, 123456, 2);
        assert(!truncate(raw_path, TONIEFILE_FRAME_SIZE * 3 + 1));
        assert(!taf_original_file_complete(raw_path)); /* A trailing byte also violates exact size. */
    }
    else if (!strcmp(test, "invalid-library"))
    {
        import_and_check();
        assert(!truncate(library_path, (off_t)UINT32_MAX + 1 + TONIEFILE_FRAME_SIZE * 3));
        expect_resolved(raw_path, &global_settings, NULL);
        assert(!truncate(library_path, TONIEFILE_FRAME_SIZE + 1));
        expect_resolved(raw_path, &global_settings, NULL);
        assert(!fsDeleteFile(library_path));
        expect_resolved(raw_path, &global_settings, NULL);
        char *body = read_reference(); free(body); /* Invalid references are not deleted. */
    }
    else if (!strcmp(test, "invalid-reference"))
    {
        import_and_check();
        const char *invalid[] = {"{", "{}", "{\"_version\":2}",
            "{\"_version\":1,\"audioId\":123456,\"size\":12288,\"headerSha1\":\"bad\",\"source\":\"lib://../../escape.taf\"}"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        {
            write_reference(invalid[i]);
            expect_resolved(raw_path, &global_settings, NULL);
        }
        assert(fsFileExists(library_path));
    }
    else if (!strcmp(test, "copy-failure") || !strcmp(test, "reference-failure") || !strcmp(test, "flush-failure"))
    {
        write_taf(raw_path, 123456, 2);
        fail_write_number = !strcmp(test, "copy-failure") ? 1 : 4;
        fail_flush = !strcmp(test, "flush-failure");
        assert(taf_original_cache_import(raw_path, &global_settings) != NO_ERROR);
        assert(fsFileExists(raw_path));
        expect_resolved(raw_path, &global_settings, NULL);
        fail_write_number = 0; fail_flush = false;
        assert(!taf_original_cache_import(raw_path, &global_settings));
        expect_resolved(raw_path, &global_settings, library_path);
    }
    else if (!strcmp(test, "rename-failure"))
    {
        import_and_check();
        char *before = read_reference();
        write_taf(raw_path, 234567, 4);
        fail_rename = true;
        assert(taf_original_cache_import(raw_path, &global_settings) != NO_ERROR);
        assert(fsFileExists(raw_path));
        char *after = read_reference();
        assert(!strcmp(before, after)); free(before); free(after);
        expect_resolved(raw_path, &global_settings, library_path);
        fail_rename = false;
        assert(!taf_original_cache_import(raw_path, &global_settings));
        expect_resolved(raw_path, &global_settings, "library/by/audioID/234567.taf");
    }
    else if (!strcmp(test, "cleanup-failure"))
    {
        write_taf(raw_path, 123456, 2);
        fail_delete = true;
        assert(taf_original_cache_import(raw_path, &global_settings) != NO_ERROR);
        assert(fsFileExists(raw_path));
        expect_resolved(raw_path, &global_settings, library_path);
        fail_delete = false;
        assert(!taf_original_cache_import(raw_path, &global_settings));
        assert(!fsFileExists(raw_path));
    }
    else if (!strcmp(test, "scope"))
    {
        import_and_check();
        settings_t overlay = global_settings;
        overlay.internal.contentdirfull = "other-content";
        overlay.internal.cachedirfull = "unused-overlay-cache";
        const char *other_raw = "other-content/8194F21C/500304E0";
        expect_resolved(other_raw, &overlay, NULL);
        write_taf(other_raw, 234567, 3);
        assert(!taf_original_cache_import(other_raw, &overlay));
        expect_resolved(other_raw, &overlay, "library/by/audioID/234567.taf");
        expect_resolved(raw_path, &global_settings, library_path);
    }
    else if (!strcmp(test, "concurrent"))
    {
        const char *other_raw = "content/8194F21C/500304E1";
        write_taf(raw_path, 123456, 2); write_taf(other_raw, 123456, 2);
        pthread_t a, b;
        assert(!pthread_create(&a, NULL, import_thread, (void *)raw_path));
        assert(!pthread_create(&b, NULL, import_thread, (void *)other_raw));
        assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
        expect_resolved(raw_path, &global_settings, library_path);
        expect_resolved(other_raw, &global_settings, library_path);
    }
    else
        assert(!"unknown case");
    osDeleteMutex(&import_mutex);
    printf("PASS TAF original reference: %s\n", test);
    return 0;
}
