#!/usr/bin/env python3
"""Scoped diagnostic default/export contracts for the coupled GitHub MQTT path."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
API = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
SETTINGS = (ROOT / "src/settings.c").read_text(encoding="utf-8")
SERVER = (ROOT / "src/server.c").read_text(encoding="utf-8")


def function(source: str, name: str) -> str:
    definition = re.search(r"(?m)^" + re.escape(name) + r"\([^;]*?\n\{", source)
    if definition is None:
        raise AssertionError(f"Function definition not found: {name}")
    start = definition.start()
    return source[start : source.index("\n}", start) + 2]


class MqttDebugContractTests(unittest.TestCase):
    def test_diagnostic_scope_is_checked_before_set_and_reset(self):
        resolver = function(API, "static settings_t *api_mqtt_debug_box")
        self.assertIn("overlay[0] == '\\0'", resolver)
        self.assertIn("(id = get_overlay_id(overlay)) == 0", resolver)
        self.assertIn("internal.config_used", resolver)
        self.assertIn("toniebox.boxGeneration == GENERATION_TB2", resolver)
        for name, mutation in (
            ("error_t handleApiSettingsSet", "settings_set_by_string_ovl("),
            ("error_t handleApiSettingsReset", "settings_reset_id("),
        ):
            body = function(API, name)
            self.assertIn('!osStrcmp(item, "mqtt_server.debug_enabled")', body)
            guard = body.index("api_mqtt_debug_box(queryString) == NULL")
            self.assertLess(guard, body.index(mutation))
            self.assertIn('400, "invalid_debug_scope"', body)
        index = function(API, "error_t handleApiGetIndex")
        self.assertIn('!osStrcmp(opt->option_name, "mqtt_server.debug_enabled")', index)
        self.assertLess(
            index.index("api_mqtt_debug_box(queryString) == NULL"),
            index.index("cJSON *jsonEntry"),
        )

    def test_diagnostic_default_is_box_local_and_explicit_false_is_preserved(self):
        self.assertIn(
            'OPTION_BOOL("mqtt_server.debug_enabled", &settings->mqtt_server.debug_enabled, settingsId > 0,',
            SETTINGS,
        )
        inheritance = function(SETTINGS, "void overlay_settings_init_opt")
        self.assertIn('!osStrcmp(opt->option_name, "mqtt_server.debug_enabled")', inheritance)
        self.assertIn("? opt->init.bool_value :", inheritance)
        loader = function(SETTINGS, "static error_t settings_load_ovl")
        self.assertIn('!overlay && !osStrcmp(option_name, "mqtt_server.debug_enabled")', loader)
        self.assertLess(loader.index("overlay_settings_init();"), loader.index('strcmp(value_str, "false")'))
        self.assertIn('else if (strcmp(value_str, "false") == 0)\n                            {\n                                *((bool *)opt->ptr) = false;', loader)
        reset = function(SETTINGS, "bool settings_reset_id")
        self.assertIn("overlay_settings_init_opt(option, globalOption);", reset)
        self.assertIn(
            '!osStrcmp(item, "mqtt_server.debug_enabled") && settingsId == 0 && value',
            SETTINGS,
        )

    def test_download_resolves_box_and_snapshot_before_sending_headers(self):
        status = function(API, "error_t handleApiMqttDiagnostics")
        download = function(API, "error_t handleApiMqttDiagnosticFile")
        for body in (status, download):
            self.assertIn("api_mqtt_debug_box(queryString)", body)
            self.assertIn("connection->response.noCache = TRUE", body)
            self.assertNotIn("fsOpenFile", body)
        self.assertIn("mqtt_debug_file_open(settings, session, name", download)
        self.assertLess(download.index("mqtt_debug_file_limit("), download.index("httpWriteHeader("))
        self.assertIn("UINT64_MAX", download)
        self.assertIn('409, "recording_changed"', download)
        self.assertIn("ERROR_UNEXPECTED_END_OF_FILE", download)
        self.assertIn("mqtt_debug_file_close(file)", download)
        self.assertIn('{REQ_GET, "/api/diagnostics/mqtt", SERTY_WEB,', SERVER)
        self.assertIn('{REQ_GET, "/api/diagnostics/mqtt/file", SERTY_WEB,', SERVER)


if __name__ == "__main__":
    unittest.main()
