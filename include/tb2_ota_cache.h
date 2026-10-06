#ifndef TB2_OTA_CACHE_H
#define TB2_OTA_CACHE_H

#include "handler.h"

/** Start/stop the process-owned, bounded counterpart downloader. */
void tb2_ota_cache_init(void);
void tb2_ota_cache_deinit(void);

/** Apply the global opt-in without waiting for a download; drops pending work. */
void tb2_ota_cache_set_enabled(bool enabled);

/** Forward an OTA check unchanged, observing at most 16 KiB when opted in. */
error_t tb2_ota_cache_check(HttpConnection *connection, const char *uri,
                            const char *query, client_ctx_t *client,
                            const uint8_t *body, size_t length);

typedef error_t (*tb2_ota_fallback_t)(HttpConnection *, const char *, const char *, client_ctx_t *);

/** Handle a manifest image or serialize the existing handler with active writes. */
error_t tb2_ota_cache_serve(HttpConnection *connection, const char *uri,
                              const char *query, client_ctx_t *client,
                              tb2_ota_fallback_t fallback);

#endif
