#include "tb2_ruid.h"

#include <byteswap.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "os_port.h"

static bool_t tb2_ruid_is_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool_t tb2_ruid_canonicalize(const char *value, char canonical[TB2_RUID_SIZE])
{
    if (value == NULL || canonical == NULL ||
        osStrlen(value) != TB2_RUID_HEX_LENGTH)
    {
        return FALSE;
    }

    for (size_t index = 0; index < TB2_RUID_HEX_LENGTH; index++)
    {
        char current = value[index];
        if (!tb2_ruid_is_hex(current))
        {
            return FALSE;
        }
        canonical[index] = current >= 'a' && current <= 'f'
                               ? (char)(current - ('a' - 'A'))
                               : current;
    }
    canonical[TB2_RUID_HEX_LENGTH] = '\0';
    return TRUE;
}

bool_t tb2_ruid_to_uid(const char *value, uint64_t *uid)
{
    char canonical[TB2_RUID_SIZE];
    if (uid == NULL || !tb2_ruid_canonicalize(value, canonical))
    {
        return FALSE;
    }

    *uid = bswap_64(strtoull(canonical, NULL, 16));
    return TRUE;
}

void tb2_ruid_from_uid(uint64_t uid, char canonical[TB2_RUID_SIZE])
{
    if (canonical != NULL)
    {
        osSnprintf(canonical, TB2_RUID_SIZE, "%016" PRIX64, bswap_64(uid));
    }
}

tb2_ruid_kind_t tb2_ruid_classify(const char *value)
{
    char canonical[TB2_RUID_SIZE];
    if (!tb2_ruid_canonicalize(value, canonical))
    {
        return TB2_RUID_INVALID;
    }

    return osStrncmp(canonical, TB2_SYSTEM_RUID_PREFIX,
                     TB2_SYSTEM_RUID_PREFIX_LENGTH) == 0
               ? TB2_RUID_SYSTEM
               : TB2_RUID_CONTENT;
}
