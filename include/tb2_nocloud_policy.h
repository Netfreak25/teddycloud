#ifndef TB2_NOCLOUD_POLICY_H
#define TB2_NOCLOUD_POLICY_H

#include "contentJson.h"
#include "settings.h"
#include "tb2_ruid.h"

typedef struct
{
    uint8_t overlay_id;
    uint64_t uid;
    char ruid[TB2_RUID_SIZE];
    tb2_ruid_kind_t kind;
    bool_t metadata_exists;
    bool_t nocloud;
    bool_t cloud_override;
} tb2_nocloud_policy_t;

/**
 * Resolve the effective per-overlay NoCloud policy directly from content JSON.
 * System rUIDs are classified separately and never inherit Tonie content policy.
 */
bool_t tb2_nocloud_policy_from_content(settings_t *settings,
                                       const char *ruid,
                                       const contentJson_t *content,
                                       tb2_nocloud_policy_t *policy);

/**
 * Resolve the effective per-overlay NoCloud policy from persisted metadata.
 * Missing legacy boolean fields are compatible and default to false. Invalid
 * files return false so privacy-sensitive callers can fail closed.
 */
bool_t tb2_nocloud_policy_resolve(settings_t *settings,
                                  const char *ruid,
                                  tb2_nocloud_policy_t *policy);

/** True only for normal content explicitly protected from upstream access. */
bool_t tb2_nocloud_policy_blocks_upstream(const tb2_nocloud_policy_t *policy);

#endif
