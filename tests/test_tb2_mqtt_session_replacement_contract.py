#!/usr/bin/env python3
"""Focused guardrails for replacing stale authenticated TB2 MQTT sessions."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttSessionReplacementContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mqtt = (ROOT / "src" / "mqtt_server.c").read_text(encoding="utf-8")

    def test_replacement_matches_overlay_and_canonical_box_id(self):
        helper = self.mqtt[
            self.mqtt.index("static void mqtt_connection_replace_existing_box_sessions") :
            self.mqtt.index("static bool mqtt_topic_match")
        ]
        self.assertIn("candidate->box_overlay_id != current->box_overlay_id", helper)
        self.assertGreaterEqual(helper.count("settings_canonicalize_box_id"), 2)
        self.assertIn('mqtt_connection_close(candidate, "superseded by reconnect")', helper)

    def test_replacement_runs_only_after_successful_connection_setup(self):
        connect = self.mqtt[
            self.mqtt.index("static error_t handle_mqtt_connect") :
            self.mqtt.index("static error_t handle_mqtt_pingreq")
        ]
        self.assertLess(connect.index("connack write failed"), connect.index("mqtt_connection_replace_existing_box_sessions(conn)"))

        passthrough = self.mqtt[
            self.mqtt.index("tb2_mqtt_passthrough_forward_initial") - 500 :
            self.mqtt.index("tb2_mqtt_passthrough_forward_initial") + 500
        ]
        self.assertIn("if (!error)", passthrough)
        self.assertIn("mqtt_connection_replace_existing_box_sessions(conn)", passthrough)

    def test_topic_promotion_replaces_only_on_first_promotion(self):
        update = self.mqtt[
            self.mqtt.index("static void mqtt_connection_update_context") :
            self.mqtt.index("static char_t *mqtt_server_cert")
        ]
        self.assertIn("bool_t was_box_connection = conn->box_connection", update)
        self.assertIn("if (!was_box_connection)", update)
        self.assertIn("mqtt_connection_replace_existing_box_sessions(conn)", update)


if __name__ == "__main__":
    unittest.main()
