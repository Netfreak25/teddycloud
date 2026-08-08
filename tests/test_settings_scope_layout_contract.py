import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAYOUT_PATH = (
    ROOT
    / "teddycloud_web"
    / "src"
    / "components"
    / "common"
    / "form"
    / "settingsLayout.json"
)
SETTINGS_PATH = ROOT / "src" / "settings.c"
FORM_COMPONENTS_PATH = (
    ROOT / "teddycloud_web" / "src" / "components" / "common" / "form"
)
SETTINGS_HANDLER_PATH = ROOT / "teddycloud_web" / "src" / "data" / "SettingsDataHandler.ts"

PUBLIC_OPTION_PATTERN = re.compile(
    r"\bOPTION_(?:BOOL|STRING|UNSIGNED|SIGNED|FLOAT|READONLY_BOOL|READONLY_STRING)"
    r"\(\s*(?:\"([^\"]+)\"|([A-Z][A-Z0-9_]+))"
)
DEFINE_PATTERN = re.compile(
    r'^\s*#define\s+([A-Z][A-Z0-9_]+)\s+"([^"]+)"', re.MULTILINE
)


def load_public_setting_ids() -> set[str]:
    source = SETTINGS_PATH.read_text(encoding="utf-8")
    source_without_line_comments = "\n".join(
        line.split("//", 1)[0] for line in source.splitlines()
    )
    constants = dict(DEFINE_PATTERN.findall(source_without_line_comments))
    option_ids: set[str] = set()

    for literal_id, constant_name in PUBLIC_OPTION_PATTERN.findall(source_without_line_comments):
        option_id = literal_id or constants.get(constant_name)
        if option_id is None:
            raise AssertionError(f"Unresolved settings identifier constant: {constant_name}")
        option_ids.add(option_id)

    return option_ids


class SettingsScopeLayoutContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.layout = json.loads(LAYOUT_PATH.read_text(encoding="utf-8"))
        cls.public_setting_ids = load_public_setting_ids()

    @classmethod
    def matching_sections(cls, option_id: str) -> list[dict]:
        return [
            section
            for section in cls.layout["sections"]
            if option_id in section.get("ids", [])
            or any(option_id.startswith(prefix) for prefix in section.get("prefixes", []))
        ]

    @classmethod
    def scope_of(cls, option_id: str) -> str:
        matches = cls.matching_sections(option_id)
        if len(matches) != 1:
            raise AssertionError(
                f"Expected one section for {option_id}, got "
                f"{[section['id'] for section in matches]}"
            )
        return matches[0]["scope"]

    @classmethod
    def overlay_eligible(cls, option_id: str) -> bool:
        overlay = cls.layout["overlay"]
        return option_id in overlay["ids"] or any(
            option_id.startswith(prefix) for prefix in overlay["prefixes"]
        )

    @classmethod
    def visible_in_overlay(cls, option_id: str, generation: str | None) -> bool:
        if not cls.overlay_eligible(option_id):
            return False
        scope = cls.scope_of(option_id)
        return scope == "global" or (generation is not None and scope == generation)

    def test_every_public_repository_setting_has_exactly_one_section(self) -> None:
        failures = {
            option_id: [section["id"] for section in self.matching_sections(option_id)]
            for option_id in sorted(self.public_setting_ids)
            if len(self.matching_sections(option_id)) != 1
        }
        self.assertEqual({}, failures)

    def test_expected_generation_assignments(self) -> None:
        expectations = {
            "mqtt.enabled": "global",
            "hass.name": "global",
            "rtnl.logRaw": "tb1",
            "core.certdir": "tb1",
            "core.client_cert_tb1.file.crt": "tb1",
            "core.server_cert.file.crt": "tb1",
            "cloud.enabled": "tb1",
            "cloud.enableV2Content": "tb1",
            "toniebox.overrideCloud": "tb1",
            "core.certdir_tb2": "tb2",
            "core.client_cert_tb2.file.crt": "tb2",
            "core.server_cert_tb2.file.crt": "tb2",
            "core.server_cert_tb2.hostname": "tb2",
            "core.server_cert_tb2.rotation_status": "tb2",
            "cloud.tb2_enabled": "tb2",
            "cloud.tb2_v3_enabled": "tb2",
            "mqtt_server.enabled": "tb2",
            "mqtt_server.hostname": "tb2",
            "mqtt_server.cert.rotation_status": "tb2",
            "mqtt_client_upstream.enabled": "tb2",
            "mqtt_client_upstream.local_control_enabled": "tb2",
            "mqtt_client_upstream.forward.logs.other": "tb2",
            "toniebox2.max_volume": "tb2",
            "toniebox.api_access": "global",
            "toniebox.boxGeneration": "global",
            "cloud.cacheOta": "global",
            "cloud.localOta": "global",
            "cloud.markCustomTagByPass": "global",
            "cloud.markCustomTagByUid": "global",
            "cloud.dumpRuidAuthContentJson": "global",
            "cloud.cacheContent": "tb1",
            "cloud.cacheToLibrary": "tb1",
            "cloud.prioCustomContent": "tb1",
            "cloud.updateOnLowerAudioId": "tb1",
        }
        self.assertEqual(
            expectations,
            {option_id: self.scope_of(option_id) for option_id in expectations},
        )

    def test_box_overlays_only_show_global_and_matching_generation(self) -> None:
        self.assertTrue(self.visible_in_overlay("toniebox.api_access", "tb1"))
        self.assertTrue(self.visible_in_overlay("toniebox.api_access", "tb2"))
        self.assertTrue(self.visible_in_overlay("toniebox.api_access", None))
        self.assertTrue(self.visible_in_overlay("toniebox.overrideCloud", "tb1"))
        self.assertFalse(self.visible_in_overlay("toniebox.overrideCloud", "tb2"))
        self.assertFalse(self.visible_in_overlay("toniebox.overrideCloud", None))
        self.assertTrue(self.visible_in_overlay("toniebox2.max_volume", "tb2"))
        self.assertFalse(self.visible_in_overlay("toniebox2.max_volume", "tb1"))
        self.assertTrue(self.visible_in_overlay("mqtt_client_upstream.forward.claim", "tb2"))
        self.assertFalse(self.visible_in_overlay("mqtt_client_upstream.forward.claim", "tb1"))
        self.assertTrue(
            self.visible_in_overlay("mqtt_client_upstream.local_control_enabled", "tb2")
        )
        self.assertFalse(
            self.visible_in_overlay("mqtt_client_upstream.local_control_enabled", "tb1")
        )

    def test_all_ici_forward_filters_are_tb2_overlay_settings(self) -> None:
        filter_ids = {
            option_id
            for option_id in self.public_setting_ids
            if option_id.startswith("mqtt_client_upstream.forward.")
        }
        self.assertTrue(filter_ids)
        self.assertTrue(all(self.scope_of(option_id) == "tb2" for option_id in filter_ids))
        self.assertTrue(all(self.overlay_eligible(option_id) for option_id in filter_ids))

    def test_v3_endpoint_dependency_is_complete_and_exact(self) -> None:
        dependency = next(
            item
            for item in self.layout["dependencies"]
            if item["master"] == "cloud.tb2_v3_enabled"
        )
        self.assertEqual(
            {
                "cloud.enableV3FreshnessCheck",
                "cloud.enableV3Ota",
                "cloud.enableV3SetupStatus",
                "cloud.enableV3ContentMeta",
                "cloud.enableV3Chapter",
            },
            set(dependency["dependents"]),
        )
        self.assertTrue(dependency["hideWhenDisabled"])

    def test_tb2_https_settings_are_ordered_without_public_capture_switch(self) -> None:
        section = next(item for item in self.layout["sections"] if item["id"] == "tb2.https")
        self.assertEqual(
            [
                "cloud.tb2_v3_enabled",
                "cloud.enableV3FreshnessCheck",
                "cloud.enableV3Ota",
                "cloud.enableV3SetupStatus",
                "cloud.enableV3ContentMeta",
                "cloud.enableV3Chapter",
                "cloud.tb2_enabled",
                "cloud.tb2_capture_dir",
                "cloud.tb2_capture_max_mib",
                "cloud.remote_hostname_tb2",
                "cloud.remote_port_tb2",
            ],
            section["order"],
        )
        self.assertNotIn("cloud.tb2_capture_enabled", self.public_setting_ids)
        self.assertNotIn("cloud.tb2_capture_enabled", section["ids"])

    def test_dependency_controls_and_listener_cleanup_are_wired(self) -> None:
        scope_tabs = (FORM_COMPONENTS_PATH / "SettingsScopeTabs.tsx").read_text(
            encoding="utf-8"
        )
        option_item = (FORM_COMPONENTS_PATH / "SettingsOptionItem.tsx").read_text(
            encoding="utf-8"
        )
        switch_field = (FORM_COMPONENTS_PATH / "SettingsSwitchField.tsx").read_text(
            encoding="utf-8"
        )
        handler = SETTINGS_HANDLER_PATH.read_text(encoding="utf-8")

        self.assertIn("handler.getSetting(dependency.master)?.value !== enabledWhen", scope_tabs)
        self.assertIn("dependency?.appliesWhen?.every", scope_tabs)
        self.assertIn("dependency?.hideWhenDisabled && isDependencyDisabled(optionId)", scope_tabs)
        self.assertIn("disabled={disabled}", option_item)
        self.assertIn("disabled={disabled}", switch_field)
        self.assertNotIn("changeSetting(", scope_tabs)
        self.assertIn("this.listeners = this.listeners.filter", handler)
        self.assertIn("this.idListeners.push({ iD, listener })", handler)
        self.assertIn("this.idListeners = this.idListeners.filter", handler)
        self.assertIn("return () => handler.removeIdListener(idListener)", switch_field)

    def test_local_control_dependency_covers_all_tb2_settings(self) -> None:
        dependency = next(
            item
            for item in self.layout["dependencies"]
            if item["master"] == "mqtt_client_upstream.local_control_enabled"
        )
        self.assertEqual(["toniebox2."], dependency["dependentPrefixes"])
        self.assertEqual(
            {("mqtt_client_upstream.enabled", True)},
            {(item["setting"], item["value"]) for item in dependency["appliesWhen"]},
        )


if __name__ == "__main__":
    unittest.main()
