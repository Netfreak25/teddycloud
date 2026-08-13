#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "fs_port.h"
#include "os_port.h"
#include "tb2_ruid.h"

#define V3_NATIVE_CACHE_OBJECT_NAME_SIZE 160
#define V3_NATIVE_CACHE_OBJECT_AUTH_SIZE 4096
#define V3_NATIVE_CACHE_OBJECT_TYPE_SIZE 64
#define V3_NATIVE_CACHE_OBJECT_FILENAME_SIZE 256
#define V3_NATIVE_CACHE_CONTENT_TYPE_SIZE 128
#define V3_NATIVE_CACHE_CHAPTER_NAME_SIZE V3_NATIVE_CACHE_OBJECT_NAME_SIZE
#define V3_NATIVE_CACHE_CHAPTER_AUTH_SIZE V3_NATIVE_CACHE_OBJECT_AUTH_SIZE
#define V3_NATIVE_LIBRARY_STAGING_DIR ".tb2-native-staging"
#define V3_NATIVE_LIBRARY_HASH_HEX_LENGTH 64
#define V3_NATIVE_LIBRARY_HASH_HEX_SIZE (V3_NATIVE_LIBRARY_HASH_HEX_LENGTH + 1)

typedef struct
{
    char name[V3_NATIVE_CACHE_OBJECT_NAME_SIZE];
    char auth[V3_NATIVE_CACHE_OBJECT_AUTH_SIZE];
    char type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    char filename[V3_NATIVE_CACHE_OBJECT_FILENAME_SIZE];
    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE];
    uint32_t file_size;
} v3_native_cache_download_object_t;

/* Source compatibility for callers that still expose chapter counters. */
typedef v3_native_cache_download_object_t v3_native_cache_download_chapter_t;

typedef struct
{
    char ruid[TB2_RUID_SIZE];
    uint32_t version;
    char content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    v3_native_cache_download_object_t *objects;
    size_t object_count;
    /* Compatibility aliases; both point to the same allocation. */
    v3_native_cache_download_chapter_t *chapters;
    size_t chapter_count;
} v3_native_cache_download_plan_t;

typedef struct
{
    size_t index;
    char original_name[V3_NATIVE_CACHE_CHAPTER_NAME_SIZE];
    char sha256[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
    uint32_t file_size;
    char *path;
} v3_native_library_collection_chapter_t;

typedef struct
{
    uint8_t overlay_id;
    char ruid[TB2_RUID_SIZE];
    uint32_t content_version;
} v3_native_library_origin_t;

typedef struct
{
    char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
    uint32_t audio_id;
    v3_native_library_collection_chapter_t *chapters;
    size_t chapter_count;
    v3_native_library_origin_t *origins;
    size_t origin_count;
} v3_native_library_collection_t;

typedef struct
{
    size_t index;
    char name[V3_NATIVE_CACHE_OBJECT_NAME_SIZE];
    char type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    char filename[V3_NATIVE_CACHE_OBJECT_FILENAME_SIZE];
    char sha256[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
    uint32_t file_size;
    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE];
    char *path;
} v3_tonieplay_library_object_t;

typedef struct
{
    char content_hash[V3_NATIVE_LIBRARY_HASH_HEX_SIZE];
    uint32_t content_version;
    char content_type[V3_NATIVE_CACHE_OBJECT_TYPE_SIZE];
    char *manifest_path;
    uint8_t *manifest;
    size_t manifest_length;
    v3_tonieplay_library_object_t *objects;
    size_t object_count;
    v3_native_library_origin_t *origins;
    size_t origin_count;
} v3_tonieplay_library_collection_t;

/** Streaming capture state for one TONIES content-meta response. */
typedef struct
{
    char *cache_root;
    uint8_t overlay_id;
    char ruid[TB2_RUID_SIZE];
    uint8_t *data;
    size_t length;
    size_t capacity;
    uint32_t status_code;
    bool_t store;
    bool_t failed;
} v3_native_cache_meta_capture_t;

typedef enum
{
    V3_NATIVE_CHAPTER_BYPASS = 0,
    V3_NATIVE_CHAPTER_CAPTURE,
    V3_NATIVE_CHAPTER_SERVE,
    /** Complete in staging; reuse for manual downloads but keep live passthrough. */
    V3_NATIVE_CHAPTER_STAGED,
    V3_NATIVE_CHAPTER_FORWARD,
    V3_NATIVE_CHAPTER_REJECT,
} v3_native_cache_chapter_action_t;

/** Streaming capture state for one expected TONIES V3 object. */
typedef struct
{
    char *cache_root;
    char *stage_dir;
    char *temp_path;
    char *final_path;
    FsFile *file;
    uint8_t overlay_id;
    char ruid[TB2_RUID_SIZE];
    char name[V3_NATIVE_CACHE_CHAPTER_NAME_SIZE];
    uint32_t version;
    uint32_t expected_size;
    uint32_t written;
    size_t object_index;
    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE];
    bool_t failed;
} v3_native_cache_chapter_capture_t;

/** Accept only one portable, unambiguous filename segment. */
bool_t v3_native_cache_chapter_name_is_safe(const char *name);

/** Load an active original manifest backed completely by cache or library. */
error_t v3_native_cache_read_active_manifest(const char *cache_root,
                                             const char *library_root,
                                             uint8_t overlay_id,
                                             const char *ruid,
                                             uint8_t **data,
                                             size_t *length,
                                             uint32_t *version);

/** Return the version of a complete active original TONIES generation. */
bool_t v3_native_cache_active_version(const char *cache_root,
                                      uint8_t overlay_id,
                                      const char *ruid,
                                      uint32_t *version);

/** Return whether the complete active generation contains Tonieplay objects. */
bool_t v3_native_cache_active_is_tonieplay(const char *cache_root,
                                           const char *library_root,
                                           uint8_t overlay_id,
                                           const char *ruid);

/** Return validated metadata for one complete active original generation. */
bool_t v3_native_cache_active_info(const char *cache_root,
                                   const char *library_root,
                                   uint8_t overlay_id,
                                   const char *ruid,
                                   uint32_t *version,
                                   size_t *object_count,
                                   bool_t *tonieplay);

/**
 * Import one complete active TONIES generation into the native TB2 library.
 *
 * The content-addressed import is staged below the library root and becomes
 * visible only after metadata and every chapter have been copied and checked.
 * A successful import links the generation and removes duplicate cache objects
 * only after the immutable library paths have been validated.
 */
error_t v3_native_cache_import_active_library(const char *cache_root,
                                               const char *library_root,
                                               uint8_t overlay_id,
                                               const char *ruid,
                                               char **library_source);

/**
 * Return the validated native-library source linked to one active generation.
 *
 * The optional descriptor link is accepted only when the active marker still
 * selects the requested version and the immutable library collection retains
 * matching overlay/RUID/version provenance. The caller owns the returned
 * string.
 */
error_t v3_native_cache_active_library_source(const char *cache_root,
                                              const char *library_root,
                                              uint8_t overlay_id,
                                              const char *ruid,
                                              uint32_t version,
                                              char **library_source);

/** Import a complete active Tonieplay generation including its raw manifest. */
error_t v3_native_cache_import_active_tonieplay_library(
    const char *cache_root,
    const char *library_root,
    uint8_t overlay_id,
    const char *ruid);

/** Return true only for the canonical native-library source URI. */
bool_t v3_native_library_source_is_candidate(const char *source);

/** Derive the stable non-zero TB1 audio ID from a canonical source URI. */
bool_t v3_native_library_source_audio_id(const char *source,
                                         uint32_t *audio_id);

/** Load and validate one immutable content-addressed native collection. */
error_t v3_native_library_collection_load(
    const char *library_root,
    const char *source,
    bool_t verify_hashes,
    v3_native_library_collection_t *collection);

void v3_native_library_collection_free(
    v3_native_library_collection_t *collection);

/** Load and fully validate one immutable Tonieplay collection. */
error_t v3_tonieplay_library_collection_load(
    const char *library_root,
    const char *source,
    bool_t verify_hashes,
    v3_tonieplay_library_collection_t *collection);

void v3_tonieplay_library_collection_free(
    v3_tonieplay_library_collection_t *collection);

bool_t v3_tonieplay_library_source_version(const char *library_root,
                                            const char *source,
                                            uint32_t *version);

/** Activate a library Tonieplay collection for one TB2 overlay/rUID. */
error_t v3_tonieplay_library_activate(
    const char *library_root,
    const char *source,
    uint8_t overlay_id,
    const char *ruid,
    uint32_t effective_version,
    uint8_t **manifest,
    size_t *manifest_length);

/** Resolve an exact object name from the currently assigned collection. */
bool_t v3_tonieplay_library_resolve(uint8_t overlay_id,
                                    const char *name,
                                    char **path,
                                    char content_type[V3_NATIVE_CACHE_CONTENT_TYPE_SIZE]);

bool_t v3_tonieplay_library_route_active(uint8_t overlay_id,
                                         const char *ruid);

bool_t v3_tonieplay_library_route_assigned(uint8_t overlay_id);

void v3_tonieplay_library_deactivate(uint8_t overlay_id);

/** Delete exactly one canonical native-library collection and its TB1 derivative. */
error_t v3_native_library_collection_delete(const char *library_root,
                                             const char *cache_root,
                                             const char *content_hash);

/** Detach an original generation after a source assignment changes. */
void v3_native_cache_invalidate(const char *cache_root,
                                uint8_t overlay_id,
                                const char *ruid);

void v3_native_cache_meta_capture_init(v3_native_cache_meta_capture_t *capture,
                                       const char *cache_root,
                                       uint8_t overlay_id,
                                       const char *ruid);
/** Observe content-meta routing without persisting manifest or chapters. */
void v3_native_cache_meta_observe_init(v3_native_cache_meta_capture_t *capture,
                                       uint8_t overlay_id,
                                       const char *ruid);
void v3_native_cache_meta_capture_response(v3_native_cache_meta_capture_t *capture,
                                           uint32_t status_code);
void v3_native_cache_meta_capture_append(v3_native_cache_meta_capture_t *capture,
                                         const void *data,
                                         size_t length);
error_t v3_native_cache_meta_capture_finish(v3_native_cache_meta_capture_t *capture);
void v3_native_cache_meta_capture_abort(v3_native_cache_meta_capture_t *capture);

/** Copy the validated current manifest route for a sequential manual download. */
error_t v3_native_cache_download_plan_get(uint8_t overlay_id,
                                          const char *ruid,
                                          v3_native_cache_download_plan_t *plan);
void v3_native_cache_download_plan_free(v3_native_cache_download_plan_t *plan);

/**
 * Resolve a chapter against the current content-meta route for an overlay.
 *
 * CAPTURE returns an initialized capture. SERVE returns an allocated immutable
 * cache or library path while preserving the requested original TONIES name.
 * BYPASS performs no local file access. REJECT denotes an ambiguous or
 * internally inconsistent cache mapping and must not be served locally.
 */
v3_native_cache_chapter_action_t v3_native_cache_chapter_prepare(
    const char *cache_root,
    const char *library_root,
    uint8_t overlay_id,
    const char *name,
    v3_native_cache_chapter_capture_t *capture,
    char **serve_path);

/** Record the upstream MIME type observed for the currently captured object. */
void v3_native_cache_object_content_type(
    v3_native_cache_chapter_capture_t *capture,
    const char *content_type);

/** Verify that a chapter still belongs to the current route generation. */
bool_t v3_native_cache_route_matches(uint8_t overlay_id,
                                     const char *ruid,
                                     uint32_t version,
                                     const char *name);

/** Return the version of the currently validated route for an overlay/rUID. */
bool_t v3_native_cache_route_version(uint8_t overlay_id,
                                     const char *ruid,
                                     uint32_t *version);

void v3_native_cache_chapter_append(v3_native_cache_chapter_capture_t *capture,
                                    const void *data,
                                    size_t length);
error_t v3_native_cache_chapter_finish(v3_native_cache_chapter_capture_t *capture);
void v3_native_cache_chapter_abort(v3_native_cache_chapter_capture_t *capture);
