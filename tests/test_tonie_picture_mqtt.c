/* Real playback/publisher code with only integration sinks and artwork lookup stubbed.
 * No broker, production settings, filesystem writes or box connection are used. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "toniebox_state.h"
#include "tonie_picture.h"
#include "tb2_ruid.h"

static settings_t settings[MAX_OVERLAYS];
static bool enabled = true;
static unsigned pictures, lookups, revision;
static char last_picture[256], last_ruid[17];
static uint32_t last_audio_id;

settings_t *get_settings_id(uint8_t id) { return &settings[id]; }
bool settings_get_bool(const char *key) { assert(!strcmp(key, "mqtt.enabled")); return enabled; }
const char *settings_get_string(const char *key) { assert(!strcmp(key, "core.host_url")); return "https://tc:8443/"; }
bool settings_set_unsigned_id(const char *key, uint32_t value, uint8_t id) { return true; }
void setLastRuid(char ruid[17], settings_t *context) {}
void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *ptr) { free(ptr); }
char *custom_asprintf(const char *format, ...) {
    char *text = NULL; va_list args; va_start(args, format);
    assert(vasprintf(&text, format, args) >= 0); va_end(args); return text;
}
char *__wrap_tonie_picture_resolve(settings_t *context, const char *ruid, uint32_t audio_id) {
    lookups++; strcpy(last_ruid, ruid); last_audio_id = audio_id;
    return *ruid ? custom_asprintf("/api/tonie/image/%s?v=%u", ruid, revision) : strdup("/img_unknown.png");
}
error_t mqtt_sendBoxEvent(const char *event, const char *content, client_ctx_t *context) {
    if (!strcmp(event, "ContentPicture")) { pictures++; snprintf(last_picture, sizeof(last_picture), "%s", content); }
    return NO_ERROR;
}
error_t sse_sendEvent(const char *event, const char *content, bool escape) { return NO_ERROR; }

static void report(client_ctx_t *context, const char *ruid, unsigned chapter) {
    tbs_toniebox2_playback_state(context, ruid, true, 123, true, chapter, true, 42, "1:00");
}

int main(void) {
    toniebox_state_init();
    settings[1].internal.config_used = true;
    settings[1].toniebox.boxGeneration = GENERATION_TB2;
    client_ctx_t tb2 = {0}; tb2.settings = &settings[1]; tb2.state = get_toniebox_state_id(1);
    report(&tb2, "8194f21c500304e0", 0);
    assert(pictures == 1 && lookups == 1 && last_audio_id == 123);
    assert(!strcmp(last_picture, "https://tc:8443/api/tonie/image/8194F21C500304E0?v=0"));
    report(&tb2, "8194f21c500304e0", 1);
    assert(pictures == 1 && lookups == 1); // Chapter/position does not rehash artwork.
    revision++;
    tbs_refresh_content_picture("8194F21C500304E0");
    assert(pictures == 2 && strstr(last_picture, "?v=1"));
    tbs_refresh_content_picture("0000000000000001");
    assert(pictures == 2);
    report(&tb2, "ambiguous-playback-name", 0);
    assert(last_ruid[0] == '\0' && !strcmp(last_picture, "https://tc:8443/img_unknown.png"));
    // The retained old playback RUID must NOT be used for custom artwork.
    unsigned before = pictures;
    tbs_refresh_content_picture("8194F21C500304E0"); assert(pictures == before);
    report(&tb2, NULL, 0);
    assert(strstr(last_picture, "/img_empty.png"));
    before = pictures; tbs_refresh_content_picture("8194F21C500304E0"); assert(pictures == before);

    settings[2].internal.config_used = true;
    settings[2].toniebox.boxGeneration = GENERATION_TB1;
    client_ctx_t tb1 = {0}; tb1.settings = &settings[2]; tb1.state = get_toniebox_state_id(2);
    assert(tb2_ruid_to_uid("8194F21C500304E0", &tb1.state->tag.uid));
    tb1.state->tag.valid = true; tb1.state->tag.audio_id = 456;
    tbs_publish_content_picture(&tb1);
    assert(last_audio_id == 456 && !strcmp(last_ruid, "8194F21C500304E0"));
    before = pictures; tbs_refresh_content_picture(last_ruid); assert(pictures == before + 1);
    tbs_playback(&tb1, TBS_PLAYBACK_STOPPED);
    before = pictures; tbs_refresh_content_picture(last_ruid); assert(pictures == before);
    enabled = false; tbs_publish_content_picture(&tb1); assert(pictures == before);
    assert(tonie_picture_is_unknown("https://tc/img_unknown.png?v=2"));
    assert(!tonie_picture_is_unknown("https://tc/cover.png"));
    puts("MQTT artwork: TB1/TB2 URLs, explicit RUID, refresh, stop, chapter deduplication and disabled integration passed.");
    return 0;
}
