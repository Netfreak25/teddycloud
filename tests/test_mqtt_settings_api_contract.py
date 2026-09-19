"""Contracts for cloud-owned TB2 settings at the existing web API boundary.

These checks are offline. Snapshot/revision behavior is exercised by the focused
MQTT settings native test; no running box or server configuration is changed.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
API = (ROOT / "src" / "handler_api.c").read_text(encoding="utf-8")
SETTINGS = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin + len(start))]


class MqttSettingsApiContractTests(unittest.TestCase):
    def test_set_and_reset_share_locked_prewrite_ownership_guard(self):
        guard = section(API, "static uint_t api_settings_write(", "/* sanitizes")
        self.assertLess(guard.index("mutex_lock(MUTEX_SETTINGS)"),
                        guard.index("mqtt_settings_cloud_managed("))
        self.assertLess(guard.index("status = 409"), guard.index("settings_reset_id("))
        self.assertLess(guard.index("settings_set_by_string_id("),
                        guard.index("mutex_unlock(MUTEX_SETTINGS)"))
        self.assertIn("const int cloud_field = mqtt_settings_field_index(item)", guard)
        self.assertIn("settings_id != 0 && cloud_field >= 0", guard)
        self.assertIn("(cloud_field >= 0 || desired_policy)", guard)
        self.assertIn("if (lock_box_settings)", guard)
        setter = section(API, "error_t handleApiSettingsSet(", "error_t handleApiSettingsReset(")
        resetter = section(API, "error_t handleApiSettingsReset(", "static bool_t api_is_native_collection_detail_path(")
        self.assertIn("api_settings_write(item, data, overlay, FALSE)", setter)
        self.assertIn("api_settings_write(item, NULL, overlay, TRUE)", resetter)
        for handler in (setter, resetter):
            self.assertIn("status == 409", handler)
            self.assertIn('"ERROR: tonies_settings"', handler)
            self.assertIn("connection->response.statusCode = status", handler)

    def test_overlay_desired_policy_reapplies_snapshot_without_echo(self):
        guard = section(API, "static uint_t api_settings_write(", "/* sanitizes")
        self.assertIn('!osStrcmp(item, "mqtt_client_upstream.forward.settings.desired")', guard)
        self.assertIn("if (lock_box_settings && desired_policy)", guard)
        self.assertLess(guard.index("if (success)"),
                        guard.index("mqtt_settings_reapply_overlays_locked();"))
        self.assertLess(guard.index("mqtt_settings_reapply_overlays_locked();"),
                        guard.index("mutex_unlock(MUTEX_SETTINGS)"))
        self.assertNotIn("mqtt_server_", guard)

    def test_unknown_nonempty_overlay_cannot_write_global_settings(self):
        resolver = section(API, "static bool_t api_settings_resolve_overlay(", "static error_t api_settings_error(")
        self.assertIn("overlay == NULL || overlay[0] == '\\0'", resolver)
        self.assertIn("*settings_id != 0", resolver)
        self.assertIn("internal.config_used", resolver)
        guard = section(API, "static uint_t api_settings_write(", "/* sanitizes")
        self.assertLess(guard.index("!api_settings_resolve_overlay("),
                        guard.index("settings_set_by_string_id("))
        self.assertIn("status = 404", guard)
        index = section(API, "error_t handleApiGetIndex(", "error_t handleApi")
        self.assertIn("!api_settings_resolve_overlay(", index)

    def test_dynamic_metadata_is_limited_to_exact_cloud_fields(self):
        index = section(API, "error_t handleApiGetIndex(", "error_t handleApi")
        self.assertIn("mqtt_settings_field_index(opt->option_name)", index)
        self.assertIn("if (cloud_field >= 0)", index)
        self.assertIn('"readOnlyReason", "tonies_settings"', index)
        self.assertIn('"cloudSettingsState"', index)
        self.assertIn('settings_id == 0 ? "local" : mqtt_settings_field_state(', index)
        self.assertIn("settings_id != 0 && mqtt_settings_cloud_managed(scope)", index)
        self.assertNotRegex(index, r"opt->read_only\s*=")
        self.assertNotIn("managementSource", index)
        self.assertNotIn("managementState", index)
        self.assertLess(index.index("mutex_lock(MUTEX_SETTINGS)"),
                        index.index("mqtt_settings_field_state("))

    def test_overlay_reload_reapplies_cloud_snapshot_without_recursive_save_lock(self):
        wrapper = section(SETTINGS, "error_t settings_save_overlays_locked(", "error_t settings_apply")
        self.assertIn("return settings_save_ovl(true);", wrapper)
        self.assertNotIn("mutex_lock", wrapper)
        loader = section(SETTINGS, "static error_t settings_load_ovl(bool overlay)\n{", "static ")
        self.assertIn("if (err == NO_ERROR && overlay)", loader)
        self.assertEqual(loader.count("mqtt_settings_reapply_overlays_locked();"), 2)

    def test_config_write_and_flush_errors_are_not_reported_as_success(self):
        save = section(SETTINGS, "static error_t settings_save_ovl(bool overlay)\n{", "error_t settings_load()")
        self.assertIn("if (overlayPrefix == NULL)", save)
        self.assertIn("buffer == NULL ? ERROR_OUT_OF_MEMORY : NO_ERROR", save)
        self.assertIn("write_error = fsWriteFile(file, buffer, osStrlen(buffer));", save)
        self.assertIn("return write_error;", save)
        self.assertIn("flush_error = fsFlushFile(file);", save)
        self.assertLess(save.index("return flush_error;"),
                        save.index("internal.config_changed = false"))
        trigger = section(API, "error_t handleApiTrigger(", "error_t handleApiSettingsGet(")
        self.assertIn("error_t save_error = settings_save();", trigger)
        self.assertIn("status = 500", trigger)
        self.assertIn("connection->response.statusCode = status", trigger)


if __name__ == "__main__":
    unittest.main()
