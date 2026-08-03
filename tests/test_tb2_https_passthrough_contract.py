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
        self.assertIn("settings->internal.client_tb2", self.passthrough)
        self.assertNotIn("settings->internal.client.", self.passthrough)

    def test_capture_defaults_are_registered(self):
        self.assertIn('"cloud.tb2_enabled"', self.settings)
        self.assertIn('"cloud.tb2_v3_enabled"', self.settings)
        self.assertIn('"cloud.tb2_capture_enabled"', self.settings)
        self.assertIn('"cloud.tb2_passthrough_enabled"', self.settings)
        self.assertIn("OPTION_INTERNAL_BOOL(CLOUD_TB2_LEGACY_PASSTHROUGH_SETTING", self.settings)
        self.assertIn('"data/diagnostics/tb2-https-passthrough"', self.settings)
        self.assertIn("&settings->cloud.tb2_capture_max_mib, 4096", self.settings)

    def test_legacy_runtime_gate_mirrors_the_single_proxy_setting(self):
        gate = self.passthrough[
            self.passthrough.index("error_t tb2_https_passthrough_post_tls") :
            self.passthrough.index("error_t tb2_https_passthrough_write_status")
        ]
        self.assertIn("!global->cloud.tb2_enabled", gate)
        self.assertIn("!global->cloud.tb2_passthrough_enabled", gate)
        self.assertIn(
            "settings->cloud.tb2_passthrough_enabled = settings->cloud.tb2_enabled;",
            self.settings,
        )

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
        self.assertIn("CLOUD_TB2_LEGACY_PASSTHROUGH_SETTING", setter)
        self.assertIn("is deprecated and cannot be changed", setter)
        self.assertIn("settings_select_tb2_https_mode(settingsId, item);", reset)

    def test_passthrough_logs_why_a_connection_is_skipped(self):
        self.assertIn("TB2 HTTPS passthrough skipped: client certificate identity unavailable", self.passthrough)
        self.assertIn("TB2 HTTPS passthrough skipped: client certificate is not mapped to an active overlay", self.passthrough)
        self.assertIn("TB2 HTTPS passthrough skipped: no eligible TB2 overlay resolved", self.passthrough)

    def test_status_distinguishes_standby_and_armed(self):
        status = self.passthrough[
            self.passthrough.index("error_t tb2_https_passthrough_write_status") :
        ]
        self.assertIn('"passthrough_enabled"', status)
        self.assertIn('state = "standby"', status)
        self.assertIn('state = "armed"', status)

if __name__ == "__main__":
    unittest.main(verbosity=2)
