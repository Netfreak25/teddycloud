import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Tb2ServerCertificateReconciliationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cert = (ROOT / "src" / "cert.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cls.server = (ROOT / "src" / "server.c").read_text(encoding="utf-8")
        cls.mqtt = (ROOT / "src" / "mqtt_server.c").read_text(encoding="utf-8")

    def test_independent_hostname_settings_and_required_sans(self) -> None:
        self.assertIn(
            'OPTION_STRING("core.server_cert_tb2.hostname"', self.settings
        )
        self.assertIn('"tbs2.tonie.cloud"', self.settings)
        self.assertIn('OPTION_STRING("mqtt_server.hostname"', self.settings)
        for hostname in (
            "ici.tonie.cloud",
            "ici.dev.tonie.cloud",
            "ici.stage.tonie.cloud",
        ):
            self.assertIn(f'"{hostname}"', self.cert)

    def test_rebuild_is_validation_driven_and_reloads_leaf_only(self) -> None:
        reconcile = self.cert.split("error_t cert_tb2_reconcile_service", 1)[1]
        reconcile = reconcile.split("error_t cert_tb2_reconcile_all", 1)[0]
        self.assertIn("cert_tb2_validate_pair", reconcile)
        self.assertIn("x509ValidateCertificate", self.cert)
        self.assertIn("cert_key_matches", self.cert)
        self.assertIn("X509_EXT_KEY_USAGE_SERVER_AUTH", self.cert)
        self.assertIn("cert_has_exact_dns_names", self.cert)
        self.assertNotIn("Generating TB2 CA", reconcile)
        self.assertIn("settings_reload_tb2_server_certificate", self.cert)
        self.assertIn("mqtt_server_reload_certificate", self.cert)

    def test_archive_rollback_and_tls_readers_share_rotation_lock(self) -> None:
        self.assertIn("certs%carchive%ctb2", self.cert)
        self.assertIn("metadata.json", self.cert)
        self.assertIn(".rollback", self.cert)
        self.assertIn("cert_tb2_rotation_lock();", self.server)
        self.assertIn("cert_tb2_rotation_lock();", self.mqtt)

    def test_all_language_files_expose_the_expert_fields(self) -> None:
        keys = {
            "core__server_cert_tb2__hostname",
            "core__server_cert_tb2__rotation_status",
            "mqtt_server__hostname",
            "mqtt_server__cert__rotation_status",
        }
        for language in ("de", "en", "es", "fr", "tlh"):
            path = ROOT / "teddycloud_web" / "public" / "translations" / f"{language}.json"
            options = json.loads(path.read_text(encoding="utf-8"))["settings"]["optionText"]
            self.assertTrue(keys.issubset(options), language)


if __name__ == "__main__":
    unittest.main()
