#ifndef TB2_HTTPS_PASSTHROUGH_H
#define TB2_HTTPS_PASSTHROUGH_H

#include "compiler_port.h"
#include "error.h"

struct _HttpConnection;

error_t tb2_https_passthrough_init(void);
void tb2_https_passthrough_deinit(void);

error_t tb2_https_passthrough_post_tls(struct _HttpConnection *connection, bool_t *handled);
error_t tb2_https_passthrough_write_status(struct _HttpConnection *connection);

#endif
