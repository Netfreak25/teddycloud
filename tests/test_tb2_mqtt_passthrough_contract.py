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
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")

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
        self.assertIn("TB2_MQTT_PACKET_PUBCOMP", processor)

    def test_qos_two_duplicate_pubrel_remains_idempotent(self):
        state = self.function(
            "static tb2_mqtt_qos2_entry_t *tb2_mqtt_qos2_find",
            "static void tb2_mqtt_qos2_remove",
        )
        self.assertIn("existing->completed = FALSE", state)
        self.assertIn("entry->completed = TRUE", state)
        self.assertNotIn("osFreeMem", state)
        self.assertIn("blocked_qos2_box", self.passthrough)
        self.assertIn("blocked_qos2_upstream", self.passthrough)

    def test_new_qos_two_publish_clears_reused_packet_id_state(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        self.assertIn("if (!duplicate && qos == 2)", processor)
        self.assertIn("tb2_mqtt_qos2_remove(list, stale);", processor)

    def test_unknown_pubrel_is_forwarded_unchanged(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        complete = processor.index("tb2_mqtt_qos2_complete")
        ordinary = processor.index("if (type != TB2_MQTT_PACKET_PUBLISH)")
        self.assertLess(complete, ordinary)
        ordinary_flow = " ".join(processor[ordinary:].split())
        self.assertIn(
            "packet_size, type, NULL, TRUE, NULL, FALSE, TRUE",
            ordinary_flow,
        )

    def test_capture_and_status_expose_packet_decisions(self):
        for field in (
            '"wire_data_base64"',
            '"packet_type"',
            '"topic"',
            '"forwarded"',
            '"filter_id"',
            '"generated"',
            '"packet_complete"',
            '"packet_id"',
            '"wire_packet_id"',
            '"action"',
            '"removed_count"',
            '"messages_forwarded_box_to_upstream"',
            '"messages_forwarded_upstream_to_box"',
            '"messages_blocked_box_to_upstream"',
            '"messages_blocked_upstream_to_box"',
            '"messages_rewritten_box_to_upstream"',
            '"messages_rewritten_upstream_to_box"',
            '"nocloud_items_removed_box_to_upstream"',
            '"nocloud_items_removed_upstream_to_box"',
        ):
            self.assertIn(field, self.passthrough)
        self.assertIn('"incomplete_packet"', self.passthrough)

    def test_automatic_filter_runs_after_observer_and_before_manual_filter(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        observer = processor.index("session->observer(")
        manual = processor.index("mqtt_forward_filter_should_block")
        automatic = processor.index("mqtt_nocloud_filter_publish")
        record = processor.index("tb2_mqtt_record_packet_ex", automatic)
        self.assertLess(observer, automatic)
        self.assertLess(automatic, manual)
        self.assertLess(automatic, record)
        self.assertIn("topic, effective_payload", processor[automatic:manual])
        self.assertNotIn("!local_rewrite", processor[observer:automatic])

    def test_capture_marks_local_processing_before_security_decision(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        for action in (
            '"local_status_forward"',
            '"local_status_manual_block"',
            '"local_status_nocloud_block"',
            '"local_status_nocloud_rewrite"',
            '"local_status_local_content_block"',
        ):
            self.assertIn(action, processor)
        self.assertIn("observer_result.locally_processed", processor)

    def test_nocloud_rewrite_preserves_mqtt_publish_header(self):
        rebuilder = self.function(
            "static error_t tb2_mqtt_rebuild_publish",
            "static error_t tb2_mqtt_process_mapped_control",
        )
        self.assertIn("output[position++] = packet[0]", rebuilder)
        self.assertIn("packet + fixed_header_size", rebuilder)
        self.assertIn("packet_id_offset - fixed_header_size", rebuilder)
        self.assertIn("replacement_payload", rebuilder)

        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        self.assertIn("forwarded_packet_id_offset", processor)
        self.assertIn("mapping->wire_id, &wire_packet", processor)
        self.assertIn('"nocloud_rewrite"', processor)
        self.assertIn('"nocloud_block"', processor)
        self.assertIn("MQTT_NOCLOUD_BLOCK", processor)
        self.assertIn("MQTT_NOCLOUD_REWRITE", processor)

    def test_proxy_observes_subscriptions_without_generating_acks(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        self.assertIn("TB2_MQTT_PACKET_SUBSCRIBE", processor)
        self.assertIn("TB2_MQTT_PACKET_UNSUBSCRIBE", processor)
        self.assertIn("TB2_MQTT_CONTROL_SUBSCRIBE", processor)
        self.assertIn("TB2_MQTT_CONTROL_UNSUBSCRIBE", processor)
        self.assertNotIn("TB2_MQTT_PACKET_SUBACK", processor)
        self.assertNotIn("TB2_MQTT_PACKET_UNSUBACK", processor)

    def test_local_freshness_puback_is_consumed_without_blocked_count(self):
        controls = self.function(
            "static error_t tb2_mqtt_process_mapped_control",
            "static error_t tb2_mqtt_process_packet",
        )
        self.assertIn("if (entry->local)", controls)
        self.assertIn("if (type != TB2_MQTT_PACKET_PUBACK)", controls)
        self.assertIn('"local_freshness_puback"', controls)
        self.assertIn("TB2_MQTT_CONTROL_LOCAL_PUBACK", controls)
        self.assertIn("packet_id, FALSE", controls)

    def test_cloud_packet_ids_are_remapped_around_local_ids(self):
        processor = self.function(
            "static error_t tb2_mqtt_process_packet",
            "static error_t tb2_mqtt_process_stream",
        )
        self.assertIn("tb2_mqtt_packet_id_find_upstream", processor)
        self.assertIn("tb2_mqtt_packet_id_find_wire", processor)
        self.assertIn("tb2_mqtt_allocate_wire_packet_id", processor)
        self.assertIn("tb2_mqtt_rewrite_packet_id", processor)
        self.assertIn('"packet_id_remap"', processor)

        controls = self.function(
            "static error_t tb2_mqtt_process_mapped_control",
            "static error_t tb2_mqtt_process_packet",
        )
        for packet_type in (
            "TB2_MQTT_PACKET_PUBACK",
            "TB2_MQTT_PACKET_PUBREC",
            "TB2_MQTT_PACKET_PUBREL",
            "TB2_MQTT_PACKET_PUBCOMP",
        ):
            self.assertIn(packet_type, controls)
        self.assertIn("entry->original_id", controls)
        self.assertIn("entry->wire_id", controls)

    def test_observer_only_runs_passive_box_handlers(self):
        table = self.server[
            self.server.index("mqtt_passthrough_observer_handlers") :
            self.server.index("static error_t mqtt_passthrough_observe_publish")
        ]
        for handler in (
            "handle_mqtt_publish_claim",
            "handle_mqtt_publish_logs",
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
            self.server.index("static error_t mqtt_passthrough_observe_publish") :
            self.server.index("void mqtt_server_task")
        ]
        self.assertIn("if (!box_to_upstream", observer)

    def test_outbound_tls_uses_only_explicit_tb2_identity(self):
        tls_init = self.passthrough[
            self.passthrough.index("static bool_t tb2_mqtt_has_original_identity") :
            self.passthrough.index("static error_t tb2_mqtt_connect_upstream")
        ]
        self.assertIn("settings->internal.client_tb2", tls_init)
        self.assertNotIn("settings->internal.client_tb1", tls_init)
        self.assertNotIn("settings->internal.client.", tls_init)

    def test_box_certificate_uses_canonical_cn_overlay_mapping(self):
        mapping = self.passthrough[
            self.passthrough.index("static settings_t *tb2_mqtt_settings_from_certificate") :
            self.passthrough.index("static bool_t tb2_mqtt_has_original_identity")
        ]
        self.assertIn("get_settings_cn(common_name)", mapping)
        self.assertNotIn("get_overlay_id(common_name)", mapping)
        self.assertIn("stage=box_client_auth certificate_present=true", mapping)

    def test_certificate_and_topic_box_ids_are_case_insensitive(self):
        self.assertNotIn(
            "osStrcmp(conn->box_common_name", self.server
        )
        self.assertIn(
            "osStrcasecmp(conn->box_common_name, logical_common_name)", self.server
        )
        self.assertIn(
            "osStrcasecmp(conn->box_common_name, topic_common_name)", self.server
        )
        self.assertIn(
            "osStrcasecmp(Settings_Overlay[i].commonName, commonName)", self.settings
        )
        self.assertIn(
            "osStrcasecmp(Settings_Overlay[i].internal.overlayUniqueId, overlay_unique_id)",
            self.settings,
        )
        self.assertIn(
            "osStrcasecmp(settings->commonName, common_name)", self.passthrough
        )

    def test_new_box_ids_are_uppercase_but_certificate_paths_stay_lowercase(self):
        canonicalizer = self.settings[
            self.settings.index("bool_t settings_canonicalize_box_id") :
            self.settings.index("bool settings_set_by_string")
        ]
        self.assertIn("osStrlen(input_id) != 12", canonicalizer)
        self.assertIn("isxdigit", canonicalizer)
        self.assertIn("toupper", canonicalizer)

        overlay_creation = self.settings[
            self.settings.index("settings_t *get_settings_cn") :
            self.settings.index("uint8_t get_overlay_id")
        ]
        self.assertIn("settings_canonicalize_box_id", overlay_creation)
        self.assertIn('settings_set_string_id("commonName", boxId', overlay_creation)
        self.assertIn('settings_set_string_id("internal.overlayUniqueId", boxId', overlay_creation)
        self.assertIn("osStringToLower(boxId)", overlay_creation)

        certificate_mapping = self.passthrough[
            self.passthrough.index("static settings_t *tb2_mqtt_settings_from_certificate") :
            self.passthrough.index("static bool_t tb2_mqtt_has_original_identity")
        ]
        self.assertIn("settings_canonicalize_box_id", certificate_mapping)
        self.assertNotIn("osStringToLower", certificate_mapping)

    def test_global_tb2_identity_is_default_until_overlay_override(self):
        selector = self.passthrough[
            self.passthrough.index("static bool_t tb2_mqtt_overlay_has_identity_override") :
            self.passthrough.index("static error_t tb2_mqtt_outbound_tls_init")
        ]
        self.assertIn("core.client_cert_tb2.file.ca", selector)
        self.assertIn("core.client_cert_tb2.data.key", selector)
        self.assertIn("option->overlayed", selector)
        self.assertIn("overlay_override ? box_settings : get_settings()", selector)
        self.assertIn("source=%s overlay_override=%s", selector)
        self.assertIn("tb2_mqtt_connect_upstream(identity_settings", self.passthrough)

    def test_new_overlay_keeps_tb2_client_identity_inherited(self):
        self.assertIn(
            "settings_set_overlay_client_cert_paths(i, customCertDir, FALSE)",
            self.settings,
        )
        self.assertIn(
            "settings_set_overlay_client_cert_paths(settingsId, settings->core.certdir, TRUE)",
            self.settings,
        )

    def test_upstream_client_auth_reports_request_and_actual_response(self):
        self.assertIn("tls_context->clientCertRequested", self.passthrough)
        self.assertIn("tls_context->cert != NULL", self.passthrough)
        self.assertIn('"certificate_sent"', self.passthrough)
        self.assertIn('"empty_certificate"', self.passthrough)
        self.assertIn('"not_sent"', self.passthrough)


if __name__ == "__main__":
    unittest.main(verbosity=2)
