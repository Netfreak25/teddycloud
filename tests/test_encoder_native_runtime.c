/* Production encoder HTTP handlers, filesystem publisher and Ogg/Opus parser.
 * Only HTTP transport, settings, randomness and locking are fixture boundaries.
 * --bridge keeps slots alive and accepts JSONL commands for browser smoke tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"
#include "mutex_manager.h"
#include "taf_upload.h"
#include "cJSON.h"
#include "hash/sha1.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"
#undef TRACE_LEVEL
#include "../src/encoder_api.c"

static settings_t test_settings;
static unsigned held[MUTEX_LAST];
static FILE *request_body;
static char *response_body;
static unsigned random_counter;
static size_t transport_chunk = 137;
static bool_t fail_flush;

error_t __real_fsFlushFile(FsFile *file);
error_t __wrap_fsFlushFile(FsFile *file)
{
    return fail_flush ? ERROR_WRITE_FAILED : __real_fsFlushFile(file);
}

void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *pointer) { free(pointer); }
void osSuspendAllTasks(void) {}
void osResumeAllTasks(void) {}
settings_t *get_settings(void) { return &test_settings; }
char *error2text(error_t error) { (void)error; return "fixture transport error"; }
void mutex_lock(mutex_id_t id)
{
    assert(id < MUTEX_LAST && held[id] == 0);
    held[id]++;
}
void mutex_unlock(mutex_id_t id)
{
    assert(id < MUTEX_LAST && held[id] == 1);
    held[id]--;
}
static error_t fixture_random(void *context, uint8_t *output, size_t length)
{
    (void)context;
    memset(output, 0, length);
    unsigned value = ++random_counter;
    memcpy(output, &value, MIN(sizeof(value), length));
    return NO_ERROR;
}
static const PrngAlgo fixture_prng = {.read = fixture_random};
const PrngAlgo *rand_get_algo(void) { return &fixture_prng; }
void *rand_get_context(void) { return NULL; }

error_t httpReceive(HttpConnection *connection, void *data, size_t size,
                    size_t *received, uint_t flags)
{
    (void)flags;
    assert(!held[MUTEX_ENCODER_UPLOAD]);
    size = MIN(size, transport_chunk);
    size = MIN(size, connection->request.byteCount);
    *received = request_body ? fread(data, 1, size, request_body) : 0;
    return *received ? NO_ERROR : ERROR_END_OF_STREAM;
}
error_t httpReadStream(HttpConnection *connection, void *data, size_t size,
                       size_t *received, uint_t flags)
{
    error_t error = httpReceive(connection, data, MIN(size, connection->request.byteCount), received, flags);
    if (!error) connection->request.byteCount -= *received;
    return error;
}
void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{
    (void)type; (void)length;
    connection->response.statusCode = 200;
    connection->response.keepAlive = TRUE;
}
error_t httpWriteResponse(HttpConnection *connection, void *data, size_t length, bool_t release)
{
    (void)connection;
    assert(!held[MUTEX_ENCODER_UPLOAD]);
    free(response_body);
    response_body = malloc(length + 1);
    assert(response_body);
    memcpy(response_body, data, length);
    response_body[length] = 0;
    if (release) free(data);
    return NO_ERROR;
}

static cJSON *request(const char *method, const char *uri, const char *body_path,
                      const char *body_text, const char *boundary)
{
    HttpConnection connection = {0};
    assert(strlen(method) < sizeof(connection.request.method));
    strcpy(connection.request.method, method);
    if (boundary)
    {
        assert(strlen(boundary) < sizeof(connection.request.boundary));
        strcpy(connection.request.boundary, boundary);
        connection.request.boundaryLength = strlen(boundary);
    }
    if (body_path)
        request_body = fopen(body_path, "rb");
    else
    {
        request_body = tmpfile();
        if (body_text) assert(fwrite(body_text, 1, strlen(body_text), request_body) == strlen(body_text));
        rewind(request_body);
    }
    assert(request_body);
    struct stat information;
    assert(fstat(fileno(request_body), &information) == 0);
    connection.request.byteCount = (size_t)information.st_size;
    free(response_body); response_body = NULL;
    error_t error;
    if (!strcmp(uri, "/api/encoder/capabilities"))
        error = handleApiEncoderCapabilities(&connection, uri, "", NULL);
    else if (!strcmp(uri, "/api/library/native/import"))
        error = handleApiNativeImportStart(&connection, uri, "", NULL);
    else if (!strncmp(uri, "/api/tafUpload", strlen("/api/tafUpload")))
    {
        char *target = custom_asprintf("%s/browser-upload.taf", test_settings.internal.librarydirfull);
        error = taf_upload_receive(&connection, target);
        free(target);
        connection.response.statusCode = error ? 400 : 200;
        response_body = strdup(error ? "{\"ok\":false}" : "{\"ok\":true}");
        error = NO_ERROR;
    }
    else
        error = handleApiNativeImportOperation(&connection, uri, "", NULL);
    fclose(request_body); request_body = NULL;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "status", error ? 500 : connection.response.statusCode);
    cJSON *body = response_body ? cJSON_Parse(response_body) : NULL;
    cJSON_AddItemToObject(result, "body", body ? body : cJSON_CreateString("no response"));
    return result;
}

static cJSON *checked_request(unsigned status, const char *method, const char *uri,
                              const char *file, const char *text)
{
    cJSON *result = request(method, uri, file, text, NULL);
    unsigned actual = (unsigned)cJSON_GetObjectItemCaseSensitive(result, "status")->valueint;
    if (actual != status)
    {
        char *json = cJSON_PrintUnformatted(result);
        fprintf(stderr, "expected %u, got %s\n", status, json);
        free(json);
    }
    assert(actual == status);
    cJSON *body = cJSON_DetachItemFromObjectCaseSensitive(result, "body");
    cJSON_Delete(result);
    return body;
}

static char *start(size_t count, const char *title)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "title", title);
    cJSON *chapters = cJSON_AddArrayToObject(json, "chapters");
    for (size_t i = 0; i < count; i++)
    {
        cJSON *chapter = cJSON_CreateObject();
        char name[64]; snprintf(name, sizeof(name), "Chapter %zu", i + 1);
        cJSON_AddStringToObject(chapter, "title", name);
        cJSON_AddItemToArray(chapters, chapter);
    }
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    cJSON *response = checked_request(201, "POST", "/api/library/native/import", NULL, text);
    free(text);
    char *id = strdup(cJSON_GetObjectItemCaseSensitive(response, "id")->valuestring);
    assert(strlen(id) == ENCODER_UPLOAD_ID_LENGTH);
    cJSON_Delete(response);
    return id;
}

static cJSON *operation(unsigned status, const char *id, const char *suffix, const char *file)
{
    char *uri = custom_asprintf("%s%s%s", ENCODER_UPLOAD_PREFIX, id, suffix);
    cJSON *response = checked_request(status, *suffix ? "POST" : "DELETE", uri, file, NULL);
    free(uri);
    return response;
}

static void write_packet(FILE *file, ogg_stream_state *stream, unsigned char *data,
                         long length, long number, ogg_int64_t granule, int eos)
{
    ogg_packet packet = {.packet = data, .bytes = length, .b_o_s = number == 0,
        .e_o_s = eos, .granulepos = granule, .packetno = number};
    assert(ogg_stream_packetin(stream, &packet) == 0);
    ogg_page page;
    while (ogg_stream_flush(stream, &page))
    {
        assert(fwrite(page.header, 1, page.header_len, file) == (size_t)page.header_len);
        assert(fwrite(page.body, 1, page.body_len, file) == (size_t)page.body_len);
    }
}

static void opus_fixture(const char *path)
{
    FILE *file = fopen(path, "wb"); assert(file);
    ogg_stream_state stream; assert(ogg_stream_init(&stream, 123) == 0);
    unsigned char header[19] = {'O','p','u','s','H','e','a','d',1,2,0x38,1,0x80,0xbb,0,0,0,0,0};
    unsigned char tags[16] = {'O','p','u','s','T','a','g','s',0,0,0,0,0,0,0,0};
    unsigned char audio[] = {0xf8, 0xff, 0xfe};
    write_packet(file, &stream, header, sizeof(header), 0, 0, 0);
    write_packet(file, &stream, tags, sizeof(tags), 1, 0, 0);
    write_packet(file, &stream, audio, sizeof(audio), 2, 960, 0);
    write_packet(file, &stream, audio, sizeof(audio), 3, 1920, 1);
    ogg_stream_clear(&stream); fclose(file);
}

static void test_validation(void)
{
    opus_fixture("valid.opus");
    double duration = 0;
    assert(encoder_validate_opus_file("valid.opus", &duration) == NO_ERROR);
    assert(duration > 0.033 && duration < 0.034);
    FILE *file = fopen("invalid.opus", "wb"); assert(file);
    assert(fwrite("OggSbroken", 1, 10, file) == 10); fclose(file);
    assert(encoder_validate_opus_file("invalid.opus", &duration) != NO_ERROR);
    opus_fixture("truncated.opus");
    struct stat info; assert(stat("truncated.opus", &info) == 0);
    assert(truncate("truncated.opus", info.st_size - 1) == 0);
    assert(encoder_validate_opus_file("truncated.opus", &duration) != NO_ERROR);
    opus_fixture("trailing.opus");
    file = fopen("trailing.opus", "ab"); assert(file); fputc(1, file); fclose(file);
    assert(encoder_validate_opus_file("trailing.opus", &duration) != NO_ERROR);
}

static void test_import_and_reuse(void)
{
    const size_t count = 114;
    char *saved_source = NULL;
    for (unsigned pass = 0; pass < 2; pass++)
    {
        char *id = start(count, pass ? "Must not replace metadata" : "Original title");
        cJSON_Delete(operation(409, id, "/finish", NULL));
        cJSON_Delete(operation(409, id, "/chapter/1", "valid.opus"));
        for (size_t i = 0; i < count; i++)
        {
            char suffix[64]; snprintf(suffix, sizeof(suffix), "/chapter/%zu", i);
            cJSON *response = operation(200, id, suffix, "valid.opus");
            assert(cJSON_GetObjectItemCaseSensitive(response, "nextIndex")->valueint == (int)i + 1);
            cJSON_Delete(response);
        }
        cJSON *response = operation(pass ? 200 : 201, id, "/finish", NULL);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "reused")) == (pass != 0));
        const char *source = cJSON_GetObjectItemCaseSensitive(response, "source")->valuestring;
        if (!pass) saved_source = strdup(source);
        else assert(!strcmp(saved_source, source));
        v3_native_library_collection_t collection;
        assert(v3_native_library_collection_load(test_settings.internal.librarydirfull, source, TRUE, &collection) == NO_ERROR);
        assert(collection.chapter_count == count && collection.origin_count == 0);
        assert(!strcmp(collection.title, "Original title"));
        assert(!strcmp(collection.chapters[113].title, "Chapter 114"));
        assert(collection.chapters[113].has_duration && collection.chapters[113].duration_seconds > 0.033);
        assert(strstr(collection.chapters[100].path, "_100.opus"));
        assert(fsCompareFiles(collection.chapters[113].path, "valid.opus", NULL) == NO_ERROR);
        v3_native_library_collection_free(&collection);
        cJSON_Delete(response);
        cJSON_Delete(operation(404, id, "/finish", NULL));
        free(id);
    }
    free(saved_source);
}

static void test_limits_and_cleanup(void)
{
    cJSON *cap = checked_request(200, "GET", "/api/encoder/capabilities", NULL, NULL);
    cJSON *native = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cap, "formats"), "native");
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(native, "maxFiles")));
    cJSON_Delete(cap);
    char *ids[ENCODER_UPLOAD_SLOTS];
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++) ids[i] = start(1, "Pending");
    cJSON_Delete(checked_request(503, "POST", "/api/library/native/import", NULL,
        "{\"title\":\"Full\",\"chapters\":[{\"title\":\"Track\"}]}"));
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++)
    {
        cJSON_Delete(operation(200, ids[i], "", NULL)); free(ids[i]);
    }
    char *id = start(1, "Invalid");
    cJSON_Delete(operation(422, id, "/chapter/0", "invalid.opus"));
    cJSON_Delete(operation(404, id, "", NULL)); free(id);
    id = start(1, "Failed flush");
    fail_flush = TRUE;
    cJSON_Delete(operation(422, id, "/chapter/0", "valid.opus"));
    fail_flush = FALSE;
    cJSON_Delete(operation(404, id, "", NULL)); free(id);
    id = start(1, "Expired");
    encoder_upload_t *slot = NULL;
    for (size_t i = 0; i < ENCODER_UPLOAD_SLOTS; i++) if (encoder_uploads[i].used) slot = &encoder_uploads[i];
    assert(slot);
    char *directory = strdup(slot->directory);
    slot->last_activity -= ENCODER_UPLOAD_IDLE_SECONDS * 1000U;
    cJSON_Delete(operation(404, id, "", NULL));
    assert(!fsDirExists(directory)); free(directory); free(id);
    char *orphan = custom_asprintf("%s/%s/ffffffffffffffffffffffffffffffff/chapters",
        test_settings.internal.librarydirfull, ENCODER_UPLOAD_DIRECTORY);
    assert(fsCreateDirEx(orphan, TRUE) == NO_ERROR);
    encoder_uploads_init(); assert(!fsDirExists(orphan)); free(orphan);
    cJSON_Delete(checked_request(400, "POST", "/api/library/native/import", NULL,
        "{\"title\":\"Empty\",\"chapters\":[]}garbage"));
    cJSON *oversized = cJSON_CreateObject();
    cJSON_AddStringToObject(oversized, "title", "Oversized metadata");
    cJSON *chapters = cJSON_AddArrayToObject(oversized, "chapters");
    for (size_t i = 0; i < 5000; i++)
    {
        cJSON *chapter = cJSON_CreateObject();
        cJSON_AddStringToObject(chapter, "title", "Metadata preflight");
        cJSON_AddItemToArray(chapters, chapter);
    }
    char *text = cJSON_PrintUnformatted(oversized); cJSON_Delete(oversized);
    cJSON_Delete(checked_request(413, "POST", "/api/library/native/import", NULL, text));
    free(text);
}

static void taf_fixture(const char *path, bool_t bad_hash, bool_t bad_chapter)
{
    unsigned char payload[1024] = {0};
    /* A bare boundary or an invalid delimiter suffix is ordinary binary data. */
    memcpy(payload + 20, "fixture-boundary", sizeof("fixture-boundary"));
    memcpy(payload + 60, "\r\n--fixture-boundaryXX", sizeof("\r\n--fixture-boundaryXX"));
    uint8_t hash[SHA1_DIGEST_SIZE];
    sha1Compute(payload, sizeof(payload), hash);
    if (bad_hash) hash[0] ^= 1;
    uint32_t page = bad_chapter ? 1 : 0;
    TonieboxAudioFileHeader header = TONIEBOX_AUDIO_FILE_HEADER__INIT;
    header.audio_id = 123;
    header.num_bytes = sizeof(payload);
    header.sha1_hash.data = hash; header.sha1_hash.len = sizeof(hash);
    header.track_page_nums = &page; header.n_track_page_nums = 1;
    uint8_t data[ENCODER_TAF_HEADER_SIZE] = {0};
    size_t length = toniebox_audio_file_header__pack(&header, data + 4);
    data[0] = length >> 24; data[1] = length >> 16; data[2] = length >> 8; data[3] = length;
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    assert(fwrite(payload, 1, sizeof(payload), file) == sizeof(payload)); fclose(file);
}

static cJSON *taf_multipart_request(const char *path, bool_t complete)
{
    FILE *input = fopen(path, "rb"); assert(input);
    FILE *output = fopen("taf-multipart.body", "wb"); assert(output);
    fputs("--fixture-boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.taf\"\r\nContent-Type: application/octet-stream\r\n\r\n", output);
    uint8_t buffer[1024]; size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0)
        assert(fwrite(buffer, 1, count, output) == count);
    if (complete) fputs("\r\n--fixture-boundary--\r\n", output);
    fclose(input); fclose(output);
    return request("POST", "/api/tafUpload", "taf-multipart.body", NULL, "fixture-boundary");
}

static void test_taf_atomic_upload(void)
{
    taf_fixture("valid.taf", FALSE, FALSE);
    taf_fixture("bad-hash.taf", TRUE, FALSE);
    taf_fixture("bad-chapter.taf", FALSE, TRUE);
    assert(taf_upload_validate("valid.taf"));
    assert(!taf_upload_validate("bad-hash.taf"));
    assert(!taf_upload_validate("bad-chapter.taf"));
    cJSON *response = taf_multipart_request("valid.taf", TRUE);
    assert(cJSON_GetObjectItemCaseSensitive(response, "status")->valueint == 200);
    cJSON_Delete(response);
    char *target = custom_asprintf("%s/browser-upload.taf", test_settings.internal.librarydirfull);
    const char *invalid[] = {"bad-hash.taf", "bad-chapter.taf", "valid.taf"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        response = taf_multipart_request(invalid[i], i != 2);
        assert(cJSON_GetObjectItemCaseSensitive(response, "status")->valueint == 400);
        cJSON_Delete(response);
        assert(fsCompareFiles(target, "valid.taf", NULL) == NO_ERROR);
    }
    fail_flush = TRUE;
    response = taf_multipart_request("valid.taf", TRUE);
    fail_flush = FALSE;
    assert(cJSON_GetObjectItemCaseSensitive(response, "status")->valueint == 400);
    cJSON_Delete(response);
    assert(fsCompareFiles(target, "valid.taf", NULL) == NO_ERROR);
    free(target);
}

static void bridge(void)
{
    char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0)
    {
        cJSON *command = cJSON_Parse(line); assert(cJSON_IsObject(command));
        cJSON *method = cJSON_GetObjectItemCaseSensitive(command, "method");
        cJSON *uri = cJSON_GetObjectItemCaseSensitive(command, "uri");
        cJSON *file = cJSON_GetObjectItemCaseSensitive(command, "bodyFile");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(command, "body");
        cJSON *boundary = cJSON_GetObjectItemCaseSensitive(command, "boundary");
        assert(cJSON_IsString(method) && cJSON_IsString(uri));
        cJSON *result = request(method->valuestring, uri->valuestring,
            cJSON_IsString(file) ? file->valuestring : NULL,
            cJSON_IsString(body) ? body->valuestring : NULL,
            cJSON_IsString(boundary) ? boundary->valuestring : NULL);
        char *text = cJSON_PrintUnformatted(result); puts(text); fflush(stdout);
        free(text); cJSON_Delete(result); cJSON_Delete(command);
    }
    free(line);
}

int main(int argc, char **argv)
{
    test_settings.internal.librarydirfull = argc > 2 && !strcmp(argv[1], "--bridge") ? argv[2] : "library";
    assert(fsDirExists(test_settings.internal.librarydirfull) ||
           fsCreateDirEx(test_settings.internal.librarydirfull, TRUE) == NO_ERROR);
    encoder_uploads_init();
    if (argc > 1 && !strcmp(argv[1], "--bridge")) bridge();
    else if (argc == 3 && !strcmp(argv[1], "--validate-opus"))
    {
        double duration = 0; error_t error = encoder_validate_opus_file(argv[2], &duration);
        printf("{\"error\":%d,\"durationSeconds\":%.9f}\n", error, duration);
        return error ? 1 : 0;
    }
    else if (argc == 3 && !strcmp(argv[1], "--validate-taf")) return taf_upload_validate(argv[2]) ? 0 : 1;
    else
    {
        test_validation(); test_import_and_reuse(); test_limits_and_cleanup(); test_taf_atomic_upload();
        puts("PASS: native import, 114 chapters, reuse, validation, limits, cleanup and atomic TAF upload");
    }
    free(response_body);
    return 0;
}
