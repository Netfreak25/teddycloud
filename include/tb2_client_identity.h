#ifndef TB2_CLIENT_IDENTITY_H
#define TB2_CLIENT_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "contentJson.h"
#include "settings.h"
#include "tb2_ruid.h"

typedef struct
{
    char ruid[TB2_RUID_SIZE];
    const uint8_t *auth;
    size_t auth_len;
    bool_t overridden;
} tb2_content_identity_t;

/**
 * Resolve the client identity used for upstream TB2 HTTPS connections.
 *
 * The global client_tb2 identity is the default. A box identity is selected
 * only when at least one TB2 client-certificate setting is explicitly
 * overlaid for that box. An incomplete explicit override fails closed.
 */
const settings_cert_t *tb2_client_identity_resolve(settings_t *box_settings,
                                                   const char **source);

/**
 * Resolve the canonical rUID and authentication token for a TB2 content
 * request. Persisted lowercase override rUIDs remain readable; the selected
 * identity returned to new upstream requests is always canonical.
 */
bool_t tb2_content_identity_resolve(const char *requested_ruid,
                                    const uint8_t *requested_auth,
                                    const contentJson_t *content,
                                    tb2_content_identity_t *identity);

#endif
