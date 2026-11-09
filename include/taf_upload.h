#pragma once
#include "core/net.h"
#include "http/http_server.h"

/* Receive one TAF into a sibling temporary file, validate, then atomically replace. */
error_t taf_upload_receive(HttpConnection *connection, const char *target);
bool_t taf_upload_validate(const char *path);
