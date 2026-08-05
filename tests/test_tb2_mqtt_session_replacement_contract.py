#!/usr/bin/env python3
"""Focused contract for replacing stale TB2 MQTT sessions after reconnect."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttSessionReplacementContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")

    def section(self, start: str, end: str) -> str:
        begin = self.server.index(start)
        finish = self.server.index(end, begin + len(start))
        return self.server[begin:finish]

    def test_replacement_matches_authenticated_overlay_and_canonical_box_id(self):
        replacement = self.section(
            "static void mqtt_connection_replace_existing_box_sessions(",
            "static bool mqtt_topic_match(",
        )
        self.assertIn("candidate == current", replacement)
        self.assertIn("!candidate->active", replacement)
        self.assertIn("!candidate->box_connection", replacement)
        self.assertIn(
            "candidate->box_overlay_id != current->box_overlay_id", replacement
        )
        self.assertGreaterEqual(
            replacement.count("settings_canonicalize_box_id"), 2
        )
        self.assertIn("osStrcmp(candidate_box_id, current_box_id) != 0", replacement)
        self.assertIn(
            'mqtt_connection_close(candidate, "superseded by reconnect")',
            replacement,
        )
        self.assertNotIn("client_id", replacement.lower())

    def test_direct_session_replaces_old_only_after_successful_connack(self):
        connect = self.section(
            "static error_t handle_mqtt_connect(",
            "static error_t handle_mqtt_pingreq(",
        )
        write_check = connect.index("error != NO_ERROR || written != sizeof(connack)")
        replacement = connect.index(
            "mqtt_connection_replace_existing_box_sessions(conn)"
        )
        self.assertLess(write_check, replacement)
        self.assertIn('mqtt_connection_close(conn, "connack write failed")', connect)

    def test_proxy_session_replaces_old_only_after_initial_forward(self):
        task = self.section("void mqtt_server_task(", "void mqtt_server_deinit(")
        forward = task.index("tb2_mqtt_passthrough_forward_initial")
        replacement = task.index(
            "mqtt_connection_replace_existing_box_sessions(conn)", forward
        )
        self.assertLess(forward, replacement)
        self.assertIn("if (!error)", task[forward:replacement])

    def test_trusted_topic_mapping_replaces_only_on_first_promotion(self):
        update = self.section(
            "static void mqtt_connection_update_context(",
            "static char_t *mqtt_server_cert",
        )
        self.assertIn("bool_t was_box_connection = conn->box_connection", update)
        self.assertIn("mqtt_connection_has_trusted_client_cert(conn)", update)
        self.assertIn("if (!was_box_connection)", update)
        self.assertIn("mqtt_connection_replace_existing_box_sessions(conn)", update)

    def test_closing_old_session_preserves_persistent_freshness(self):
        close = self.section(
            "static void mqtt_connection_close(",
            "static void mqtt_connection_replace_existing_box_sessions(",
        )
        self.assertIn("freshnessCacheChanged", close)
        self.assertIn('mqtt_mark_fresh_tonies_pending', close)
        self.assertIn("mqtt_fresh_tonies_reset_connection(conn)", close)
        self.assertNotIn("freshness_cache_remove", close)


if __name__ == "__main__":
    unittest.main(verbosity=2)
