#ifndef TB2_HTTPS_STATUS_H
#define TB2_HTTPS_STATUS_H

#include <stddef.h>
#include <stdint.h>

#include "compiler_port.h"
#include "error.h"

struct _HttpConnection;

error_t tb2_https_status_init(void);
void tb2_https_status_deinit(void);

void tb2_https_status_v3_start(const char *endpoint);
void tb2_https_status_v3_finish(error_t error, uint32_t http_status);

void tb2_https_status_tunnel_start(void);
void tb2_https_status_tunnel_set_state(const char *state);
void tb2_https_status_tunnel_add_bytes(bool_t box_to_upstream, size_t length);
void tb2_https_status_tunnel_finish(bool_t success, const char *error_code);

error_t tb2_https_status_write(struct _HttpConnection *connection);

#endif
