#!/usr/bin/env python3
"""Static guardrails for explicit TB1 certificates and Boxine TLS routing."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb1BoxineCertificateContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cloud = (ROOT / "src/cloud_request.c").read_text(encoding="utf-8")
        cls.handler = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(encoding="utf-8")

    def test_boxine_callback_uses_only_tb1_identity(self):
        callback = self.cloud[
            self.cloud.index("httpClientTlsInitCallbackClientAuthBoxine") :
            self.cloud.index("int_t cloud_request_get")
        ]
        self.assertIn("settings->internal.client_tb1", callback)
        self.assertIn("settings->core.client_cert_tb1.file", callback)
        self.assertIn("settings = get_settings();", callback)
        self.assertNotIn("settings->internal.client_tb2", callback)
        self.assertNotIn("settings->core.client_cert_tb2.file", callback)
        self.assertNotIn("boxGeneration", callback)

    def test_v20_keeps_legacy_tb1_migration_before_loading_certificates(self):
        self.assertIn("#define CONFIG_VERSION 24", self.settings_header)
        self.assertIn('OPTION_INTERNAL_STRING("core.client_cert.file.ca"', self.settings)
        self.assertIn('OPTION_STRING("core.client_cert_tb1.file.ca"', self.settings)
        self.assertIn('"core.client_cert.file.ca",', self.settings)
        self.assertIn('"core.client_cert_tb1.file.ca",', self.settings)
        self.assertIn(
            "Settings_Overlay[i].configVersion = settings_source_config_version;",
            self.settings,
        )

        overlay_load = self.settings[
            self.settings.index("if (overlay)\n    {", self.settings.index("fsCloseFile(file);")) :
            self.settings.index("FsFileStat stat;", self.settings.index("fsCloseFile(file);"))
        ]
        self.assertLess(
            overlay_load.index("settings_migrate_id(i)"),
            overlay_load.index("settings_load_certs_id(i)"),
        )

    def test_tb1_migration_does_not_reenter_settings_mutex(self):
        migration = self.settings[
            self.settings.index("static bool settings_migrate_legacy_tb1_option") :
            self.settings.index("static void settings_migrate_legacy_tb1_client_cert")
        ]
        self.assertNotIn("settings_set_string_id", migration)
        self.assertIn("char *migratedValue = strdup(legacyValue);", migration)
        self.assertIn("tb1Option->overlayed = true;", migration)

    def test_tb1_uploads_target_explicit_tb1_settings(self):
        upload = self.handler[
            self.handler.index("error_t file_save_end_cert") :
            self.handler.index("error_t handleApiUploadCert")
        ]
        self.assertIn('"core.client_cert_tb1.file.ca"', upload)
        self.assertIn('"core.client_cert_tb1.file.crt"', upload)
        self.assertIn('"core.client_cert_tb1.file.key"', upload)
        self.assertNotIn('settingName = "core.client_cert.file.', upload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
