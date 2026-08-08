#!/usr/bin/env python3
"""Static contract for local TB2 control while the ICI proxy is active."""

from pathlib import Path
import json
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MqttLocalControlContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")
        cls.proxy = (ROOT / "src/tb2_mqtt_passthrough.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.handler_cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.web_handler = (
            ROOT / "teddyCloud_web/src/data/SettingsDataHandler.ts"
        ).read_text(encoding="utf-8")
        cls.layout = json.loads(
            (
                ROOT
                / "teddyCloud_web/src/components/common/form/settingsLayout.json"
            ).read_text(encoding="utf-8")
        )

    def test_setting_defaults_off_and_is_overlay_eligible(self):
        self.assertIn(
            'OPTION_BOOL("mqtt_client_upstream.local_control_enabled"',
            self.settings,
        )
        setting = self.settings[
            self.settings.index(
                'OPTION_BOOL("mqtt_client_upstream.local_control_enabled"'
            ) :
            self.settings.index(
                'OPTION_UNSIGNED("mqtt_client_upstream.port"'
            )
        ]
        self.assertIn("FALSE", setting)
        self.assertIn(
            "mqtt_client_upstream.local_control_enabled",
            self.layout["overlay"]["ids"],
        )

    def test_direct_connections_stay_enabled_and_proxy_uses_effective_setting(self):
        gate = self.server[
            self.server.index("static bool_t mqtt_connection_local_control_allowed") :
            self.server.index("static settings_t *mqtt_connection_certificate_settings")
        ]
        self.assertIn("conn->passthrough == NULL", gate)
        self.assertIn(
            "conn->client_ctx.settings->mqtt_client_upstream.local_control_enabled",
            gate,
        )
        self.assertIn("mqtt_connection_local_control_allowed(conn)", self.server)

    def test_local_settings_and_controls_use_capture_actions(self):
        self.assertIn('"local_settings_desired"', self.server)
        self.assertIn('"local_app_control"', self.server)
        self.assertIn(
            "mqtt_publish_pending_settings_desired_to_connection(conn, TRUE, FALSE);",
            self.server,
        )
        self.assertIn(
            "mqtt_server_publish_fresh_tonies(&conn->client_ctx);", self.server
        )
        self.assertIn("if (conn->passthrough != NULL)", self.server)
        self.assertIn("tb2_mqtt_passthrough_write_local_publish", self.server)

        writer = self.proxy[
            self.proxy.index("error_t tb2_mqtt_passthrough_write_local_publish") :
            self.proxy.index("void tb2_mqtt_passthrough_close")
        ]
        self.assertIn("session, FALSE, packet, packet_size", writer)
        self.assertNotIn("session->upstream.tlsContext", writer)

    def test_cloud_to_box_commands_remain_transparent(self):
        observer = self.server[
            self.server.index("static error_t mqtt_passthrough_observe_publish") :
            self.server.index("static void mqtt_passthrough_observe_control")
        ]
        direction_gate = observer.index("if (!box_to_upstream || conn == NULL)")
        local_handlers = observer.index("mqtt_connection_update_context", direction_gate)
        self.assertLess(direction_gate, local_handlers)
        self.assertNotIn("mqtt_connection_local_control_allowed", observer)

    def test_settings_confirm_is_consumed_or_partially_rewritten(self):
        self.assertIn("mqtt_process_settings_confirm", self.server)
        self.assertIn("cJSON_DeleteItemFromObjectCaseSensitive", self.server)
        self.assertIn("TB2_MQTT_OBSERVER_CONSUME", self.server)
        self.assertIn("TB2_MQTT_OBSERVER_REWRITE", self.server)
        self.assertIn('"local_control.settings_confirm"', self.server)
        self.assertIn('"local_response_rewrite"', self.server)
        self.assertIn('"wire_data_base64"', self.proxy)

    def test_partial_confirm_restarts_retry_window_for_remaining_fields(self):
        acknowledge = self.server[
            self.server.index("static void mqtt_ack_toniebox2_settings_history") :
            self.server.index("static bool_t mqtt_get_json_uint32")
        ]
        self.assertIn("else if (*acked > 0)", acknowledge)
        self.assertIn(
            'settings_set_unsigned_id("internal.toniebox2SettingsDesiredAttempts", 0,',
            acknowledge,
        )
        self.assertIn(
            'settings_set_unsigned_id("internal.toniebox2SettingsDesiredLastAttempt", 0,',
            acknowledge,
        )

    def test_only_correlated_app_replies_are_consumed(self):
        self.assertIn("mqtt_match_app_control_ping", self.server)
        self.assertIn("MQTT_APP_CONTROL_REPLY_WINDOW_SEC", self.server)
        self.assertIn("observer_local_reply_matched = TRUE", self.server)
        self.assertIn('"local_control.app_reply"', self.server)
        self.assertIn("does not match a local pending action", self.server)

    def test_app_control_pending_markers_expire(self):
        self.assertIn(
            "elapsed_ms > MQTT_APP_CONTROL_REPLY_WINDOW_SEC *",
            self.server,
        )
        self.assertIn("last_stl->valid = FALSE;", self.server)

    def test_app_control_accepts_matching_wildcard_subscriptions(self):
        publisher = self.server[
            self.server.index("static bool_t mqtt_server_publish_app_control_for_overlay") :
            self.server.index("bool_t mqtt_server_has_playback_control")
        ]
        self.assertIn("mqtt_connection_has_sub(conn, topic)", publisher)
        self.assertNotIn("mqtt_connection_has_exact_sub", self.server)

    def test_consumed_qos_is_acked_without_blocked_counter(self):
        self.assertIn("local_consume ? \"local_response_puback\"", self.proxy)
        self.assertIn("local_consume ? \"local_response_pubrec\"", self.proxy)
        self.assertIn("qos2_entry->count_blocked", self.proxy)
        consume_capture = self.proxy[
            self.proxy.index("if (!error && local_consume)") :
            self.proxy.index("else if (!error && blocked", self.proxy.index("if (!error && local_consume)"))
        ]
        self.assertIn("packet_id, packet_id, FALSE, FALSE, 0", consume_capture)

    def test_qos_retransmits_replay_the_original_local_decision(self):
        self.assertIn("TB2_MQTT_LOCAL_RESPONSE_HISTORY_MAX 32U", self.proxy)
        process = self.proxy[
            self.proxy.index("static error_t tb2_mqtt_process_packet") :
            self.proxy.index("static error_t tb2_mqtt_process_stream")
        ]
        replay = process.index("tb2_mqtt_replay_local_response")
        observer = process.index("session->observer(session->observer_context")
        self.assertLess(replay, observer)
        self.assertIn('"local_response_replay_consume"', self.proxy)
        self.assertIn('"local_response_replay_rewrite"', self.proxy)
        self.assertIn('"local_response_replay_block"', self.proxy)
        self.assertIn('"local_response_replay_puback"', self.proxy)
        self.assertIn('"local_response_replay_pubrec"', self.proxy)
        self.assertIn('"local_response_state_limit"', self.proxy)
        close = self.proxy[
            self.proxy.index("void tb2_mqtt_passthrough_close") :
            self.proxy.index("error_t tb2_mqtt_passthrough_write_status")
        ]
        self.assertIn("tb2_mqtt_local_responses_free(session);", close)

    def test_web_save_order_and_tb2_dependency_are_explicit(self):
        enabled = self.web_handler.index("localControlChange?.value === true")
        parallel = self.web_handler.index("await Promise.all", enabled)
        disabled = self.web_handler.index("localControlChange?.value === false", parallel)
        self.assertLess(enabled, parallel)
        self.assertLess(parallel, disabled)
        dependency = next(
            item
            for item in self.layout["dependencies"]
            if item["master"] == "mqtt_client_upstream.local_control_enabled"
        )
        self.assertEqual(["toniebox2."], dependency["dependentPrefixes"])

    def test_tb1_audio_id_switches_do_not_affect_tb2(self):
        self.assertIn(
            "settings->toniebox.boxGeneration != GENERATION_TB2",
            self.handler_cloud,
        )
        self.assertIn(
            "use_tb1_audio_id_policy &&\n                                  settings->cloud.updateOnLowerAudioId",
            self.handler_cloud,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
