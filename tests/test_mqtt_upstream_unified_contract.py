#!/usr/bin/env python3
"""Focused contracts for the unified ICI upstream and certificate mapping."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MqttUpstreamUnifiedContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include" / "settings.h").read_text(
            encoding="utf-8"
        )
        cls.proxy = (ROOT / "src" / "tb2_mqtt_passthrough.c").read_text(
            encoding="utf-8"
        )
        cls.server = (ROOT / "src" / "mqtt_server.c").read_text(encoding="utf-8")

    def test_v22_migrates_only_the_previously_effective_combination(self):
        self.assertIn("#define CONFIG_VERSION 22", self.settings_header)
        migration = self.settings[
            self.settings.index("static void settings_migrate_mqtt_upstream_mode") :
            self.settings.rindex("static bool settings_migrate_id")
        ]
        self.assertIn(
            "settings->mqtt_client_upstream.enabled &&\n"
            "            settings->mqtt_client_upstream.passthrough_enabled",
            migration,
        )
        self.assertIn("settings->mqtt_client_upstream.passthrough_enabled = false", migration)
        self.assertIn("settings->configVersion < 22", self.settings)

        migrated_values = {
            (old_enabled, old_passthrough): old_enabled and old_passthrough
            for old_enabled in (False, True)
            for old_passthrough in (False, True)
        }
        self.assertEqual(
            migrated_values,
            {
                (False, False): False,
                (False, True): False,
                (True, False): False,
                (True, True): True,
            },
        )

    def test_legacy_switch_is_internal_and_rejected_by_setter(self):
        self.assertIn(
            "OPTION_INTERNAL_BOOL(MQTT_UPSTREAM_LEGACY_PASSTHROUGH_SETTING",
            self.settings,
        )
        setter = self.settings[
            self.settings.index("bool settings_set_bool_id") :
            self.settings.index("bool settings_reset_id")
        ]
        self.assertIn("MQTT_UPSTREAM_LEGACY_PASSTHROUGH_SETTING", setter)
        self.assertIn(
            'settingsId > 0 && !osStrcmp(item, "mqtt_client_upstream.enabled")',
            setter,
        )

    def test_runtime_gate_and_status_depend_only_on_enabled(self):
        gate = self.proxy[
            self.proxy.index("bool_t tb2_mqtt_passthrough_is_enabled") :
            self.proxy.index("error_t tb2_mqtt_passthrough_start")
        ]
        self.assertIn("return settings->mqtt_client_upstream.enabled;", gate)
        self.assertNotIn("passthrough_enabled", gate)
        status = self.proxy[self.proxy.index("error_t tb2_mqtt_passthrough_write_status") :]
        self.assertNotIn('state = "standby"', status)
        self.assertIn('state = "ready"', status)
        self.assertIn(
            'cJSON_AddBoolToObject(json, "passthrough_enabled",\n'
            "                         settings->mqtt_client_upstream.enabled)",
            status,
        )

    def test_both_mqtt_paths_share_strict_subject_mapping(self):
        resolver = self.settings[
            self.settings.index("settings_get_existing_tb2_from_certificate_subject") :
            self.settings.index("uint8_t get_overlay_id")
        ]
        self.assertIn("subject_length == 15", resolver)
        self.assertIn("subject_length != 12", resolver)
        self.assertIn("settings_canonicalize_box_id", resolver)
        self.assertIn("settings->internal.config_used", resolver)
        self.assertIn("settings->toniebox.boxGeneration == GENERATION_TB2", resolver)
        self.assertNotIn("issuer", resolver)
        self.assertIn("settings_get_existing_tb2_from_certificate_subject", self.proxy)
        self.assertIn("settings_get_existing_tb2_from_certificate_subject", self.server)

    def test_mapping_failure_does_not_claim_outbound_files_are_missing(self):
        start = self.proxy[
            self.proxy.index("error_t tb2_mqtt_passthrough_start") :
            self.proxy.index("error_t tb2_mqtt_passthrough_forward_initial")
        ]
        mapping_failure = start.index('tb2_mqtt_trace_error("map_box_identity"')
        identity_selection = start.index("tb2_mqtt_select_identity_settings")
        material_log = start.index("identity_material", identity_selection)
        self.assertLess(mapping_failure, identity_selection)
        self.assertGreater(material_log, identity_selection)
        self.assertIn("stage=select_upstream_identity", start)


if __name__ == "__main__":
    unittest.main(verbosity=2)
