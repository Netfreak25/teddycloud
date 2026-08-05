#pragma once

#include <stddef.h>

#include "contentJson.h"
#include "error.h"
#include "settings.h"

#define CONTENT_PLAYLIST_TITLE_MAX 200
#define CONTENT_PLAYLIST_TRACK_TITLE_MAX 200

typedef struct
{
    bool_t valid;
    bool_t durations_valid;
    char *title;
    size_t track_count;
    char **tracks;
    uint32_t *durations;
} content_playlist_t;

/**
 * Return whether a Tonie uses a stable custom TAF that can own playlist metadata.
 */
bool_t content_playlist_is_editable(const tonie_info_t *tonie_info);

/**
 * Return the authoritative number of chapters stored in the custom TAF header.
 */
size_t content_playlist_chapter_count(const tonie_info_t *tonie_info);

/**
 * Calculate one duration per real TAF chapter from its start positions and final granule position.
 */
bool_t content_playlist_calculate_durations(const tonie_info_t *tonie_info,
                                            uint32_t *durations,
                                            size_t duration_count);

/**
 * Load playlist display metadata addressed by the custom TAF audio ID and SHA-1.
 */
error_t content_playlist_load(settings_t *settings,
                              const tonie_info_t *tonie_info,
                              content_playlist_t *playlist);

/**
 * Atomically persist playlist display metadata for the current custom TAF.
 */
error_t content_playlist_save(settings_t *settings,
                              const tonie_info_t *tonie_info,
                              const char *title,
                              const char *const *tracks,
                              size_t track_count);

void content_playlist_free(content_playlist_t *playlist);
