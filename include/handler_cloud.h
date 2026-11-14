#ifndef _HANDLER_CLOUD_H
#define _HANDLER_CLOUD_H

#include "debug.h"
#include "original_content_cache.h"

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
/** Invalidate only runtime original routes sharing this content.json root. */
void v3_native_content_source_changed(settings_t *target_settings, const char *ruid);
void freshness_cache_sync_source_changed_uids(settings_t *settings);
/** Accepted cloud hint; schedules V3-only work, never performs network I/O here. */
bool_t freshness_original_cloud_hint(settings_t *settings, const char *ruid);
uint64_t freshness_original_cloud_hint_for_target(settings_t *settings, const char *ruid);
uint64_t freshness_original_cloud_pending_for_target(settings_t *settings, const char *ruid);
/** Complete-cache handoff, before optional Library import; no source assignment. */
void original_content_cache_v3_complete(original_content_cache_job_t *job, uint32_t version);
/** Merge validated original-cache decisions before the one HTTP response. */
void freshness_original_finish_response(cbr_ctx_t *ctx);
bool_t freshness_confirm_v3_content_version(settings_t *settings, const char *ruid,
                                            uint64_t content_version);
void v3_tap_playback_observe(settings_t *settings,
                             const char *previous_ruid,
                             const char *current_ruid,
                             bool_t content_version_valid,
                             uint32_t content_version);

#endif
