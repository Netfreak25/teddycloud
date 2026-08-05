#!/usr/bin/env python3
"""Static delivery contract for TB2 fresh-tonies messages."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MqttFreshToniesContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.header = (ROOT / "include/mqtt_server.h").read_text(encoding="utf-8")

    def server_function(self, start, end):
        start_index = self.server.rindex(start)
        end_index = self.server.find(end, start_index + len(start))
        if end_index < 0:
            end_index = len(self.server)
        return self.server[start_index:end_index]

    def test_payload_is_one_json_object_per_uid(self):
        builder = self.server_function(
            "static char *mqtt_build_fresh_tonie_payload",
            "static bool_t mqtt_send_fresh_tonie",
        )
        self.assertIn(r'{\"tonie\":\"%s\"}', builder)
        self.assertNotIn("cJSON_CreateArray", builder)
        self.assertNotIn("#define MQTT_FRESH_TONIES_MAX 50", self.server)

    def test_queue_preserves_cache_order_and_deduplicates(self):
        sync = self.server_function(
            "static bool_t mqtt_fresh_tonies_sync_connection",
            "static MqttFreshTonieEntry *mqtt_fresh_tonies_next",
        )
        self.assertIn("for (size_t index = 0; index < cache_len; index++)", sync)
        self.assertIn("seen->uid == cache[index]", sync)
        self.assertIn("*tail = entry", sync)
        self.assertIn("entry->present = TRUE", sync)

    def test_initial_publish_and_retries_use_qos_one(self):
        sender = self.server_function(
            "static bool_t mqtt_send_fresh_tonie",
            "static bool_t mqtt_fresh_tonies_pump",
        )
        self.assertIn("conn, topic, payload, 1, duplicate, &packet_id", sender)
        self.assertIn(
            "uint16_t packet_id = duplicate ? conn->fresh_tonie_packet_id : 0",
            sender,
        )

        encoder = self.server_function(
            "static bool_t mqtt_build_publish_packet",
            "static bool_t mqtt_connection_publish_packet",
        )
        self.assertIn("(qos << 1)", encoder)
        self.assertIn("duplicate ? 0x08U : 0", encoder)
        self.assertIn("packet_id >> 8", encoder)

    def test_retry_schedule_is_three_attempts_five_seconds_apart(self):
        self.assertIn("#define MQTT_FRESH_TONIES_RETRY_INTERVAL_SEC 5", self.server)
        self.assertIn("#define MQTT_FRESH_TONIES_MAX_ATTEMPTS 3", self.server)
        pump = self.server_function(
            "static bool_t mqtt_fresh_tonies_pump",
            "static bool_t mqtt_handle_fresh_tonies_puback",
        )
        self.assertIn("conn->fresh_tonie_attempts >= MQTT_FRESH_TONIES_MAX_ATTEMPTS", pump)
        self.assertIn("mqtt_send_fresh_tonie(conn, conn->fresh_tonie_inflight, TRUE)", pump)
        self.assertIn('mqtt_connection_close(conn, "fresh-tonies PUBACK timeout")', pump)

    def test_matching_puback_advances_without_clearing_persistent_cache(self):
        ack = self.server_function(
            "static bool_t mqtt_handle_fresh_tonies_puback",
            "bool_t mqtt_server_publish_fresh_tonies",
        )
        self.assertIn("packet_id != conn->fresh_tonie_packet_id", ack)
        self.assertIn("conn->fresh_tonie_inflight->delivered = TRUE", ack)
        self.assertIn("conn->fresh_tonie_inflight = NULL", ack)
        self.assertNotIn("settings_set_bool_id", ack)

    def test_targeted_content_change_requeues_only_its_uid(self):
        targeted = self.server_function(
            "bool_t mqtt_server_publish_fresh_tonie_for_overlay",
            "static bool_t mqtt_app_control_topic",
        )
        self.assertIn("mqtt_fresh_tonie_find(conn, uid)", targeted)
        self.assertIn("entry->delivered = FALSE", targeted)
        self.assertIn("entry != conn->fresh_tonie_inflight", targeted)
        self.assertIn("mqtt_server_publish_fresh_tonie_for_overlay", self.header)
        self.assertIn(
            "mqtt_server_publish_fresh_tonie_for_overlay(settings->internal.overlayNumber, uid)",
            self.cloud,
        )

    def test_proxy_path_runs_pump_before_its_early_continue(self):
        start = self.server.index("if (conn->passthrough != NULL)", self.server.index("void mqtt_server_task"))
        end = self.server.index("// Non-blocking check for data", start)
        proxy_path = self.server[start:end]
        self.assertIn("tb2_mqtt_passthrough_task", proxy_path)
        self.assertIn("mqtt_fresh_tonies_pump(conn)", proxy_path)
        self.assertLess(
            proxy_path.index("mqtt_fresh_tonies_pump(conn)"),
            proxy_path.index("continue;"),
        )

    def test_proxy_observer_tracks_subscribe_unsubscribe_and_local_puback(self):
        observer = self.server_function(
            "static void mqtt_passthrough_observe_control",
            "void mqtt_server_task",
        )
        self.assertIn("TB2_MQTT_CONTROL_LOCAL_PUBACK", observer)
        self.assertIn("mqtt_handle_fresh_tonies_puback", observer)
        self.assertIn("TB2_MQTT_CONTROL_UNSUBSCRIBE", observer)
        self.assertIn("mqtt_apply_subscription_packet", observer)
        self.assertNotIn("suback", observer.lower())

    def test_active_playback_never_holds_freshness(self):
        freshness = self.server_function(
            "static char *mqtt_build_fresh_tonie_payload",
            "static bool_t mqtt_handle_fresh_tonies_puback",
        )
        self.assertNotIn("active_playback", freshness)
        self.assertNotIn("held", freshness.lower())

    def test_outbound_topics_use_the_observed_connection_topic_id(self):
        identity = self.server_function(
            "static bool_t mqtt_connection_topic_id",
            "static bool_t mqtt_is_toniebox2_overlay",
        )
        self.assertIn("conn->box_topic_id", identity)
        self.assertIn("settings_canonicalize_box_id", identity)
        self.assertIn("mqtt_toniebox2_settings_desired_topic(MqttClientConnection *conn", identity)
        self.assertIn("mqtt_app_control_topic(MqttClientConnection *conn", identity)

        fresh_topic = self.server_function(
            "static bool_t mqtt_fresh_tonies_topic",
            "static MqttFreshToniesPublishState *mqtt_fresh_tonies_publish_state",
        )
        self.assertIn("mqtt_connection_topic_id(conn", fresh_topic)
        self.assertNotIn("settings->commonName", fresh_topic)

    def test_topic_identity_is_learned_from_concrete_box_topics(self):
        updater = self.server_function(
            "static void mqtt_connection_update_context(",
            "static char_t *mqtt_server_cert",
        )
        self.assertIn("mac_len == 12", updater)
        self.assertIn("settings_canonicalize_box_id", updater)
        self.assertIn("conn->box_topic_id", updater)

    def test_source_change_targets_selected_tb2_overlay_without_inventory(self):
        freshness = self.cloud[
            self.cloud.index("static bool_t freshness_mark_content_mapping_changed_for_overlay") :
            self.cloud.index("error_t handleCloudFreshnessCheck(")
        ]
        self.assertIn("settings->toniebox.boxGeneration != GENERATION_TB2", freshness)
        self.assertIn("require_inventory || !source_changed", freshness)
        self.assertIn("source_changed, FALSE", freshness)
        self.assertIn("source_changed, TRUE", freshness)
        self.assertIn("inventory_available ? \"present\" : \"absent\"", freshness)
        self.assertIn(
            "freshness_mark_content_mapping_changed(client_ctx->settings, ruid, source_changed)",
            self.api,
        )

    def test_followup_metadata_change_preserves_source_change_marker(self):
        helper = self.cloud[
            self.cloud.index("static bool_t freshness_mark_content_mapping_changed_for_overlay") :
            self.cloud.index("void freshness_mark_content_mapping_changed(")
        ]
        self.assertIn("freshness_cache_add_source_changed_uid", helper)
        self.assertNotIn("freshness_cache_remove_source_changed_uid", helper)

    def test_missing_subscription_is_logged_once_per_pending_series(self):
        pump = self.server_function(
            "static bool_t mqtt_fresh_tonies_pump",
            "static bool_t mqtt_handle_fresh_tonies_puback",
        )
        self.assertIn("waiting_for_subscription_logged", pump)
        self.assertIn("MQTT fresh-tonies waiting", pump)
        clear = self.server_function(
            "static void mqtt_clear_fresh_tonies_pending",
            "static MqttFreshTonieEntry *mqtt_fresh_tonie_find",
        )
        self.assertIn("waiting_for_subscription_logged = FALSE", clear)


if __name__ == "__main__":
    unittest.main(verbosity=2)
