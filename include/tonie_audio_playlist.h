#pragma once

#include "debug.h"
#include "toniefile.h"

#define TAP_TYPE_TAP "tap"
#define TAP_SHUFFLE_NONE 0
#define TAP_SHUFFLE_ALL 1
#define TAP_SHUFFLE_ONE 2
#define TAP_SHUFFLE_ONE_MAX_CANDIDATES 500

typedef struct
{
    char *filepath;
    char *name;
    char *_filepath_resolved;
} tap_file_t;
typedef struct
{
    char *type;
    time_t audio_id;
    char *filepath;
    char *_filepath_resolved;
    char *name;
    uint8_t shuffle;
    tap_file_t *files;
    size_t filesCount;
    bool_t _cached;
    bool_t _valid;
} tonie_audio_playlist_t;

error_t tap_load(char *filename, tonie_audio_playlist_t *tap);
error_t tap_save(char *filename, tonie_audio_playlist_t *tap);
void tap_free(tonie_audio_playlist_t *tap);
bool_t tap_final_cache_is_current(tonie_audio_playlist_t *tap);
error_t tap_publish_taf_replace_safe(const char *tmp_taf, const char *final_taf);

/**
 * Synchronously generate and atomically publish one complete TAP snapshot.
 *
 * The caller must serialize access to tap->_filepath_resolved for the entire
 * call. On success, final_taf_path points to the borrowed final path owned by
 * tap; a temporary .tmp path is never returned. Runtime shuffle indices are
 * selected exactly once for the published snapshot.
 */
error_t tap_materialize_final_snapshot(tonie_audio_playlist_t *tap, const char **final_taf_path);

/**
 * Materialize a complete TAP snapshot using an already selected runtime order.
 *
 * This is used by the TB2 V3 path so shuffle selection is performed exactly
 * once and the same indices can be persisted with the generated metadata.
 */
error_t tap_materialize_final_snapshot_selected(tonie_audio_playlist_t *tap,
                                                size_t *runtime_indices,
                                                size_t runtime_files_count,
                                                const char **final_taf_path);

error_t tap_prepare_runtime_indices(tonie_audio_playlist_t *tap, size_t **runtime_indices, size_t *runtime_files_count);
void tap_free_runtime_indices(size_t *runtime_indices);
error_t tap_predict_taf_live_header(tonie_audio_playlist_t *tap, size_t *runtime_indices, size_t runtime_files_count, toniefile_live_header_t *live_header, uint32_t *predicted_size);

/**
 * Remux independent Ogg/Opus chapters into one TAF without decoding audio.
 * Passing target_taf == NULL performs only the deterministic header/size pass.
 */
error_t tap_remux_native_collection(const char *const *chapter_paths,
                                    size_t chapter_count,
                                    uint32_t audio_id,
                                    const char *target_taf,
                                    size_t *current_source,
                                    bool_t *active,
                                    toniefile_live_header_t *live_header,
                                    uint32_t *predicted_size);
