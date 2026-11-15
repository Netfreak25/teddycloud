#pragma once

#include "cJSON.h"
#include "settings.h"
#include "v3_native_cache.h"

#define V3_PLAYLIST_METADATA_MAX_BYTES (1024U * 1024U)
#define V3_PLAYLIST_METADATA_REVISION_SIZE 65U

/* Original-content display metadata. No function publishes a playback route,
 * changes a source, acknowledges freshness or performs network I/O. */
error_t v3_playlist_metadata_observe(settings_t *settings, const char *ruid,
    const uint8_t *manifest, size_t length);
error_t v3_playlist_metadata_enrich(settings_t *settings, const char *ruid,
    uint32_t version);
error_t v3_playlist_metadata_bind_collection(settings_t *settings, const char *ruid,
    uint32_t version, const char *source);
/** Positive evidence of a locally converted TAF; absence is not native proof. */
bool_t v3_playlist_metadata_is_local_taf(settings_t *settings, const char *ruid,
    uint32_t version);
bool_t v3_playlist_metadata_needs_fetch(settings_t *settings, const char *ruid,
    uint32_t version);
error_t v3_playlist_metadata_begin_attempt(settings_t *settings, const char *ruid,
    uint32_t version);
void v3_playlist_metadata_finish_attempt(settings_t *settings, const char *ruid,
    uint32_t version, const char *reason);
void v3_playlist_metadata_reset_attempt(settings_t *settings, const char *ruid);

/* Read-only, owned JSON. An unknown version has no invented chapterCount or
 * contentVersion. Empty titles and null durations retain their exact indices. */
cJSON *v3_playlist_metadata_json(settings_t *settings, const char *ruid,
    uint32_t version);
cJSON *v3_playlist_collection_json(const v3_native_library_collection_t *collection,
    bool_t version_valid, uint32_t version);
void v3_playlist_metadata_revision(settings_t *settings, const char *ruid,
    uint32_t version, char revision[V3_PLAYLIST_METADATA_REVISION_SIZE]);
