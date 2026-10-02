/* Real collector, POSIX writer and filesystem; only settings are fixtures. */
#include <assert.h>
#include "../src/mqtt_debug.c"

static settings_t settings_fixture[3];

settings_t *get_settings(void) { return &settings_fixture[0]; }
settings_t *get_settings_id(uint8_t id) { return id < 3 ? &settings_fixture[id] : NULL; }
setting_item_t *settings_get_ovl(int index, const char *overlay)
{ (void)index; (void)overlay; return NULL; }
uint16_t settings_get_size(void) { return 0; }
bool_t settings_canonicalize_box_id(const char *input, char *output, size_t capacity)
{
    if (!input || strlen(input) != 12 || capacity < 13) return false;
    for (size_t i = 0; i < 12; i++) {
        if (!isxdigit((unsigned char)input[i])) return false;
        output[i] = (char)toupper((unsigned char)input[i]);
    }
    output[12] = 0; return true;
}

/* Hold only the writer's disk boundary. Production helpers still perform all
 * queue accounting, record serialization, flushing and atomic publication. */
static void flush_locked(void)
{
    debug_writer_maintenance();
    for (;;) {
        osAcquireMutex(&debug.mutex);
        debug_event_t event = {0};
        if (debug.count) {
            event = debug.events[debug.head];
            debug.head = (debug.head + 1) % DEBUG_EVENTS;
            debug.count--; debug.bytes -= event.length + 1;
        }
        osReleaseMutex(&debug.mutex);
        if (!event.line) {
            debug_writer_maintenance();
            osAcquireMutex(&debug.mutex); bool more = debug.count != 0; osReleaseMutex(&debug.mutex);
            if (more) continue;
            break;
        }
        debug_write_event(&event);
    }
    debug_writer_maintenance();
}
static void flush_all(void)
{ osAcquireMutex(&debug.disk); flush_locked(); osReleaseMutex(&debug.disk); }

static char *read_export(settings_t *settings, const char *session, const char *name, size_t *size)
{
    mqtt_debug_file_t *file = NULL; uint64_t length = 0;
    assert(mqtt_debug_file_open(settings, session, name, &file, &length) == NO_ERROR);
    assert(length < 4U * DEBUG_EVENT_BYTES);
    char *text = calloc(1, (size_t)length + 1); assert(text);
    size_t received = 0;
    assert(mqtt_debug_file_read(file, text, (size_t)length, &received) == NO_ERROR);
    assert(received == length);
    mqtt_debug_file_close(file);
    if (size) *size = (size_t)length;
    return text;
}
static size_t mqtt_packet(uint8_t *output, uint8_t header, const void *payload, size_t length)
{
    output[0] = header; size_t remaining = length, offset = 1;
    do {
        uint8_t digit = remaining % 128U; remaining /= 128U;
        output[offset++] = digit | (remaining ? 0x80 : 0);
    } while (remaining);
    if (length) memcpy(output + offset, payload, length);
    return offset + length;
}
static void publish(uint64_t owner, const char *payload)
{
    const char *topic = "toniebox/AABBCCDDEEFF/logs";
    uint8_t body[4096], packet[4100];
    size_t topic_length = strlen(topic), payload_length = strlen(payload);
    assert(topic_length + payload_length + 2 < sizeof(body));
    body[0] = topic_length >> 8; body[1] = topic_length;
    memcpy(body + 2, topic, topic_length);
    memcpy(body + 2 + topic_length, payload, payload_length);
    size_t length = mqtt_packet(packet, 0x30, body, topic_length + payload_length + 2);
    mqtt_debug_packet(owner, 7, mqtt_debug_next_message(owner), "box", "packet_received", packet, length);
}
static bool count_name(const char *name, void *context)
{ (void)name; (*(unsigned *)context)++; return true; }

static void test_selection_packets_and_redaction(char session[64])
{
    assert(!mqtt_debug_enabled(&settings_fixture[0]));
    /* A box-local default is sufficient; an explicit override is not required. */
    assert(mqtt_debug_enabled(&settings_fixture[1]));
    settings_fixture[0].mqtt_server.debug_enabled = false;
    assert(mqtt_debug_enabled(&settings_fixture[1])); /* Not inherited globally. */
    settings_fixture[1].mqtt_server.debug_enabled = false;
    assert(!mqtt_debug_enabled(&settings_fixture[1])); /* Explicit opt-out. */
    settings_fixture[1].mqtt_server.debug_enabled = true;
    settings_fixture[1].toniebox.boxGeneration = GENERATION_TB1;
    assert(!mqtt_debug_enabled(&settings_fixture[1]));
    settings_fixture[1].toniebox.boxGeneration = GENERATION_UNKNOWN;
    assert(!mqtt_debug_enabled(&settings_fixture[1]));
    settings_fixture[1].toniebox.boxGeneration = GENERATION_TB2;
    settings_fixture[1].internal.config_used = false;
    assert(!mqtt_debug_enabled(&settings_fixture[1]));
    settings_fixture[1].internal.config_used = true;
    assert(!mqtt_debug_enabled(&settings_fixture[2]));
    mqtt_debug_sync(100, &settings_fixture[1], false);
    mqtt_debug_sync(200, &settings_fixture[2], true);
    assert(mqtt_debug_active(100) && !mqtt_debug_active(200));
    osAcquireMutex(&debug.mutex);
    strcpy(session, debug_find(100)->id);
    osReleaseMutex(&debug.mutex);

    publish(100, "{\"title\":\"Chapter one\",\"nested\":{\"password\":\"secret-one\",\"password\":\"secret-two\",\"TOKEN\":\"secret-three\"},\"worker_token\":73}");
    publish(100, "normal readable status");
    publish(100, "password=hidden-value authorization=hidden-auth https://user:hidden-userinfo@example.invalid/path?token=hidden-query");
    publish(200, "must-not-record-other-box");
    cJSON *worker = cJSON_CreateObject();
    cJSON_AddNumberToObject(worker, "worker_token", 74);
    mqtt_debug_event(100, 7, 0, "tonies_transport", "worker_counter", worker);
    uint8_t buffer[128];
    const uint8_t connect[] = {0,4,'M','Q','T','T',4,0xc2,0,60,0,3,'b','o','x',0,4,'u','s','e','r',0,4,'p','a','s','s'};
    size_t length = mqtt_packet(buffer, 0x10, connect, sizeof(connect));
    mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "box", "packet_received", buffer, length);
    const uint8_t connack[] = {0x20,2,0,0};
    mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "local", "packet_created", connack, sizeof(connack));
    const uint8_t subscribe[] = {0x82,6,0,1,0,1,'x',1};
    const uint8_t suback[] = {0x90,3,0,1,1};
    const uint8_t unsubscribe[] = {0xa2,5,0,1,0,1,'x'};
    mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "box", "packet_received", subscribe, sizeof(subscribe));
    mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "local", "packet_created", suback, sizeof(suback));
    mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "box", "packet_received", unsubscribe, sizeof(unsubscribe));
    const uint8_t types[] = {4,5,6,7,11,12,13,14};
    for (size_t i = 0; i < sizeof(types); i++) {
        uint8_t packet[] = {(uint8_t)((types[i] << 4) | (types[i] == 6 ? 2 : 0)), 2, 0, 1};
        if (types[i] >= 12) packet[1] = 0;
        mqtt_debug_packet(100, 0, mqtt_debug_next_message(100), "box", "packet_received", packet, packet[1] + 2);
    }
    flush_all();
    char *text = read_export(&settings_fixture[1], session, "events-000001.jsonl", NULL);
    assert(strstr(text, "Chapter one") && strstr(text, "normal readable status"));
    assert(!strstr(text, "secret-one") && !strstr(text, "secret-two") && !strstr(text, "secret-three"));
    assert(!strstr(text, "hidden-value") && !strstr(text, "hidden-auth") && !strstr(text, "hidden-userinfo") && !strstr(text, "hidden-query"));
    assert(!strstr(text, "must-not-record-other-box") && !strstr(text, "data_base64"));
    assert(!strstr(text, "\"worker_token\":73"));
    assert(strstr(text, "\"worker_token\":74"));
    const char *names[] = {"CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL", "PUBCOMP",
        "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(strstr(text, names[i]));
    char *line = text;
    while (*line) {
        char *end = strchr(line, '\n'); assert(end); *end = 0;
        cJSON *entry = cJSON_ParseWithOpts(line, NULL, true); assert(entry);
        assert(cJSON_IsString(cJSON_GetObjectItem(entry, "stage")));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(entry, "recordingId")), session));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(entry, "processStartId")), debug.process_id));
        assert(cJSON_GetObjectItem(entry, "localConnectionId")->valuedouble == 100);
        assert(cJSON_GetObjectItem(entry, "overlay")->valuedouble == 1);
        assert(cJSON_GetObjectItem(entry, "wallTimeMs")->valuedouble > 1000000000000.0);
        assert(cJSON_GetObjectItem(entry, "monotonicMs")->valuedouble > 0);
        assert(cJSON_GetObjectItem(entry, "recordingSequence")->valuedouble > 0);
        cJSON_Delete(entry); line = end + 1;
    }
    free(text);
}

static void test_queue_gap_and_rotation(const char *session)
{
    osAcquireMutex(&debug.disk);
    for (size_t i = 0; i < DEBUG_EVENTS + 16; i++)
        mqtt_debug_event(100, 0, 0, "local", "queue_fixture", cJSON_CreateObject());
    osAcquireMutex(&debug.mutex);
    assert(debug.count <= DEBUG_EVENTS && debug.bytes <= DEBUG_QUEUE_BYTES);
    debug_session_t *active = debug_find(100); assert(active && active->dropped >= 16);
    osReleaseMutex(&debug.mutex);
    flush_locked();
    assert(active->stream && fflush(active->stream) == 0);
    char path[DEBUG_PATH], line[2048];
    assert(debug_path(path, active->box, session, "events-000001.jsonl"));
    FILE *input = debug_file(path, false, false); assert(input);
    bool gap = false;
    while (fgets(line, sizeof(line), input)) if (strstr(line, "\"stage\":\"diagnostic_gap\"")) gap = true;
    assert(!ferror(input) && gap); fclose(input);
    off_t original_length = ftello(active->stream); assert(original_length > 0);
    assert(ftruncate(fileno(active->stream), DEBUG_SEGMENT_BYTES) == 0);
    assert(fseeko(active->stream, 0, SEEK_END) == 0);
    active->segment_bytes = DEBUG_SEGMENT_BYTES;
    mqtt_debug_event(100, 0, 0, "local", "rotated_fixture", cJSON_CreateObject());
    flush_locked();
    assert(active->segment == 2 && active->stream);
    /* Remove only fixture-added sparse padding after rotation so status/export
     * can still inspect the genuine complete JSONL records in segment zero. */
    FILE *completed = debug_file(path, true, false); assert(completed);
    assert(!ftruncate(fileno(completed), original_length) && !fclose(completed));
    osReleaseMutex(&debug.disk);
    char *text = read_export(&settings_fixture[1], session, "events-000002.jsonl", NULL);
    assert(strstr(text, "rotated_fixture")); free(text);
}

static void test_export_binding_prefix_and_paths(const char *session, const char *base)
{
    mqtt_debug_file_t *file = NULL; uint64_t length = 0;
    assert(mqtt_debug_file_open(&settings_fixture[2], session, "session.json", &file, &length) != NO_ERROR);
    assert(mqtt_debug_file_open(&settings_fixture[1], "../outside", "session.json", &file, &length) != NO_ERROR);
    assert(mqtt_debug_file_open(&settings_fixture[1], session, "../session.json", &file, &length) != NO_ERROR);
    char path[DEBUG_PATH]; assert(debug_path(path, "AABBCCDDEEFF", session, "events-000099.jsonl"));
    osAcquireMutex(&debug.disk);
    FILE *output = debug_file(path, true, true); assert(output);
    const char *complete = "{\"stage\":\"complete\"}\n";
    assert(fputs(complete, output) >= 0 && fputs("{\"stage\":\"partial", output) >= 0);
    assert(fclose(output) == 0);
    osReleaseMutex(&debug.disk);
    assert(mqtt_debug_file_open(&settings_fixture[1], session, "events-000099.jsonl", &file, &length) == NO_ERROR);
    assert(length == strlen(complete));
    assert(mqtt_debug_file_limit(file, length - 1) == ERROR_INVALID_LENGTH);
    assert(mqtt_debug_file_limit(file, length) == NO_ERROR);
    char buffer[128]; size_t received = 0;
    assert(mqtt_debug_file_read(file, buffer, sizeof(buffer), &received) == NO_ERROR && received == length);
    assert(!memcmp(buffer, complete, received));
    assert(mqtt_debug_file_read(file, buffer, sizeof(buffer), &received) == NO_ERROR && !received);
    mqtt_debug_file_close(file);
    assert(mqtt_debug_file_open(&settings_fixture[1], session, "session.json", &file, &length) == NO_ERROR);
    assert(length && mqtt_debug_file_limit(file, length - 1) == ERROR_INVALID_LENGTH);
    mqtt_debug_file_close(file);
    char outside[DEBUG_PATH]; assert(snprintf(outside, sizeof(outside), "%s/outside.txt", base) < (int)sizeof(outside));
    output = fopen(outside, "wb"); assert(output); assert(fputs("not-exportable", output) >= 0); assert(!fclose(output));
    assert(debug_path(path, "AABBCCDDEEFF", session, "events-000098.jsonl"));
    assert(symlink(outside, path) == 0);
    assert(mqtt_debug_file_open(&settings_fixture[1], session, "events-000098.jsonl", &file, &length) != NO_ERROR);
    assert(unlink(path) == 0);
    assert(debug_path(path, "AABBCCDDEEFF", "0000000000000-0000000000000000", NULL));
    assert(symlink(base, path) == 0);
    assert(mqtt_debug_file_open(&settings_fixture[1], "0000000000000-0000000000000000", "session.json", &file, &length) != NO_ERROR);
    assert(unlink(path) == 0);
    assert(debug_path(path, "AABBCCDDEEFF", session, NULL));
    debug_directory_t dir; assert(debug_directory_open(&dir, path, false));
    unsigned first = 0, second = 0;
    assert(debug_list_directory(&dir, count_name, &first));
    assert(debug_list_directory(&dir, count_name, &second));
    assert(first && first == second); debug_directory_close(&dir);
}

static void test_retention_and_pinned_download(void)
{
    assert(sizeof(debug_retention_scan_t) < 16U * 1024U);
    debug_session_t historical = {.started = 1};
    strcpy(historical.box, "AABBCCDDEEFF");
    strcpy(historical.id, "0000000000001-0000000000000001");
    char path[DEBUG_PATH];
    osAcquireMutex(&debug.disk);
    assert(debug_summary(&historical, false));
    assert(debug_path(path, historical.box, historical.id, "events-000000.jsonl"));
    FILE *output = debug_file(path, true, true); assert(output);
    assert(fputs("{\"stage\":\"historical\"}\n", output) >= 0 && !fflush(output));
    struct timespec expired[2] = {{.tv_sec = 1}, {.tv_sec = 1}};
    assert(futimens(fileno(output), expired) == 0 && !fclose(output));
    osReleaseMutex(&debug.disk);
    mqtt_debug_file_t *pinned = NULL; uint64_t length = 0;
    assert(mqtt_debug_file_open(&settings_fixture[1], historical.id, "events-000000.jsonl", &pinned, &length) == NO_ERROR);
    osAcquireMutex(&debug.disk);
    debug_session_t *active = debug_find(100); assert(active);
    assert(debug_quota(active, 0, true));
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    osReleaseMutex(&debug.disk);
    char text[128]; size_t received = 0;
    assert(mqtt_debug_file_read(pinned, text, sizeof(text) - 1, &received) == NO_ERROR && received == length);
    text[received] = 0; assert(strstr(text, "historical")); mqtt_debug_file_close(pinned);
    char *metadata = read_export(&settings_fixture[1], historical.id, "session.json", NULL);
    assert(strstr(metadata, "\"truncated\":true") && strstr(metadata, "\"retentionGaps\":1")); free(metadata);

    /* Sparse regular files exercise actual byte accounting without writing
     * hundreds of MiB; only completed historical segments may be removed. */
    osAcquireMutex(&debug.disk);
    output = debug_file(path, true, true); assert(output);
    assert(!fflush(output) && !ftruncate(fileno(output), DEBUG_BOX_BYTES) && !fclose(output));
    assert(debug_quota(active, 0, true));
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    strcpy(historical.box, "112233445566");
    assert(debug_summary(&historical, false));
    assert(debug_path(path, historical.box, historical.id, "events-000000.jsonl"));
    output = debug_file(path, true, true); assert(output);
    assert(!fflush(output) && !ftruncate(fileno(output), DEBUG_TOTAL_BYTES) && !fclose(output));
    assert(debug_quota(active, 0, true));
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    assert(active->stream && !active->error[0]);
    osReleaseMutex(&debug.disk);
}

static void test_status_and_unassigned(void)
{
    for (unsigned i = 0; i < DEBUG_PRELUDE + 3; i++) {
        cJSON *details = cJSON_CreateObject(); cJSON_AddNumberToObject(details, "fixture", i);
        mqtt_debug_unassigned("identity_pending", details);
    }
    cJSON *status = mqtt_debug_status(&settings_fixture[1]); assert(status);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(status, "state")), "recording"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(status, "recentUnassigned")) == DEBUG_PRELUDE);
    assert(cJSON_GetObjectItem(status, "unassignedDroppedEvents")->valuedouble == 3);
    cJSON *sessions = cJSON_GetObjectItem(status, "sessions"); assert(cJSON_IsArray(sessions));
    cJSON *current = cJSON_GetArrayItem(sessions, 0); assert(current);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(current, "active")));
    const char *started = cJSON_GetStringValue(cJSON_GetObjectItem(current, "startedAt"));
    assert(started && strchr(started, 'T') && strchr(started, 'Z'));
    cJSON_Delete(status);
}

static void test_failure_pause_and_shutdown(const char *session)
{
    osAcquireMutex(&debug.disk);
    debug_session_t *active = debug_find(100); assert(active && active->stream);
    assert(!fclose(active->stream)); active->stream = fopen("/dev/full", "wb"); assert(active->stream);
    mqtt_debug_event(100, 0, 0, "local", "write_failure_fixture", cJSON_CreateObject());
    flush_locked(); debug_writer_maintenance();
    osReleaseMutex(&debug.disk);
    assert(!mqtt_debug_active(100));
    mqtt_debug_sync(100, &settings_fixture[1], true);
    assert(!mqtt_debug_active(100));
    osAcquireMutex(&debug.mutex);
    active = debug_find(100); assert(active && active->error[0] && !strcmp(active->id, session));
    osReleaseMutex(&debug.mutex);
    cJSON *status = mqtt_debug_status(&settings_fixture[1]); assert(status);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(status, "state")), "error"));
    cJSON *sessions = cJSON_GetObjectItem(status, "sessions"); assert(cJSON_IsArray(sessions));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(sessions, 0), "active")));
    cJSON_Delete(status);
    settings_fixture[1].mqtt_server.debug_enabled = false;
    mqtt_debug_sync(100, &settings_fixture[1], true);
    flush_all(); assert(!mqtt_debug_active(100));
    mqtt_debug_deinit();
    osAcquireMutex(&debug.mutex); assert(debug.stopped); osReleaseMutex(&debug.mutex);
}

static void test_listener_reload_and_mid_session(const char *previous_session)
{
    struct timespec before, after; assert(!clock_gettime(CLOCK_REALTIME, &before));
    uint64_t wall = mqtt_debug_wall_ms();
    assert(!clock_gettime(CLOCK_REALTIME, &after));
    assert(wall >= (uint64_t)before.tv_sec * 1000U + (uint64_t)before.tv_nsec / 1000000U);
    assert(wall <= (uint64_t)after.tv_sec * 1000U + (uint64_t)after.tv_nsec / 1000000U);
    char process_id[64]; strcpy(process_id, debug.process_id);
    settings_fixture[1].mqtt_server.debug_enabled = true;
    mqtt_debug_init();
    osAcquireMutex(&debug.mutex);
    assert(debug.ready && !debug.stopping && !debug.stopped && !debug.count);
    assert(!debug_find(100));
    osReleaseMutex(&debug.mutex);
    mqtt_debug_sync(101, &settings_fixture[1], true);
    assert(mqtt_debug_active(101));
    char session[64];
    osAcquireMutex(&debug.mutex); strcpy(session, debug_find(101)->id); osReleaseMutex(&debug.mutex);
    assert(strcmp(session, previous_session) && !strcmp(process_id, debug.process_id));
    flush_all();
    char *text = read_export(&settings_fixture[1], session, "events-000001.jsonl", NULL);
    assert(strstr(text, "mid_session_start") && strstr(text, "\"established\":true")); free(text);
    mqtt_debug_close(101, "fixture_completed"); flush_all();
    assert(!mqtt_debug_active(101));
    text = read_export(&settings_fixture[1], session, "session.json", NULL);
    assert(strstr(text, "fixture_completed") && strstr(text, "\"active\":false")); free(text);
    mqtt_debug_deinit();
    osAcquireMutex(&debug.mutex); assert(debug.stopped && !debug.count); osReleaseMutex(&debug.mutex);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    settings_fixture[0].internal.datadirfull = argv[1];
    settings_fixture[0].mqtt_server.debug_enabled = true;
    for (unsigned i = 1; i < 3; i++) {
        settings_fixture[i].internal.config_used = true;
        settings_fixture[i].internal.overlayNumber = i;
        settings_fixture[i].internal.overlayUniqueId = i == 1 ? "aabbccddeeff" : "112233445566";
        settings_fixture[i].toniebox.boxGeneration = GENERATION_TB2;
        settings_fixture[i].mqtt_server.debug_enabled = true;
    }
    settings_fixture[2].mqtt_server.debug_enabled = false;
    mqtt_debug_init(); assert(debug.ready && !debug.error[0]);
    char session[64];
    test_selection_packets_and_redaction(session);
    test_queue_gap_and_rotation(session);
    test_export_binding_prefix_and_paths(session, argv[1]);
    test_retention_and_pinned_download();
    test_status_and_unassigned();
    test_failure_pause_and_shutdown(session);
    test_listener_reload_and_mid_session(session);
    puts("MQTT debug PASS: TB2 default-on and opt-out, redaction, queue gaps, rotation/retention, pinned export, status, failure latch, UTC milliseconds and listener reload");
    return 0;
}
