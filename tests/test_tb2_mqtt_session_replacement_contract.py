#!/usr/bin/env python3
"""Focused guardrails for replacing stale authenticated TB2 MQTT sessions."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttSessionReplacementContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mqtt = (ROOT / "src" / "mqtt_server.c").read_text(encoding="utf-8")
        cls.proxy = (ROOT / "src" / "tb2_mqtt_passthrough.c").read_text(encoding="utf-8")

    def test_packet_aware_takeover_waits_for_completed_connack(self):
        def function(name):
            match = re.search(r"static error_t " + name + r"\([^;]*?\n\{[\s\S]*?\n\}", self.proxy)
            self.assertIsNotNone(match, name)
            return match.group()

        accept = function("tb2_mqtt_accept_connect")
        self.assertIn("saved->pending_owner = session->owner", accept)
        self.assertNotIn("tb2_mqtt_box_terminal", accept)
        self.assertNotIn("tb2_mqtt_box_writes_cancel", accept)
        pump = function("tb2_mqtt_box_write_pump")
        write = pump.index("tb2_mqtt_tls_write_step")
        finish = pump.index("tb2_mqtt_finish_connect")
        self.assertLess(write, pump.index("if (error) break;", write))
        self.assertLess(pump.index("osReleaseMutex", write), finish)
        complete = function("tb2_mqtt_finish_connect")
        self.assertLess(complete.index("tb2_mqtt_restore_subscriptions"), complete.index("tb2_mqtt_box_terminal"))
        self.assertLess(complete.index("tb2_mqtt_box_terminal"), complete.index("session->established = session->connack_sent = TRUE"))

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
        self.assertLess(connect.index("conn->established = TRUE"), connect.index("mqtt_connection_replace_existing_box_sessions(conn)"))

        start = self.mqtt.index("tb2_mqtt_passthrough_forward_initial")
        passthrough = self.mqtt[start:self.mqtt.index("if (handled)", start)]
        self.assertIn("if (!error && tb2_mqtt_passthrough_is_established(conn->passthrough))", passthrough)
        self.assertIn("conn->established = TRUE", passthrough)
        self.assertIn("mqtt_connection_replace_existing_box_sessions(conn)", passthrough)

    def test_incomplete_connection_is_closed_after_bounded_setup_time(self):
        task = self.mqtt[
            self.mqtt.index("void mqtt_server_task()") :
            self.mqtt.index("void mqtt_server_deinit()")
        ]
        self.assertIn("#define MQTT_CONNECTION_ESTABLISH_TIMEOUT_MS 15000U", self.mqtt)
        self.assertIn("conn->accepted_at = osGetSystemTime()", task)
        self.assertIn("!conn->established", task)
        self.assertIn("elapsed >= MQTT_CONNECTION_ESTABLISH_TIMEOUT_MS", task)
        self.assertIn('mqtt_connection_close(conn, "establishment timeout")', task)

    def test_topic_promotion_replaces_only_on_first_promotion(self):
        update = self.mqtt[
            self.mqtt.index("static void mqtt_connection_update_context") :
            self.mqtt.index("static char_t *mqtt_server_cert")
        ]
        self.assertIn("bool_t was_box_connection = conn->box_connection", update)
        self.assertIn("if (!was_box_connection)", update)
        self.assertIn("mqtt_connection_replace_existing_box_sessions(conn)", update)

    def test_accepted_socket_has_io_timeout_before_tls_handshake(self):
        task = self.mqtt[
            self.mqtt.index("void mqtt_server_task()") :
            self.mqtt.index("void mqtt_server_deinit()")
        ]
        self.assertTrue(
            "#define MQTT_CONNECTION_IO_TIMEOUT_MS 300U" in self.mqtt,
            "Accepted MQTT sockets must use the 300 ms I/O timeout",
        )
        timeout = "socketSetTimeout(conn->socket, MQTT_CONNECTION_IO_TIMEOUT_MS)"
        self.assertIn(timeout, task)
        self.assertLess(task.index(timeout), task.index("conn->tlsContext = tlsInit()"))
        self.assertNotIn("socketSetTimeout(conn->socket, 0)", task)
        self.assertIn("error != ERROR_TIMEOUT", task)


if __name__ == "__main__":
    unittest.main()
