#ifndef TB2_CLIENT_IDENTITY_H
#define TB2_CLIENT_IDENTITY_H

#include "settings.h"

/**
 * Resolve the client identity used for upstream TB2 HTTPS connections.
 *
 * The global client_tb2 identity is the default. A box identity is selected
 * only when at least one TB2 client-certificate setting is explicitly
 * overlaid for that box. An incomplete explicit override fails closed.
 */
const settings_cert_t *tb2_client_identity_resolve(settings_t *box_settings,
                                                   const char **source);

#endif
