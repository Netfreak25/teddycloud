#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "hash/sha256.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"

#define V3_LOCAL_CONTENT_RUID_HEX_LENGTH 16
#define V3_LOCAL_CONTENT_RUID_SIZE (V3_LOCAL_CONTENT_RUID_HEX_LENGTH + 1)
#define V3_LOCAL_CONTENT_SHA256_HEX_SIZE (SHA256_DIGEST_SIZE * 2 + 1)
#define V3_LOCAL_CONTENT_NAME_SIZE 96
#define V3_LOCAL_CONTENT_DESCRIPTOR_SCHEMA_VERSION 1

/** A fully materialized, immutable TB2 chapter. */
typedef struct
{
    size_t index;
    char name[V3_LOCAL_CONTENT_NAME_SIZE];
    char *path;
    uint32_t file_size;
    uint8_t sha256[SHA256_DIGEST_SIZE];
    char sha256_hex[V3_LOCAL_CONTENT_SHA256_HEX_SIZE];
} v3_local_content_chapter_t;

/** All chapters belonging to one local TB2 content generation. */
typedef struct
{
    uint8_t overlay_id;
    uint32_t effective_version;
    char ruid[V3_LOCAL_CONTENT_RUID_SIZE];
    v3_local_content_chapter_t *chapters;
    size_t chapter_count;
} v3_local_content_generation_t;

/**
 * Remux all chapters of a TAF into independent Ogg/Opus files.
 *
 * Audio packets are copied without decoding or re-encoding. The returned
 * generation owns its descriptors and path strings and must be released with
 * v3_local_content_generation_free(). No descriptor is returned unless every
 * chapter has been prepared and atomically published in cache_root/v3-local.
 */
error_t v3_local_content_prepare(const char *taf_path,
                                 const TonieboxAudioFileHeader *taf_header,
                                 const char *cache_root,
                                 const char *ruid,
                                 v3_local_content_generation_t *generation);

/** Release all memory owned by a prepared generation. */
void v3_local_content_generation_free(v3_local_content_generation_t *generation);

/** Find a chapter by its exact, content-addressed manifest name. */
const v3_local_content_chapter_t *v3_local_content_find_chapter(const v3_local_content_generation_t *generation,
                                                               const char *name);

/**
 * Atomically persist a complete generation descriptor.
 *
 * Object paths are never serialized. They are derived from each SHA-256
 * digest below cache_root/v3-local when the descriptor is loaded.
 */
error_t v3_local_content_generation_save(const char *cache_root,
                                         uint8_t overlay_id,
                                         uint32_t effective_version,
                                         v3_local_content_generation_t *generation);

/**
 * Load a persisted generation descriptor and validate every chapter's
 * digest-derived name, object path and file size.
 */
error_t v3_local_content_generation_load(const char *cache_root,
                                         uint8_t overlay_id,
                                         const char *ruid,
                                         uint32_t effective_version,
                                         v3_local_content_generation_t *generation);
