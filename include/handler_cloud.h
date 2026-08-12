#ifndef _HANDLER_CLOUD_H
#define _HANDLER_CLOUD_H

#include "debug.h"

#include "core/net.h"
#include "core/ethernet.h"
#include "core/ip.h"
#include "core/tcp.h"
#include "http/http_server.h"
#include "http/http_server_misc.h"

#include "handler.h"

#define BODY_BUFFER_SIZE 4096

error_t handleCloudTime(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudOTA(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudCheckOtaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudOtaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudSetupStatusV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudChapterV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudContentMetaV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudLog(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudClaim(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudContent(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx, bool_t noPassword);
error_t handleCloudContentV1(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudContentV2(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudContentDownloadV3(HttpConnection *connection, const char *ruid,
                                     const contentJson_t *content,
                                     client_ctx_t *client_ctx);
bool_t v3_original_content_metadata_complete(settings_t *settings,
                                             tonie_info_t *tonie_info,
                                             const char *ruid);
error_t handleCloudFreshnessCheck(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudFreshnessCheckV3(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleCloudReset(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
error_t handleContent(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx);
void freshness_mark_content_mapping_changed(settings_t *target_settings, const char *ruid,
                                            bool_t source_changed);
void freshness_cache_sync_source_changed_uids(settings_t *settings);
bool_t freshness_confirm_v3_content_version(settings_t *settings, const char *ruid,
                                            uint64_t content_version);
void v3_tap_playback_observe(settings_t *settings,
                             const char *previous_ruid,
                             const char *current_ruid,
                             bool_t content_version_valid,
                             uint32_t content_version);

#endif
