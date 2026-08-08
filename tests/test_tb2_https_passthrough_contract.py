#!/usr/bin/env python3
"""Static guardrails for the TB2 HTTPS pre-parser passthrough contract."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2HttpsPassthroughContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.http_server = (ROOT / "src/cyclone/cyclone_tcp/http/http_server.c").read_text(
            encoding="utf-8"
        )
        cls.passthrough = (ROOT / "src/tb2_https_passthrough.c").read_text(
            encoding="utf-8"
        )
        cls.identity = (ROOT / "src/tb2_client_identity.c").read_text(
            encoding="utf-8"
        )
        cls.status = (ROOT / "src/tb2_https_status.c").read_text(encoding="utf-8")
        cls.cloud_request = (ROOT / "src/cloud_request.c").read_text(encoding="utf-8")
        cls.cloud_handler = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")

    def test_passthrough_runs_before_http_parser(self):
        callback = self.http_server.index("postTlsCallback(connection, &handled)")
        parser = self.http_server.index("httpReadRequestHeader(connection)")
        self.assertLess(callback, parser)

    def test_capture_is_flushed_before_forwarding(self):
        forwarder = self.passthrough[
            self.passthrough.index("static error_t tb2_forward_ready") :
        ]
        capture = forwarder.index("tb2_capture_chunk(capture")
        send = forwarder.index("tb2_tls_write_all(destination")
        self.assertLess(capture, send)
        self.assertIn("fsFlushFile(capture->traffic)", self.passthrough)

    def test_passthrough_does_not_use_http_cloud_handlers(self):
        forbidden = (
            "cloud_request(",
            "httpClientCreateRequest(",
            "httpClientSetMethod(",
            "httpClientAddHeaderField(",
            "httpSendErrorResponse(",
        )
        for symbol in forbidden:
            self.assertNotIn(symbol, self.passthrough)

    def test_passthrough_uses_only_tb2_client_identity(self):
        self.assertIn("tb2_client_identity_resolve(settings, NULL)", self.passthrough)
        self.assertIn("tb2_client_identity_resolve(settings, NULL)", self.cloud_request)
        self.assertNotIn("settings->internal.client.", self.passthrough)
        self.assertNotIn("client_tb1", self.identity)

    def test_tb2_identity_defaults_global_and_uses_only_explicit_overlay(self):
        self.assertIn('"global_default"', self.identity)
        self.assertIn('"box_overlay"', self.identity)
        self.assertIn("settings_is_overlayed_id", self.identity)
        self.assertIn("? &box_settings->internal.client_tb2", self.identity)
        self.assertIn(": &get_settings()->internal.client_tb2", self.identity)
        self.assertIn("refusing global fallback", self.identity)

    def test_capture_defaults_are_registered(self):
        self.assertIn('"cloud.tb2_enabled"', self.settings)
        self.assertIn('"cloud.tb2_v3_enabled"', self.settings)
        self.assertIn('"cloud.tb2_capture_enabled"', self.settings)
        self.assertIn('"cloud.tb2_passthrough_enabled"', self.settings)
        self.assertIn("OPTION_BOOL(CLOUD_TB2_V3_SETTING, &settings->cloud.tb2_v3_enabled, TRUE", self.settings)
        self.assertIn("OPTION_INTERNAL_BOOL(CLOUD_TB2_CAPTURE_SETTING", self.settings)
        self.assertIn("OPTION_INTERNAL_BOOL(CLOUD_TB2_LEGACY_PASSTHROUGH_SETTING", self.settings)
        self.assertIn('"data/diagnostics/tb2-https-passthrough"', self.settings)
        self.assertIn("&settings->cloud.tb2_capture_max_mib, 4096", self.settings)

    def test_runtime_uses_only_the_single_proxy_setting(self):
        gate = self.passthrough[
            self.passthrough.index("error_t tb2_https_passthrough_post_tls") :
            self.passthrough.index("error_t tb2_https_passthrough_write_status")
        ]
        self.assertIn("tb2_https_proxy_is_configured()", gate)
        self.assertIn("!box_settings->cloud.tb2_enabled", gate)
        self.assertNotIn("tb2_passthrough_enabled", gate)

    def test_v20_migration_preserves_active_mode_and_excludes_conflicts(self):
        migration = self.settings[
            self.settings.rindex("static void settings_migrate_tb2_https_modes") :
            self.settings.rindex("static bool settings_migrate_id")
        ]
        self.assertIn(
            "settings->cloud.tb2_enabled &&\n                                             settings->cloud.tb2_passthrough_enabled",
            migration,
        )
        self.assertIn(
            "settings->cloud.tb2_v3_enabled = !transparentProxyEnabled && settings->cloud.enabled;",
            migration,
        )
        self.assertIn("settings->configVersion < 20", self.settings)

    def test_v23_migration_preserves_explicit_modes_and_retires_capture_switch(self):
        migration = self.settings[
            self.settings.index("static void settings_migrate_tb2_https_capture") :
            self.settings.rindex("static bool settings_migrate_id")
        ]
        self.assertIn("settings_tb2_proxy_loaded", migration)
        self.assertIn("settings_tb2_v3_loaded", migration)
        self.assertIn("Settings_Overlay[0].cloud.tb2_enabled", migration)
        self.assertIn("Settings_Overlay[0].cloud.tb2_v3_enabled", migration)
        self.assertIn("settings->cloud.tb2_capture_enabled = true;", migration)
        self.assertIn("captureOption->overlayed = false;", migration)
        self.assertIn("settings->configVersion < 23", self.settings)

    def test_mode_setter_and_reset_keep_modes_mutually_exclusive(self):
        selector = self.settings[
            self.settings.rindex("static void settings_select_tb2_https_mode") :
            self.settings.rindex("static void settings_normalize_tb2_https_modes")
        ]
        setter = self.settings[
            self.settings.index("bool settings_set_bool_id") :
            self.settings.index("bool settings_reset_id")
        ]
        reset = self.settings[
            self.settings.index("bool settings_reset_id") :
            self.settings.index("int32_t settings_get_signed")
        ]
        self.assertIn("disabledOption->overlayed = true;", selector)
        self.assertIn("settings_select_tb2_https_mode(settingsId, item);", setter)
        self.assertIn("CLOUD_TB2_CAPTURE_SETTING", setter)
        self.assertIn("CLOUD_TB2_LEGACY_PASSTHROUGH_SETTING", setter)
        self.assertIn("is deprecated and cannot be changed", setter)
        self.assertIn("settings_select_tb2_https_mode(settingsId, item);", reset)

    def test_passthrough_logs_why_a_connection_is_skipped(self):
        self.assertIn("TB2 HTTPS passthrough skipped: client certificate identity unavailable", self.passthrough)
        self.assertIn("TB2 HTTPS passthrough skipped: client certificate is not mapped to an active overlay", self.passthrough)
        self.assertIn("TB2 HTTPS passthrough skipped: no eligible TB2 overlay resolved", self.passthrough)

    def test_capture_is_mandatory_without_changing_raw_capture_format(self):
        self.assertNotIn("tb2_capture_enabled", self.passthrough)
        self.assertNotIn("capture->enabled", self.passthrough)
        self.assertNotIn("capture.enabled", self.passthrough)
        self.assertIn('cJSON_AddStringToObject(entry, "data_base64", encoded)', self.passthrough)
        self.assertIn("tb2_rotate_completed_captures(box_settings);", self.passthrough)
        self.assertIn('tb2_https_status_tunnel_finish(FALSE, "capture_open_failed")', self.passthrough)

    def test_v3_handlers_use_dedicated_mode_and_request_context(self):
        self.assertNotRegex(self.cloud_handler, r"cloud\.enabled.*enableV3")
        self.assertNotRegex(
            self.cloud_handler,
            r"cloud_request_(?:get|post)\([^\n]*remote_hostname_tb2",
        )
        self.assertGreaterEqual(self.cloud_handler.count("cloud_request_tb2_get("), 4)
        self.assertGreaterEqual(self.cloud_handler.count("cloud_request_tb2_post("), 3)
        self.assertIn("settings->cloud.tb2_v3_enabled", self.cloud_request)
        self.assertIn("settings->cloud.remote_port_tb2", self.cloud_request)
        self.assertIn("httpClientTlsInitCallbackClientAuthTb2", self.cloud_request)

    def test_v3_content_meta_honors_per_tonie_no_cloud_setting(self):
        helper = self.cloud_handler[
            self.cloud_handler.index("static bool_t tonie_cloud_access_allowed") :
            self.cloud_handler.index("void markCustomTonie")
        ]
        self.assertIn("!tonieInfo->json.nocloud || tonieInfo->json.cloud_override", helper)

        handler = self.cloud_handler[
            self.cloud_handler.index("error_t handleCloudContentMetaV3") :
            self.cloud_handler.index("error_t handleCloudChapterV3")
        ]
        local_response = handler.index("tonieInfo->exists && tonieInfo->valid")
        cloud_gate = handler.index("tonie_cloud_access_allowed(tonieInfo)", local_response)
        cloud_request = handler.index("cloud_request_tb2_get", cloud_gate)
        self.assertLess(local_response, cloud_gate)
        self.assertLess(cloud_gate, cloud_request)
        self.assertIn("V3 content meta marked as no cloud", handler)

    def test_v3_freshness_honors_per_tonie_cloud_override(self):
        handler = self.cloud_handler[
            self.cloud_handler.index("void process_freshness_check") :
            self.cloud_handler.index("void freshness_mark_content_mapping_changed")
        ]
        self.assertIn("allow_cloud_override && tonieInfo->json.cloud_override", handler)
        self.assertNotIn("if (!tonieInfo->json.nocloud)", handler)

        v1_call = self.cloud_handler[
            self.cloud_handler.index("error_t handleCloudFreshnessCheck(") :
            self.cloud_handler.index("error_t handleCloudFreshnessCheckV3(")
        ]
        v3_call = self.cloud_handler[
            self.cloud_handler.index("error_t handleCloudFreshnessCheckV3(") :
            self.cloud_handler.index("error_t handleCloudCheckOtaV3(")
        ]
        self.assertRegex(v1_call, r"process_freshness_check\([^;]+, FALSE\);")
        self.assertRegex(v3_call, r"process_freshness_check\([^;]+, TRUE\);")

    def test_transport_errors_do_not_switch_modes(self):
        self.assertNotIn("settings_set_bool", self.cloud_request)
        self.assertNotIn("settings_set_bool", self.passthrough)

    def test_unified_status_reports_modes_and_independent_transports(self):
        self.assertIn("return tb2_https_status_write(connection);", self.passthrough)
        for field in (
            '"mode"',
            '"mode_counts"',
            '"v3"',
            '"transparent"',
            '"last_http_status"',
            '"active_requests"',
            '"active_sessions"',
        ):
            self.assertIn(field, self.status)
        self.assertIn('return "mixed";', self.status)
        self.assertIn('"request_active"', self.status)
        self.assertIn("tb2_https_status_v3_start(uri)", self.cloud_request)
        self.assertIn("tb2_https_status_v3_finish(error, http_status)", self.cloud_request)


if __name__ == "__main__":
    unittest.main(verbosity=2)
