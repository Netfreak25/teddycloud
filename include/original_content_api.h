#pragma once
#include "handler.h"

error_t handleApiOriginalCache(HttpConnection *connection, const char *uri,
    const char *query, client_ctx_t *client_ctx);
error_t handleApiNativeContent(HttpConnection *connection, const char *uri,
    const char *query, client_ctx_t *client_ctx);
