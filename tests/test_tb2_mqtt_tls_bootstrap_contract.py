#!/usr/bin/env python3
"""Focused guardrails for the TB2 ICI TLS bootstrap and legacy migration."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2MqttTlsBootstrapContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings_header = (ROOT / "include" / "settings.h").read_text(
            encoding="utf-8"
        )
        cls.settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cls.mqtt = (ROOT / "src" / "mqtt_server.c").read_text(encoding="utf-8")

    def test_v21_migrates_only_exact_legacy_default_paths(self):
        self.assertIn("#define CONFIG_VERSION 21", self.settings_header)
        self.assertIn(
            '#define MQTT_SERVER_LEGACY_CERT_PATH "certs/server/ici.pem"',
            self.settings,
        )
        self.assertIn(
            '#define MQTT_SERVER_LEGACY_KEY_PATH "certs/server/ici.key"',
            self.settings,
        )
        migration = self.settings[
            self.settings.rindex("static bool settings_migrate_string_default") :
            self.settings.rindex("static bool settings_migrate_id")
        ]
        self.assertIn("osStrcmp(*value, legacyValue) != 0", migration)
        self.assertIn("settingsId > 0 && !option->overlayed", migration)
        self.assertIn('"mqtt_server.cert.crt"', migration)
        self.assertIn('"mqtt_server.cert.key"', migration)
        self.assertIn("settings->configVersion < 21", self.settings)

    def test_legacy_files_move_only_as_a_complete_verified_pair(self):
        migration = self.settings[
            self.settings.rindex("static void settings_migrate_legacy_mqtt_server_files") :
            self.settings.rindex("static void settings_changed")
        ]
        self.assertIn(
            "osStrcmp(settings->mqtt_server.cert_crt, MQTT_SERVER_CERT_PATH) != 0",
            migration,
        )
        self.assertIn("!legacyCertExists || !legacyKeyExists", migration)
        self.assertIn("fsFileExists(targetCert) || fsFileExists(targetKey)", migration)
        cert_copy = migration.index("fsCopyFile(legacyCert, targetCert, FALSE)")
        key_copy = migration.index("fsCopyFile(legacyKey, targetKey, FALSE)")
        cert_verify = migration.index("fsCompareFiles(legacyCert, targetCert, NULL)")
        key_verify = migration.index("fsCompareFiles(legacyKey, targetKey, NULL)")
        cert_delete = migration.index("fsDeleteFile(legacyCert)")
        key_delete = migration.index("fsDeleteFile(legacyKey)")
        self.assertLess(cert_copy, key_copy)
        self.assertLess(key_copy, cert_verify)
        self.assertLess(cert_verify, key_verify)
        self.assertLess(key_verify, cert_delete)
        self.assertLess(cert_delete, key_delete)

    def test_listener_returns_before_opening_socket_without_tls_material(self):
        init = self.mqtt[
            self.mqtt.index("void mqtt_server_init()") :
            self.mqtt.index("static const char *mqtt_json_bool")
        ]
        socket_open = init.index("socketOpen(SOCKET_TYPE_STREAM")
        fail_log = init.index("MQTT TLS listener disabled:")
        self.assertLess(fail_log, socket_open)
        fail_block = init[fail_log:socket_open]
        self.assertIn("cert='%s' key='%s'", fail_block)
        self.assertRegex(fail_block, re.compile(r"\breturn;"))

    def test_tls_setup_failure_closes_before_plaintext_mqtt_path(self):
        accept = self.mqtt[self.mqtt.index("// 2. Non-blocking check for new connections") :]
        self.assertIn("if (conn->tlsContext == NULL)", accept)
        self.assertIn('mqtt_connection_close(conn, "TLS context allocation failed")', accept)
        self.assertIn('mqtt_connection_close(conn, "TLS setup failed")', accept)
        self.assertIn("rejected before MQTT parsing", accept)
        allocation_failure = accept[
            accept.index("if (conn->tlsContext == NULL)") :
            accept.index("error_t tlsError")
        ]
        self.assertNotIn("continue;", allocation_failure)
        self.assertNotIn("return;", allocation_failure)
        self.assertIn("else", allocation_failure)

    def test_official_docker_files_publish_ici_port(self):
        for filename in ("DockerfileUbuntu", "DockerfileDebian", "DockerfileAlpine"):
            dockerfile = (ROOT / filename).read_text(encoding="utf-8")
            self.assertRegex(dockerfile, r"(?m)^EXPOSE\s+.*\b8883\b")

        compose = (ROOT / "docker" / "docker-compose.yaml").read_text(
            encoding="utf-8"
        )
        self.assertIn("8883:8883", compose)


if __name__ == "__main__":
    unittest.main()
