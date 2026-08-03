#!/usr/bin/env python3
"""Static contract for level-5 TB2 MQTT CONNECT diagnostics."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttConnectLoggingContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(encoding="utf-8")
        cls.settings_source = (ROOT / "src/settings.c").read_text(encoding="utf-8")

    def test_connect_handler_traces_before_box_mapping(self):
        handler = self.server[
            self.server.index("static error_t handle_mqtt_connect") :
            self.server.index("static error_t handle_mqtt_pingreq")
        ]
        certificate = handler.index("mqtt_trace_tls_client_certificate(conn)")
        trace = handler.index("mqtt_trace_connect_details(payload, payload_len)")
        mapping = handler.index("mqtt_connection_update_context_from_cert(conn)")
        self.assertLess(certificate, trace)
        self.assertLess(trace, mapping)

    def test_diagnostics_require_explicit_setting(self):
        self.assertIn("bool log_connect_details;", self.settings_header)
        self.assertIn('OPTION_BOOL("mqtt_server.log_connect_details"', self.settings_source)
        tracer = self.server[
            self.server.index("static void mqtt_trace_connect_details") :
            self.server.index("static error_t handle_mqtt_connect")
        ]
        self.assertIn("!settings->mqtt_server.log_connect_details", tracer)
        self.assertIn("TRACE_DEBUG", tracer)

    def test_optional_certificate_request_and_response_are_logged(self):
        self.assertIn("TLS_CLIENT_AUTH_OPTIONAL", self.server)
        self.assertIn("MQTT TLS CertificateRequest mode=optional requested=true", self.server)
        self.assertIn("MQTT TLS client_certificate present=false", self.server)
        self.assertIn("MQTT TLS client_certificate present=true", self.server)
        self.assertIn("verification=not_enforced", self.server)

    def test_client_id_is_plain_but_credentials_remain_masked(self):
        tracer = self.server[
            self.server.index("static void mqtt_trace_masked_connect_field") :
            self.server.index("static error_t handle_mqtt_connect")
        ]
        self.assertIn("%s=<masked>", tracer)
        self.assertIn('mqtt_trace_plain_connect_field("client_id"', tracer)
        for field in ("will_topic", "will_payload", "username", "password"):
            self.assertIn(f'mqtt_trace_masked_connect_field("{field}"', tracer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
