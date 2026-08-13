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
        cls.handler_api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.state = (ROOT / "src/toniebox_state.c").read_text(encoding="utf-8")
        cls.state_types = (ROOT / "include/toniebox_state_type.h").read_text(
            encoding="utf-8"
        )
        cls.routes = (ROOT / "src/server.c").read_text(encoding="utf-8")
        cls.web_api = (
            ROOT / "teddycloud_web/src/api/apis/TeddyCloudApi.ts"
        ).read_text(encoding="utf-8")
        cls.web_controls = (
            ROOT
            / "teddycloud_web/src/components/tonieboxes/tonieboxcard/live/TonieboxLiveControls.tsx"
        ).read_text(encoding="utf-8")
        cls.web_types = (
            ROOT / "teddycloud_web/src/types/tonieboxTypes.ts"
        ).read_text(encoding="utf-8")
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

    def test_bedtime_and_sleep_use_the_existing_local_control_gate(self):
        self.assertIn(
            'mqtt_server_has_app_control_subscription(overlay_id, "stl")',
            self.server,
        )
        self.assertIn(
            'mqtt_server_has_app_control_subscription(overlay_id, "sleep")',
            self.server,
        )
        self.assertIn(
            'mqtt_server_publish_app_control_for_overlay(overlay_id, "sleep", "{}", FALSE)',
            self.server,
        )
        self.assertIn(
            'cJSON_AddBoolToObject(controls, "bedtime", mqtt_server_has_bedtime_control(overlay_id))',
            self.handler_api,
        )
        self.assertIn(
            'cJSON_AddBoolToObject(controls, "sleep", mqtt_server_has_sleep_control(overlay_id))',
            self.handler_api,
        )

    def test_tb2_volume_uses_the_confirmed_discrete_level_range(self):
        self.assertIn("#define TBS_TB2_VOLUME_LEVEL_MIN 1", self.state_types)
        self.assertIn("#define TBS_TB2_VOLUME_LEVEL_MAX 12", self.state_types)

        api = self.handler_api[
            self.handler_api.index("error_t handleApiBoxVolume") :
            self.handler_api.index("error_t handleApiBoxPing")
        ]
        self.assertIn("level >= TBS_TB2_VOLUME_LEVEL_MIN", api)
        self.assertIn("level <= TBS_TB2_VOLUME_LEVEL_MAX", api)
        self.assertIn("integer between 1 and 12", api)

        incoming = self.server[
            self.server.index("static error_t handle_mqtt_publish_volume_state") :
            self.server.index("static bool_t mqtt_match_app_control_ping")
        ]
        self.assertIn('mqtt_get_json_uint32_exact(json, "level", &level)', incoming)
        self.assertIn("level < TBS_TB2_VOLUME_LEVEL_MIN", incoming)
        self.assertIn("level > TBS_TB2_VOLUME_LEVEL_MAX", incoming)

        publisher = self.server[
            self.server.index("bool_t mqtt_server_publish_volume_for_overlay") :
            self.server.index("bool_t mqtt_server_publish_ping_for_overlay")
        ]
        self.assertIn("level < TBS_TB2_VOLUME_LEVEL_MIN", publisher)
        self.assertIn("level > TBS_TB2_VOLUME_LEVEL_MAX", publisher)
        self.assertIn('"{\\"level\\":%" PRIu32 "}"', publisher)

        runtime = self.state[
            self.state.index("void tbs_toniebox2_volume_state") :
            self.state.index("void tbs_toniebox2_pong")
        ]
        self.assertIn("level < TBS_TB2_VOLUME_LEVEL_MIN", runtime)
        self.assertIn("level > TBS_TB2_VOLUME_LEVEL_MAX", runtime)
        self.assertIn('mqtt_sendBoxEvent("VolumeLevel", value, client_ctx)', runtime)

        self.assertIn("const VOLUME_MIN = 1", self.web_controls)
        self.assertIn("const VOLUME_MAX = 12", self.web_controls)

    def test_tb2_volume_restores_persistent_state_and_uses_two_only_as_fallback(self):
        self.assertIn("#define TBS_TB2_VOLUME_FALLBACK_LEVEL 2U", self.state)
        self.assertIn('TBS_TB2_VOLUME_STATE_DIRECTORY "runtime"', self.state)
        self.assertIn('TBS_TB2_VOLUME_STATE_BOX_DIRECTORY "toniebox-state"', self.state)
        self.assertIn("settings_canonicalize_box_id", self.state)
        self.assertIn("fsFlushFile(file)", self.state)
        self.assertIn("fsMoveFile(temporary, path, TRUE)", self.state)
        writer = self.state[
            self.state.index("static error_t tbs_volume_state_write") :
            self.state.index("static void tbs_volume_update")
        ]
        self.assertIn('cJSON_AddStringToObject(json, "source", source)', writer)
        self.assertNotIn("TBS_TB2_VOLUME_SOURCE_FALLBACK", writer)

        initialization = self.state[
            self.state.index("void toniebox_state_init") :
            self.state.index("toniebox_state_t *get_toniebox_state")
        ]
        self.assertIn("TBS_TB2_VOLUME_FALLBACK_LEVEL", initialization)
        self.assertIn("TBS_TB2_VOLUME_SOURCE_FALLBACK", initialization)
        self.assertIn("TBS_TB2_VOLUME_SOURCE_PERSISTED", initialization)

        runtime_api = self.handler_api[
            self.handler_api.index('cJSON *volume = cJSON_AddObjectToObject(runtime, "volume")') - 160 :
            self.handler_api.index('cJSON *battery = cJSON_AddObjectToObject(runtime, "battery")')
        ]
        self.assertIn('cJSON_AddNumberToObject(volume, "level", volume_state.level)', runtime_api)
        self.assertIn('cJSON_AddStringToObject(volume, "source"', runtime_api)

        command = self.handler_api[
            self.handler_api.index("error_t handleApiBoxVolume") :
            self.handler_api.index("error_t handleApiBoxPing")
        ]
        publish_position = command.index("mqtt_server_publish_volume_for_overlay")
        record_position = command.index("tbs_toniebox2_volume_command")
        self.assertLess(publish_position, record_position)
        self.assertIn("previous_volume.revision", command)
        self.assertIn("volume->revision != expected_revision", self.state)

        self.assertIn("const VOLUME_FALLBACK = 2", self.web_controls)
        volume_gate = self.web_controls[
            self.web_controls.index("const volumeEnabled =") :
            self.web_controls.index("const bedtimeState =")
        ]
        self.assertNotIn("runtime.volume.valid", volume_gate)
        self.assertNotIn("runtime.volume.level !== null", volume_gate)

    def test_bedtime_http_wrapper_validates_confirmed_payload(self):
        bedtime = self.handler_api[
            self.handler_api.index("error_t handleApiBoxBedtime") :
            self.handler_api.index("error_t handleApiBoxSleep")
        ]
        self.assertIn("API_TB2_BEDTIME_DURATION_MIN 300U", self.handler_api)
        self.assertIn("API_TB2_BEDTIME_DURATION_MAX 86400U", self.handler_api)
        self.assertIn('osStrcmp(state->valuestring, "on")', bedtime)
        self.assertIn('osStrcmp(state->valuestring, "off")', bedtime)
        self.assertIn('"oneTimeAlarm"', bedtime)
        self.assertIn('"tone"', bedtime)
        self.assertIn('"volume"', bedtime)
        self.assertIn('"morningLight"', bedtime)
        self.assertIn("mqtt_server_publish_app_control_stl_for_overlay", bedtime)
        self.assertNotIn("501", bedtime)

    def test_sleep_requires_empty_body_and_active_bedtime(self):
        sleep_start = self.handler_api.index("error_t handleApiBoxSleep")
        sleep = self.handler_api[
            sleep_start :
            self.handler_api.index("static error_t loadToniesCustomJsonRoot", sleep_start)
        ]
        self.assertIn("json->child != NULL", sleep)
        self.assertIn('osStrcasecmp(state->bedtime.state, "on")', sleep)
        self.assertIn('osStrcasecmp(state->bedtime.state, "active")', sleep)
        self.assertIn("mqtt_server_publish_app_control_sleep_for_overlay", sleep)
        self.assertIn('{REQ_POST, "/api/box/sleep"', self.routes)

    def test_shutdown_activates_minimum_bedtime_before_sleep_when_needed(self):
        shutdown_start = self.handler_api.index("error_t handleApiBoxShutdown")
        shutdown = self.handler_api[
            shutdown_start : self.handler_api.index("static error_t loadToniesCustomJsonRoot", shutdown_start)
        ]

        self.assertIn("json->child != NULL", shutdown)
        self.assertIn("mqtt_server_has_sleep_control", shutdown)
        self.assertIn("mqtt_server_has_bedtime_control", shutdown)
        self.assertIn("API_TB2_BEDTIME_DURATION_MIN", shutdown)
        self.assertIn('{\\"state\\":\\"on\\",\\"duration\\":%u}', shutdown)
        self.assertIn("mqtt_server_publish_app_control_sleep_for_overlay", shutdown)
        self.assertIn('{REQ_POST, "/api/box/shutdown"', self.routes)

    def test_web_moon_control_exposes_bedtime_alarm_and_sleep(self):
        self.assertIn("TonieboxBedtimeCommand", self.web_types)
        self.assertIn("sleep: boolean", self.web_types)
        self.assertIn("apiControlTonieboxBedtime", self.web_api)
        self.assertIn("apiSleepToniebox", self.web_api)
        self.assertIn("apiShutdownToniebox", self.web_api)
        self.assertIn("onClick={openBedtimeControls}", self.web_controls)
        self.assertIn("BEDTIME_MINUTES_MIN = 5", self.web_controls)
        self.assertIn("BEDTIME_MINUTES_MAX = 24 * 60", self.web_controls)
        self.assertIn("oneTimeAlarm", self.web_controls)
        self.assertIn("runtime.controls.sleep", self.web_controls)
        self.assertIn('commandInFlight === "shutdown"', self.web_controls)
        self.assertIn("Math.ceil(runtime.bedtime.duration / 60)", self.web_controls)


if __name__ == "__main__":
    unittest.main(verbosity=2)
