#!/usr/bin/env python3
"""Static architecture contract for transparent TB2 MQTT forwarding."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttPassthroughContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")
        cls.passthrough = (ROOT / "src/tb2_mqtt_passthrough.c").read_text(
            encoding="utf-8"
        )

    def test_passthrough_is_selected_before_packet_parser(self):
        start = self.server.index("tb2_mqtt_passthrough_start")
        parser = self.server.index("size_t processed_total", start)
        self.assertLess(start, parser)
        self.assertIn("continue;", self.server[start:parser])

    def test_capture_is_flushed_before_forwarding(self):
        function = self.passthrough[
            self.passthrough.index("static error_t tb2_mqtt_forward_bytes") :
        ]
        capture = function.index("tb2_mqtt_capture_chunk")
        forward = function.index("tb2_mqtt_tls_write_all")
        self.assertLess(capture, forward)

    def test_module_does_not_construct_mqtt_packets(self):
        for forbidden in ("CONNACK", "SUBACK", "PUBACK", "PINGRESP", "handle_mqtt_"):
            self.assertNotIn(forbidden, self.passthrough)


if __name__ == "__main__":
    unittest.main(verbosity=2)
