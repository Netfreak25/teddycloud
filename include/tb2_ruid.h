#ifndef TB2_RUID_H
#define TB2_RUID_H

#include <stdint.h>

#include "compiler_port.h"

#define TB2_RUID_HEX_LENGTH 16U
#define TB2_RUID_SIZE (TB2_RUID_HEX_LENGTH + 1U)
#define TB2_SYSTEM_RUID_PREFIX "00000AF0"
#define TB2_SYSTEM_RUID_PREFIX_LENGTH 8U

typedef enum
{
    TB2_RUID_INVALID = 0,
    TB2_RUID_CONTENT,
    TB2_RUID_SYSTEM,
} tb2_ruid_kind_t;

/**
 * Validate and copy a TB2 rUID into the canonical uppercase representation.
 */
bool_t tb2_ruid_canonicalize(const char *value, char canonical[TB2_RUID_SIZE]);

/** Convert a textual TB2 rUID to TeddyCloud's byte-swapped UID storage form. */
bool_t tb2_ruid_to_uid(const char *value, uint64_t *uid);

/** Convert TeddyCloud's byte-swapped UID storage form to a canonical TB2 rUID. */
void tb2_ruid_from_uid(uint64_t uid, char canonical[TB2_RUID_SIZE]);

/** Classify a valid rUID as normal content or reserved TB2 system content. */
tb2_ruid_kind_t tb2_ruid_classify(const char *value);

#endif
