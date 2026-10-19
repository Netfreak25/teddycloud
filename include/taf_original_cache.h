#pragma once

#include "settings.h"

/** Check a TAF header and its exact complete size, without hashing or mutations. */
bool taf_original_file_complete(const char *path);

/**
 * Resolve an original RUID cache reference to a complete, validated library TAF.
 * Does not create directories, change content metadata or consult import switches.
 * raw_content_path must belong to settings->internal.contentdirfull (8/8 hex).
 * The caller owns the returned path; NULL means no usable reference.
 */
char *taf_original_cache_resolve(const char *raw_content_path, const settings_t *settings);

/**
 * Import a complete original TAF without assigning a user source. Publish the
 * library file and reference before removing the raw RUID file. Any error keeps
 * the raw file; a cleanup error can leave an extra complete copy. The caller is
 * responsible for import policy and for excluding concurrent raw-file writers.
 */
error_t taf_original_cache_import(const char *raw_content_path, const settings_t *settings);
