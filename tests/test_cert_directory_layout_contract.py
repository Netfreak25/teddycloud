#!/usr/bin/env python3
"""Contract checks for the generation-specific certificate directories."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
EXPECTED_DIRS = (
    "certs/client_tb1",
    "certs/client_tb2",
    "certs/server_tb1",
    "certs/server_tb2",
)


class CertificateDirectoryLayoutTests(unittest.TestCase):
    def test_docker_entrypoint_creates_all_generation_directories(self):
        entrypoint = (ROOT / "docker" / "docker-entrypoint.sh").read_text(encoding="utf-8")
        for directory in EXPECTED_DIRS:
            self.assertIn(f"/teddycloud/{directory}", entrypoint)

    def test_docker_entrypoint_migrates_legacy_tb1_directories_without_overwrite(self):
        entrypoint = (ROOT / "docker" / "docker-entrypoint.sh").read_text(encoding="utf-8")
        self.assertIn(
            "migrate_legacy_certificate_directory /teddycloud/certs/server "
            "/teddycloud/certs/server_tb1",
            entrypoint,
        )
        self.assertIn(
            "migrate_legacy_certificate_directory /teddycloud/certs/client "
            "/teddycloud/certs/client_tb1",
            entrypoint,
        )
        self.assertIn('[ -z "$(ls -A "$target_dir" 2>/dev/null)" ]', entrypoint)
        self.assertIn('cp -a "$legacy_dir"/. "$target_dir"/', entrypoint)

    def test_make_workdirs_contains_all_generation_directories(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        for directory in EXPECTED_DIRS:
            self.assertIn(f"{directory}/", makefile)

    def test_generation_specific_defaults_use_matching_directories(self):
        settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        self.assertIn('"core.certdir", &settings->core.certdir, "certs/client_tb1"', settings)
        self.assertIn('"core.certdir_tb2", &settings->core.certdir_tb2, "certs/client_tb2"', settings)
        self.assertIn('"core.server_cert.file.ca", &settings->core.server_cert.file.ca, "certs/server_tb1/ca-root.pem"', settings)
        self.assertIn('"core.server_cert_tb2.file.ca", &settings->core.server_cert_tb2.file.ca, "certs/server_tb2/ca-root.pem"', settings)

    def test_tb2_certificate_generation_uses_tb2_directory(self):
        cert_source = (ROOT / "src" / "cert.c").read_text(encoding="utf-8")
        self.assertIn("get_settings()->internal.certdirfull_tb2", cert_source)
        self.assertIn('certdir = "certs/client_tb2"', cert_source)

    def test_missing_tb1_does_not_trigger_or_overwrite_tb2_certificates(self):
        settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cert_source = (ROOT / "src" / "cert.c").read_text(encoding="utf-8")

        loader_start = settings.index("error_t settings_try_load_certs_id")
        loader_end = settings.index("error_t settings_load_certs_id", loader_start)
        loader = settings[loader_start:loader_end]
        self.assertIn("error_t tb1_error = NO_ERROR", loader)
        self.assertNotIn('ERR_RETURN(load_cert("internal.server.', loader)
        self.assertIn('load_cert("internal.server_tb2.ca"', loader)
        self.assertIn("return tb1_error", loader)

        self.assertIn("error_t generation_error = cert_generate_default()", settings)
        self.assertIn("TB1 certificate generation failed", settings)
        self.assertIn("error_t generation_error = cert_generate_default_tb2()", settings)
        self.assertIn("TB2 certificate generation failed", settings)

        self.assertIn("if (!cert_file_is_nonempty(cacert_der))", cert_source)
        self.assertIn('cert_tb2_reconcile_all("certificate initialization")', cert_source)
        self.assertIn('cert_tb2_reconcile_all("startup validation")', settings)
        self.assertIn("Certificate already matches; no files changed", cert_source)


if __name__ == "__main__":
    unittest.main()
