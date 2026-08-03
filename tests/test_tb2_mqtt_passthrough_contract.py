#!/usr/bin/env python3
"""Static architecture contract for the packet-aware TB2 MQTT proxy."""

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

    def function(self, start, end):
        return self.passthrough[
            self.passthrough.index(start) : self.passthrough.index(end)
        ]

    def test_proxy_is_selected_before_legacy_packet_parser(self):
        start = self.server.index("tb2_mqtt_passthrough_start")
        parser = self.server.index("size_t processed_total", start)
        self.assertLess(start, parser)
        self.assertIn("continue;", self.server[start:parser])

    def test_stream_reassembles_fragmented_and_coalesced_packets(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_stream",
            "static error_t tb2_mqtt_forward_ready",
        )
        for symbol in (
            "tb2_mqtt_stream_append",
            "while (stream->length > 0)",
            "tb2_mqtt_packet_size",
            "ERROR_WOULD_BLOCK",
            "packet_size > stream->length",
            "tb2_mqtt_process_packet",
            "osMemmove",
        ):
            self.assertIn(symbol, processor)

    def test_remaining_length_accepts_the_mqtt_protocol_limit(self):
        self.assertIn(
            "#define TB2_MQTT_MAX_REMAINING_LENGTH 268435455U", self.passthrough
        )
        parser = self.function(
            "static error_t tb2_mqtt_packet_size",
            "static tb2_mqtt_qos2_entry_t **tb2_mqtt_qos2_list",
        )
        self.assertIn("index <= 4", parser)
        self.assertIn("multiplier *= 128U", parser)
        self.assertIn("return ERROR_INVALID_LENGTH", parser)

    def test_every_packet_is_captured_before_any_write(self):
        recorder = self.function(
            "static error_t tb2_mqtt_record_packet",
            "static error_t tb2_mqtt_stream_append",
        )
        self.assertLess(
            recorder.index("tb2_mqtt_capture_packet"),
            recorder.index("tb2_mqtt_tls_write_all"),
        )
        self.assertLess(
            recorder.index("if (!forwarded)"),
            recorder.index("tb2_mqtt_tls_write_all"),
        )

    def test_publish_is_observed_then_filtered_in_both_directions(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        observer = processor.index("session->observer(")
        decision = processor.index("mqtt_forward_filter_should_block")
        record = processor.index("tb2_mqtt_record_packet", decision)
        self.assertLess(observer, decision)
        self.assertLess(decision, record)
        self.assertIn("box_to_upstream", processor)
        self.assertIn("!blocked", processor)

    def test_suppressed_qos_zero_one_and_two_are_completed_locally(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        self.assertIn("blocked && qos == 1", processor)
        self.assertIn("box_to_upstream, 4U, packet_id", processor)
        self.assertIn("blocked && qos == 2", processor)
        self.assertIn("tb2_mqtt_qos2_begin", processor)
        self.assertIn("box_to_upstream, 5U, packet_id", processor)
        self.assertIn("tb2_mqtt_qos2_complete", processor)
        self.assertIn("7U, packet_id", processor)

    def test_qos_two_duplicate_pubrel_remains_idempotent(self):
        state = self.function(
            "static tb2_mqtt_qos2_entry_t *tb2_mqtt_qos2_find",
            "static void tb2_mqtt_qos2_free",
        )
        self.assertIn("existing->completed = FALSE", state)
        self.assertIn("entry->completed = TRUE", state)
        self.assertNotIn("osFreeMem", state)
        self.assertIn("blocked_qos2_box", self.passthrough)
        self.assertIn("blocked_qos2_upstream", self.passthrough)

    def test_unknown_pubrel_is_forwarded_unchanged(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        complete = processor.index("tb2_mqtt_qos2_complete")
        ordinary = processor.index("if (type != TB2_MQTT_PACKET_PUBLISH)")
        self.assertLess(complete, ordinary)
        self.assertIn("type, NULL, TRUE, NULL, FALSE, TRUE", processor[ordinary:])

    def test_capture_and_status_expose_packet_decisions(self):
        for field in (
            '"packet_type"',
            '"topic"',
            '"forwarded"',
            '"filter_id"',
            '"generated"',
            '"packet_complete"',
            '"messages_forwarded_box_to_upstream"',
            '"messages_forwarded_upstream_to_box"',
            '"messages_blocked_box_to_upstream"',
            '"messages_blocked_upstream_to_box"',
        ):
            self.assertIn(field, self.passthrough)
        self.assertIn('"incomplete_packet"', self.passthrough)

    def test_observer_only_runs_passive_box_handlers(self):
        table = self.server[
            self.server.index("mqtt_passthrough_observer_handlers") :
            self.server.index("static void mqtt_passthrough_observe_publish")
        ]
        for handler in (
            "handle_mqtt_publish_claim",
            "handle_mqtt_publish_settings_confirm",
            "handle_mqtt_publish_setup_status",
            "handle_mqtt_publish_metrics_battery",
            "handle_mqtt_publish_metrics_events",
            "handle_mqtt_publish_metrics_fleet",
            "handle_mqtt_publish_metrics_headphones",
            "handle_mqtt_publish_playback_state",
            "handle_mqtt_publish_volume_state",
            "handle_mqtt_publish_app_reply_bedtime_state",
        ):
            self.assertIn(handler, table)
        self.assertNotIn("handle_mqtt_publish_settings_request", table)
        observer = self.server[
            self.server.index("static void mqtt_passthrough_observe_publish") :
            self.server.index("void mqtt_server_task")
        ]
        self.assertIn("if (!box_to_upstream", observer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
