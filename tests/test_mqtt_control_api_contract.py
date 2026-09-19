"""Focused API contracts for per-command local MQTT availability.

Runtime policy execution is covered by the native MQTT control test. These
checks protect the API preflight/state-update boundary and compound shutdown.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
API = (ROOT / "src" / "handler_api.c").read_text(encoding="utf-8")


def section(start: str, end: str) -> str:
    begin = API.index(start)
    return API[begin : API.index(end, begin + len(start))]


class MqttControlApiContractTests(unittest.TestCase):
    def test_runtime_boolean_and_reason_share_one_decision(self):
        helper = section("static void api_add_toniebox_control(", "static void api_add_toniebox_runtime(")
        self.assertEqual(helper.count("mqtt_server_control_availability("), 1)
        self.assertIn("availability == MQTT_CONTROL_ALLOWED", helper)
        self.assertIn("mqtt_server_control_reason(availability)", helper)
        self.assertIn("if (reason != NULL)", helper)
        runtime = section("static void api_add_toniebox_runtime(", "error_t handleApiGetBoxes(")
        self.assertIn('cJSON_AddObjectToObject(runtime, "controlReasons")', runtime)
        for name, command in (("playback", "playback"), ("volume", "volume"),
                              ("ping", "ping"), ("bedtime", "stl"), ("sleep", "sleep")):
            self.assertIn(f'api_add_toniebox_control(controls, control_reasons, overlay_id, "{name}", "{command}")', runtime)

    def test_control_endpoints_preflight_actual_command_before_publish(self):
        endpoints = (
            ("Playback", "Volume", "playback", "mqtt_server_publish_playback_position_for_overlay"),
            ("Volume", "Ping", "volume", "mqtt_server_publish_volume_for_overlay"),
            ("Ping", "Bedtime", "ping", "mqtt_server_publish_ping_for_overlay"),
            ("Bedtime", "Sleep", "stl", "mqtt_server_publish_app_control_stl_for_overlay"),
            ("Sleep", "Shutdown", "sleep", "mqtt_server_publish_app_control_sleep_for_overlay"),
        )
        for endpoint, following, command, publisher in endpoints:
            with self.subTest(endpoint=endpoint):
                handler = section(f"error_t handleApiBox{endpoint}(", f"error_t handleApiBox{following}(")
                preflight = f'mqtt_server_control_availability(overlay_id, "{command}") != MQTT_CONTROL_ALLOWED'
                self.assertLess(handler.index(preflight), handler.index(publisher))
                self.assertIn(f'api_box_control_failure(connection, overlay_id, "{command}", FALSE)', handler)
                self.assertIn(f'api_box_control_failure(connection, overlay_id, "{command}", TRUE)', handler)
                self.assertNotIn("mqtt_client_upstream.local_control_enabled", handler)

    def test_failures_distinguish_policy_connectivity_and_write_failure(self):
        failure = section("static error_t api_box_control_failure(", "static bool_t api_get_json_uint32(")
        for reason in ("CLOUD_CONTROLLED", "OFFLINE", "NOT_SUBSCRIBED"):
            self.assertIn(f"case MQTT_CONTROL_{reason}:", failure)
        self.assertIn("Could not publish Toniebox command on the local MQTT connection", failure)
        self.assertIn("api_write_status_response(connection, 409, false", failure)
        volume = section("error_t handleApiBoxVolume(", "error_t handleApiBoxPing(")
        failed_send = volume.index("if (!mqtt_server_publish_volume_for_overlay")
        self.assertIn("Could not publish command: local MQTT connection closed or unavailable", failure)
        self.assertLess(volume.index('return api_box_control_failure(connection, overlay_id, "volume", TRUE);', failed_send),
                        volume.index("tbs_toniebox2_volume_command("))

    def test_shutdown_checks_both_categories_before_first_send(self):
        shutdown = section("error_t handleApiBoxShutdown(", "static error_t loadToniesCustomJsonRoot(")
        first_send = shutdown.index("mqtt_server_publish_app_control_stl_for_overlay(")
        self.assertLess(shutdown.index('mqtt_server_control_availability(overlay_id, "sleep")'), first_send)
        self.assertLess(shutdown.index('!bedtime_active && mqtt_server_control_availability(overlay_id, "stl")'), first_send)
        self.assertIn("!bedtime_active &&\n        !mqtt_server_publish_app_control_stl_for_overlay", shutdown)
        self.assertIn("Bedtime command was sent, but the sleep command could not be published", shutdown)
        self.assertNotIn("Bedtime was activated", shutdown)
        self.assertIn('if (bedtime_active)\n            return api_box_control_failure(connection, overlay_id, "sleep", TRUE);', shutdown)

    def test_control_api_does_not_invoke_settings_or_content_hooks(self):
        handlers = section("error_t handleApiBoxPlayback(", "static error_t loadToniesCustomJsonRoot(")
        for excluded in ("mqtt_settings_", "settings_set_", "freshness_", "source_revision", "nocloud"):
            self.assertNotIn(excluded, handlers)


if __name__ == "__main__":
    unittest.main()
