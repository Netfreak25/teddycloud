#ifndef ORIGINAL_CONTENT_CACHE_H
#define ORIGINAL_CONTENT_CACHE_H

#include "handler.h"
#include "cJSON.h"
#include "tb2_ruid.h"

#define ORIGINAL_CONTENT_CACHE_CAPACITY 32U
#define ORIGINAL_CONTENT_CACHE_OWNER_SIZE 128U

typedef enum
{
    ORIGINAL_CACHE_TAF = 0,
    ORIGINAL_CACHE_V3 = 1,
    ORIGINAL_CACHE_FORMAT_COUNT = 2
} original_cache_format_t;

/** Owned request snapshot. Adapters must not resolve identities from live
 * settings; use these copies in their HTTP/TLS callback contexts instead. */
typedef struct
{
    settings_t settings;
    settings_cert_t tb1_identity;
    settings_cert_t tb2_identity;
    char owner[ORIGINAL_CONTENT_CACHE_OWNER_SIZE];
    char ruid[TB2_RUID_SIZE];
    char upstream_ruid[TB2_RUID_SIZE];
    uint8_t auth[TONIE_AUTH_TOKEN_LENGTH];
    bool automatic;
    bool both_formats;
    bool refresh_v3;
    const char *refresh_result; /* Static outcome, set only after a successful cloud check. */
    original_cache_format_t preferred;
    uint32_t observed_version;
} original_content_cache_job_t;

void original_content_cache_init(void);
void original_content_cache_deinit(void);

/** Global playback opt-in only applies to a configured mixed TB1/TB2 setup. */
bool original_content_cache_playback_enabled(void);

/** Enqueue an original operation. Automatic calls remember terminal results
 * for this observed version, so repeated cache hits do not become retries.
 * Returns OUT_OF_RESOURCES on capacity, ACCESS_DENIED for policy or ABORTED
 * when the automatic feature does not apply. The optional reason is static. */
error_t original_content_cache_submit(settings_t *settings, const char *ruid,
    original_cache_format_t preferred, bool automatic, uint32_t observed_version,
    const char **reason);

/** Explicit V3-only refresh for configured TB2s sharing this content root.
 * Appends owned per-box admission results to targets; NO_ERROR means at least
 * one accepted job, never that any download has already completed. */
error_t original_content_cache_refresh_group(settings_t *settings,
    const char *ruid, cJSON *targets);

/** Read the per-tag original preference without repairing/writing metadata.
 * Missing/invalid preference falls back to the existing generation default. */
original_cache_format_t original_content_cache_preferred(settings_t *settings,
    const char *ruid);

/** NULL means current network/cache policy permits this format. */
const char *original_content_cache_blocked_reason(settings_t *settings,
    const char *ruid, original_cache_format_t format);

/** Recheck stable overlay, source, authentication, roots and effective flags
 * before an adapter starts network I/O or publishes downloaded content. */
bool original_content_cache_job_allowed(const original_content_cache_job_t *job,
    original_cache_format_t format);

/** Same current original/cache/import policy, but no cloud/certificate
 * requirement: exclusively for reuse/import of already-complete local data. */
bool original_content_cache_job_current(const original_content_cache_job_t *job,
    original_cache_format_t format);

/** Explain a network rejection using owned job credentials, never resolving
 * missing snapshot certificates from current global settings. */
const char *original_content_cache_job_blocked_reason(
    const original_content_cache_job_t *job, original_cache_format_t format);

/** Add only the optional download status to an existing cacheState object. */
void original_content_cache_add_status(cJSON *cache_state,
    const settings_t *settings, const char *ruid);

/** Format-specific adapter implemented next to the existing download paths.
 * Reuse complete local data before attempting network I/O; import only if the
 * snapshot allows it. No Source assignment, Freshness or secondary scheduling.
 * A policy/precondition rejection returns ACCESS_DENIED with a static reason. */
error_t original_content_cache_materialize(original_content_cache_job_t *job,
    original_cache_format_t format, bool import_to_library, const char **reason);

#endif
