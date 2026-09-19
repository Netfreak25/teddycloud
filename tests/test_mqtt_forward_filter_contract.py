#!/usr/bin/env python3
"""Contract tests for route-aware manual MQTT forwarding decisions."""

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
    "setup",
    "other",
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
            r'OPTION_BOOL\("mqtt_client_upstream\.forward\.([^\"]+)"[^\n]+?, (TRUE|FALSE),'
        )
        return dict(pattern.findall(self.registration))

    def test_existing_defaults_and_setup_are_true_only_unknown_defaults_false(self):
        expected = TOPIC_OPTIONS | {f"logs.source.{source}" for source in LOG_SOURCES}
        registered = self.registered_forward_options()
        self.assertEqual(set(registered), expected)
        self.assertEqual(len(registered), 65)
        for setting, default in registered.items():
            self.assertEqual(default, "FALSE" if setting == "other" else "TRUE", setting)
        self.assertNotIn("mqtt_client_upstream.block.", self.registration)

    def test_global_master_preserves_manual_rules_and_automatic_protection(self):
        self.assertRegex(
            self.registration,
            r'OPTION_BOOL\("mqtt_client_upstream.filters_enabled",[^\n]+, TRUE,',
        )
        self.assertIn(
            '!osStrcmp(item, "mqtt_client_upstream.filters_enabled")',
            self.registration,
        )
        decision = self.matcher[self.matcher.index("mqtt_forward_filter_result_t mqtt_forward_filter_evaluate"):]
        bypass = decision[decision.index('if (!settings_get_bool("mqtt_client_upstream.filters_enabled"))'):]
        self.assertLess(bypass.index("return result;"), bypass.index("mqtt_filter_effective"))
        self.assertIn("MQTT_FORWARD_REASON_MASTER_BYPASS", bypass)
        self.assertLess(decision.index("mqtt_classify_publish(topic"),
                        decision.index('if (!settings_get_bool("mqtt_client_upstream.filters_enabled"))'))
        self.assertNotIn("settings_set_", decision)
        self.assertNotIn("settings_reset_", decision)
        processor = self.proxy[self.proxy.index("static error_t tb2_mqtt_process_packet"):]
        self.assertLess(processor.index("mqtt_nocloud_filter_publish"),
                        processor.index("mqtt_forward_filter_evaluate"))
        self.assertNotIn("mqtt_client_upstream.filters_enabled", processor)

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
            self.matcher.index("static const char *mqtt_classify_logs") :
            self.matcher.index("static const char *mqtt_classify_group")
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
        for root in ("claim", "volume", "bi-events", "fresh-tonies", "logs", "setup"):
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
            self.matcher.index("mqtt_forward_filter_result_t mqtt_forward_filter_evaluate") :
            self.matcher.index("const char *mqtt_forward_route_name")
        ]
        self.assertIn("mqtt_filter_effective(box_settings, result.setting_id)", decision)
        self.assertIn("allowed ? MQTT_FORWARD_ACTION_FORWARD : MQTT_FORWARD_ACTION_BLOCK", decision)
        self.assertIn("allowed ? MQTT_FORWARD_REASON_RULE_ALLOWED : MQTT_FORWARD_REASON_RULE_BLOCKED", decision)

    def test_local_delivery_does_not_evaluate_internet_filters(self):
        decision = self.matcher[self.matcher.index("mqtt_forward_filter_result_t mqtt_forward_filter_evaluate"):]
        local = decision[decision.index("if (route == MQTT_FORWARD_ROUTE_LOCAL_TO_BOX)"):]
        self.assertLess(local.index("return result;"), local.index("mqtt_classify_publish"))
        self.assertIn(".action = MQTT_FORWARD_ACTION_LOCAL", decision)
        self.assertIn(".setting_id = NULL", decision)

    def test_unknown_topics_have_an_explicit_rule(self):
        classify = self.matcher[
            self.matcher.index("static const char *mqtt_classify_publish") :
            self.matcher.index("mqtt_forward_filter_result_t mqtt_forward_filter_evaluate")
        ]
        self.assertIn('return "mqtt_client_upstream.forward.other";', classify)
        self.assertIn('return setting != NULL ? setting : "mqtt_client_upstream.forward.other";', classify)

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
        packet_start = self.proxy.index("static error_t tb2_mqtt_process_packet", 1000)
        processor = self.proxy[
            packet_start : self.proxy.index(
                "static error_t tb2_mqtt_process_stream", packet_start
            )
        ]
        compact_processor = " ".join(processor.split())
        self.assertIn(
            "mqtt_forward_filter_evaluate( session->box_settings",
            compact_processor,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
