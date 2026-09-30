#!/usr/bin/env python3
"""Scoped HTTP delivery contracts; no changes to the existing control policy."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MqttControlApiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")

    def handler(self, name, following):
        definition = re.search(r"(?m)^" + re.escape(name) + r"\([^;]*?\n\{", self.source)
        self.assertIsNotNone(definition, name)
        start = definition.start()
        return self.source[start:self.source.index("\n}", start) + 2]

    def test_admission_is_not_sent_or_execution(self):
        helper = self.handler("static error_t api_box_control_delivery", "error_t handleApiNativeLibraryDelete")
        self.assertIn("case MQTT_DELIVERY_SENT:", helper)
        self.assertIn("connection, 200, true, sent_message", helper)
        self.assertIn("case MQTT_DELIVERY_QUEUED:", helper)
        self.assertIn("connection, 202, true", helper)
        self.assertIn("case MQTT_DELIVERY_BUSY:", helper)
        self.assertIn("connection, 503, false", helper)
        self.assertIn("command was not accepted", helper)
        self.assertIn("connection, 409, false, failure_message", helper)
        response = self.handler("static error_t api_write_status_response", "static error_t api_box_control_delivery")
        self.assertIn("if (ok && status_code == 202)", response)
        self.assertIn('cJSON_AddStringToObject(json, "delivery", "queued")', response)

    def test_existing_controls_use_delivery_result(self):
        names = ["Playback", "Volume", "Ping", "Bedtime", "Sleep", "Shutdown"]
        for index, name in enumerate(names):
            end = "error_t handleApiBox" + names[index + 1] if index + 1 < len(names) else "static error_t loadToniesCustomJsonRoot"
            body = self.handler("error_t handleApiBox" + name, end)
            self.assertIn("mqtt_delivery_result_t delivery", body)
            self.assertIn("api_box_control_delivery(", body)
            self.assertNotIn("if (!mqtt_server_publish_", body)

    def test_volume_is_not_changed_at_http_admission(self):
        body = self.handler("error_t handleApiBoxVolume", "error_t handleApiBoxPing")
        self.assertNotIn("tbs_toniebox2_volume_command", body)
        self.assertIn("level >= TBS_TB2_VOLUME_LEVEL_MIN", body)
        self.assertIn("level <= TBS_TB2_VOLUME_LEVEL_MAX", body)

    def test_shutdown_checks_existing_permissions_then_admits_pair(self):
        body = self.handler("error_t handleApiBoxShutdown", "static error_t loadToniesCustomJsonRoot")
        publish = body.index("mqtt_server_publish_shutdown_for_overlay")
        self.assertLess(body.index("mqtt_server_has_sleep_control"), publish)
        self.assertLess(body.index("mqtt_server_has_bedtime_control"), publish)
        self.assertIn("bedtime_active ? NULL : bedtime_payload", body)
        self.assertIn("delivery.status == MQTT_DELIVERY_FAILED && bedtime_sent", body)
        self.assertIn("Bedtime command was sent", body)
        self.assertNotIn("Bedtime was activated", body)


if __name__ == "__main__":
    unittest.main()
