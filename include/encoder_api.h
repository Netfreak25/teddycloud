#pragma once

#include "handler.h"

/** Startup-only orphan cleanup; call before accepting Web API requests. */
void encoder_uploads_init(void);

error_t handleApiEncoderCapabilities(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx);
error_t handleApiNativeImportStart(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx);
error_t handleApiNativeImportOperation(HttpConnection *connection, const char_t *uri,
    const char_t *query, client_ctx_t *client_ctx);

/** Validate a single complete, unmultiplexed Ogg/Opus stream with bounded RAM. */
error_t encoder_validate_opus_file(const char *path, double *duration_seconds);
