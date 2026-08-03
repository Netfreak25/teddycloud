#!/usr/bin/env python3
"""Contract tests for inverted bidirectional MQTT forwarding settings."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]

LOG_SOURCES = {
    "dev_i2s_mcux",
    "power_domain_gpio",
    "usdhc",
    "iw416_wifi",
    "led_ring",
    "sd",
    "usb_msc",
    "tns_error_handler",
    "tns_dfu",
    "littlefs",
    "tns_fs_storage",
    "tns_signal_broker",
    "production",
    "LIS2DW12",
    "tns_usb_audio",
    "app",
    "iw416",
    "main_sm",
    "tns_pman",
    "idle_timer_log",
    "app_volume_manager",
    "ocotp",
    "wifi_nxp",
    "tns_cloud",
    "tns_storage",
    "tns_wifi_conn",
    "headphones",
    "tns_metrics",
    "bt_nxp_ctlr",
    "bt_hci_core",
    "bt_classic",
    "tns_wifi_settings",
    "tns_time",
    "tns_ota",
    "cloud_settings",
    "cloud_freshness",
    "tns_download_manager",
    "linear_playback",
}

TOPIC_OPTIONS = {
    "claim",
    "volume",
    "bi_events",
    "fresh_tonies",
    "logs.other",
    "metrics.fleet",
    "metrics.events",
    "metrics.headphones",
    "metrics.battery",
    "metrics.other",
    "app_reply.bedtime_state",
    "app_reply.other",
    "settings.desired",
    "settings.confirm",
    "settings.request",
    "settings.other",
    "playback.state",
    "playback.other",
    "app_control.ping",
    "app_control.volume",
    "app_control.stl",
    "app_control.alarm_preview",
    "app_control.playback",
    "app_control.sleep",
    "app_control.other",
}


class MqttForwardFilterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registration = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.matcher = (ROOT / "src/mqtt_forward_filter.c").read_text(
            encoding="utf-8"
        )
        cls.proxy = (ROOT / "src/tb2_mqtt_passthrough.c").read_text(
            encoding="utf-8"
        )

    def registered_forward_options(self):
        pattern = re.compile(
            r'OPTION_BOOL\("mqtt_client_upstream\.forward\.([^\"]+)"[^\n]+?, TRUE,'
        )
        return set(pattern.findall(self.registration))

    def test_every_forward_setting_defaults_to_true(self):
        expected = TOPIC_OPTIONS | {f"logs.source.{source}" for source in LOG_SOURCES}
        self.assertEqual(self.registered_forward_options(), expected)
        self.assertNotIn("mqtt_client_upstream.block.", self.registration)

    def test_all_log_sources_are_matched_exactly(self):
        mappings = set(
            re.findall(
                r'\{"([^\"]+)", "mqtt_client_upstream\.forward\.logs\.source\.[^\"]+"\}',
                self.matcher,
            )
        )
        self.assertEqual(mappings, LOG_SOURCES)
        self.assertIn("cJSON_GetObjectItemCaseSensitive", self.matcher)
        self.assertIn("strcmp(source->valuestring", self.matcher)

    def test_invalid_missing_and_unknown_log_sources_use_other(self):
        logs = self.matcher[
            self.matcher.index("static bool_t mqtt_match_logs") :
            self.matcher.index("static bool_t mqtt_match_group")
        ]
        self.assertLess(
            logs.index('"mqtt_client_upstream.forward.logs.other"'),
            logs.index("cJSON_ParseWithLength"),
        )
        self.assertIn("cJSON_IsString(source)", logs)
        self.assertIn("cJSON_Delete(json)", logs)

    def test_topic_segment_boundaries_and_other_groups_are_explicit(self):
        self.assertIn("path[length] == '\\0' || path[length] == '/'", self.matcher)
        self.assertIn("path[group_length] != '\\0' && path[group_length] != '/'", self.matcher)
        for root in ("claim", "volume", "bi-events", "fresh-tonies", "logs"):
            self.assertIn(f'mqtt_path_is_tree(path, "{root}")', self.matcher)
        for group in ("metrics", "app-reply", "settings", "playback", "app-control"):
            self.assertIn(f'path, "{group}"', self.matcher)
        for setting in (
            "metrics.other",
            "app_reply.other",
            "settings.other",
            "playback.other",
            "app_control.other",
        ):
            self.assertIn(f"mqtt_client_upstream.forward.{setting}", self.matcher)

    def test_false_means_suppress_and_true_means_forward(self):
        decision = self.matcher[
            self.matcher.index("static bool_t mqtt_match_setting") :
            self.matcher.index("static bool_t mqtt_match_logs")
        ]
        self.assertIn("if (mqtt_filter_effective(settings, setting))", decision)
        self.assertIn("return FALSE;", decision)
        self.assertIn("*filter_id = setting", decision)
        self.assertIn("return TRUE;", decision)

    def test_overlay_wins_and_global_is_read_for_every_packet(self):
        effective = self.matcher[
            self.matcher.index("static bool_t mqtt_filter_effective") :
            self.matcher.index("static const char *mqtt_topic_path")
        ]
        self.assertIn("settings_get_by_name_ovl(setting, NULL)", effective)
        self.assertIn("box_settings->internal.overlayUniqueId", effective)
        missing = effective[effective.index("if (global == NULL") :]
        self.assertLess(missing.index("return TRUE;"), missing.index("overlay->overlayed"))
        self.assertIn("overlay->overlayed", effective)
        self.assertLess(effective.index("overlay->overlayed"), effective.rindex("global->ptr"))
        processor = self.proxy[
            self.proxy.index("static error_t tb2_mqtt_process_packet") :
            self.proxy.index("static error_t tb2_mqtt_process_stream")
        ]
        self.assertIn("mqtt_forward_filter_should_block(session->box_settings", processor)


if __name__ == "__main__":
    unittest.main(verbosity=2)
