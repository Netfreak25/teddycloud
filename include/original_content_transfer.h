#pragma once
#include "original_content_cache.h"

error_t original_content_download_v3(original_content_cache_job_t *job);
/** Publish a complete RUID staging file; serialize with original Library imports. */
error_t original_content_publish_taf(const char *staged, const char *raw,
    settings_t *settings, bool import_to_library);
