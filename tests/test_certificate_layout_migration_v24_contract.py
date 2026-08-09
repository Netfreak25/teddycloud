#!/usr/bin/env python3
"""Focused contracts for certificate layout v24 and safe initialization."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CertificateLayoutMigrationV24ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.cert = (ROOT / "src/cert.c").read_text(encoding="utf-8")
        cls.doctor = (ROOT / "contrib/verify-tc-certificates.sh").read_text(
            encoding="utf-8"
        )

    def test_v24_rewrites_global_and_overlay_legacy_roots(self):
        self.assertIn("settings->configVersion < 24", self.settings)
        self.assertIn("settings_migrate_certificate_paths(settingsId)", self.settings)
        self.assertIn('"core.certdir"', self.settings)
        self.assertIn('"core.server_cert.file.ca"', self.settings)
        self.assertIn('"core.client_cert_tb1.file.ca"', self.settings)
        self.assertIn('"core.client_cert_fake.file.ca"', self.settings)
        self.assertIn("settingsId > 0 && !option->overlayed", self.settings)

    def test_legacy_directories_are_preserved_as_numbered_hidden_backups(self):
        self.assertIn('#define LEGACY_SERVER_BACKUP_DIR "certs/.server.bak"', self.settings)
        self.assertIn('#define LEGACY_CLIENT_BACKUP_DIR "certs/.client.bak"', self.settings)
        self.assertIn('custom_asprintf("%s.%u", backupBase, suffix)', self.settings)
        self.assertIn("fsRenameFile(legacy, backup)", self.settings)
        self.assertNotIn("fsDeleteDir(legacy", self.settings)

    def test_migration_blocks_autogeneration_after_conflict(self):
        self.assertIn("settings_certificate_layout_failed = result != NO_ERROR", self.settings)
        self.assertIn(
            "Automatic certificate generation disabled because certificate directory migration failed",
            self.settings,
        )

    def test_overlays_cannot_generate_global_server_certificates(self):
        overlay_guard = self.settings[
            self.settings.index("error_t settings_load_certs_id") :
            self.settings.index("static bool test_client_ca")
        ]
        self.assertIn("if (settingsId > 0)", overlay_guard)
        self.assertIn("overlays never trigger global certificate generation", overlay_guard)
        self.assertNotIn("cert_generate_default();", overlay_guard.split("if (settingsId > 0)", 1)[1].split("return tb1LoadError;", 1)[0])

    def test_safe_initializers_stage_validate_and_rollback(self):
        self.assertIn("cert_publish_new_file", self.cert)
        self.assertIn("cert_validate_server_files", self.cert)
        self.assertIn("CA certificate/key pair is incomplete", self.cert)
        self.assertIn("server certificate/key pair is incomplete", self.cert)
        self.assertIn("newly published files were rolled back", self.cert)
        self.assertNotIn("No TB1 certificates found. Generating", self.settings)

    def test_doctor_treats_hidden_backups_as_inactive(self):
        self.assertIn("check_hidden_certificate_backups", self.doctor)
        self.assertIn("not used at runtime", self.doctor)
        self.assertIn("check_operational_legacy_paths", self.doctor)
        self.assertNotIn("inventory_client_tree TB1 client\n", self.doctor)


if __name__ == "__main__":
    unittest.main(verbosity=2)
