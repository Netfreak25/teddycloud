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
        self.assertIn('"cloud.tb2_passthrough_enabled"', self.settings)
        self.assertIn('"data/diagnostics/tb2-https-passthrough"', self.settings)
        self.assertIn("&settings->cloud.tb2_capture_max_mib, 4096", self.settings)

    def test_passthrough_requires_both_enable_flags(self):
        gate = self.passthrough[
            self.passthrough.index("error_t tb2_https_passthrough_post_tls") :
            self.passthrough.index("error_t tb2_https_passthrough_write_status")
        ]
        self.assertIn("!global->cloud.tb2_enabled", gate)
        self.assertIn("!global->cloud.tb2_passthrough_enabled", gate)
        self.assertIn('!osStrcmp(item, "cloud.tb2_passthrough_enabled")', self.settings)
        self.assertIn("Settings_Overlay[0].cloud.tb2_passthrough_enabled = false", self.settings)

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
