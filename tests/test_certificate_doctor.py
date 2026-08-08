#!/usr/bin/env python3
"""Focused behavioral contracts for the generation-aware certificate doctor."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DOCTOR = ROOT / "contrib" / "verify-tc-certificates.sh"


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def require_success(*args: str, cwd: Path | None = None) -> None:
    result = run(*args, cwd=cwd)
    if result.returncode != 0:
        raise AssertionError(f"command failed ({result.returncode}): {' '.join(args)}\n{result.stdout}")


class CertificateDoctorStaticContractTests(unittest.TestCase):
    def test_public_cli_and_read_only_contract_are_present(self) -> None:
        script = DOCTOR.read_text(encoding="utf-8")
        for marker in (
            "--base-path",
            "--verbose",
            "--no-color",
            "NO_COLOR",
            "OpenSSL is required",
            "This doctor made no changes.",
            "partial $selected_generation client override",
            "shares the effective TB2 TONIES client identity",
            "Conflicting legacy/canonical files",
        ):
            self.assertIn(marker, script)
        for forbidden in ("rm -rf -- \"$CERT_ROOT", "mv --", "cp --"):
            self.assertNotIn(forbidden, script)

    def test_official_runtime_images_install_openssl(self) -> None:
        for filename in ("DockerfileAlpine", "DockerfileDebian", "DockerfileUbuntu"):
            dockerfile = (ROOT / filename).read_text(encoding="utf-8")
            runtime = dockerfile[dockerfile.index("EXPOSE 80 443 8443 8883") :]
            self.assertIn("openssl", runtime, filename)

    def test_doctor_defaults_match_registered_settings(self) -> None:
        script = DOCTOR.read_text(encoding="utf-8")
        settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        expected = {
            "core.server_cert.file.crt": "certs/server_tb1/teddy-cert.pem",
            "core.server_cert_tb2.file.crt": "certs/server_tb2/teddy-cert.pem",
            "core.client_cert_tb1.file.crt": "certs/client_tb1/client.der",
            "core.client_cert_tb2.file.crt": "certs/client_tb2/client.der",
            "mqtt_server.cert.crt": "certs/server_tb2/ici.pem",
        }
        for key, value in expected.items():
            self.assertIn(f"set_default {key} {value}", script)
            self.assertIn(f'"{key}"', settings)
            self.assertIn(f'"{value}"', settings)


@unittest.skipIf(os.name == "nt", "behavioral doctor fixtures run in Linux/WSL")
class CertificateDoctorBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("bash") is None or shutil.which("openssl") is None:
            raise unittest.SkipTest("bash and openssl are required")
        cls._workspace = tempfile.TemporaryDirectory(prefix="tc-cert-doctor-tests-")
        cls.fixture = Path(cls._workspace.name) / "valid"
        cls._create_valid_fixture(cls.fixture)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._workspace.cleanup()

    @classmethod
    def _openssl(cls, *args: str, cwd: Path) -> None:
        require_success("openssl", *args, cwd=cwd)

    @classmethod
    def _create_ca(cls, directory: Path, stem: str, subject: str) -> tuple[Path, Path]:
        key = directory / f"{stem}-key.pem"
        cert = directory / f"{stem}.pem"
        cls._openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key), cwd=directory)
        cls._openssl(
            "req",
            "-new",
            "-x509",
            "-sha256",
            "-days",
            "3650",
            "-key",
            str(key),
            "-subj",
            subject,
            "-addext",
            "basicConstraints=critical,CA:TRUE",
            "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
            "-out",
            str(cert),
            cwd=directory,
        )
        return cert, key

    @classmethod
    def _create_leaf(
        cls,
        directory: Path,
        stem: str,
        subject: str,
        ca: Path,
        ca_key: Path,
        sans: tuple[str, ...] = (),
        der: bool = False,
    ) -> tuple[Path, Path]:
        key_pem = directory / f"{stem}-key.pem"
        csr = directory / f"{stem}.csr"
        cert_pem = directory / f"{stem}.pem"
        extensions = directory / f"{stem}.ext"
        lines = ["basicConstraints=critical,CA:FALSE", "keyUsage=critical,digitalSignature"]
        if sans:
            lines.append("subjectAltName=" + ",".join(f"DNS:{name}" for name in sans))
            lines.append("extendedKeyUsage=serverAuth")
        else:
            lines.append("extendedKeyUsage=clientAuth")
        extensions.write_text("\n".join(lines) + "\n", encoding="ascii")
        cls._openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key_pem), cwd=directory)
        cls._openssl("req", "-new", "-key", str(key_pem), "-subj", subject, "-out", str(csr), cwd=directory)
        cls._openssl(
            "x509",
            "-req",
            "-sha256",
            "-days",
            "3650",
            "-in",
            str(csr),
            "-CA",
            str(ca),
            "-CAkey",
            str(ca_key),
            "-CAcreateserial",
            "-extfile",
            str(extensions),
            "-out",
            str(cert_pem),
            cwd=directory,
        )
        if not der:
            return cert_pem, key_pem
        cert_der = directory / f"{stem}.der"
        key_der = directory / f"{stem}-key.der"
        cls._openssl("x509", "-in", str(cert_pem), "-outform", "DER", "-out", str(cert_der), cwd=directory)
        cls._openssl("pkey", "-in", str(key_pem), "-outform", "DER", "-out", str(key_der), cwd=directory)
        return cert_der, key_der

    @classmethod
    def _create_intermediate(
        cls,
        directory: Path,
        stem: str,
        subject: str,
        ca: Path,
        ca_key: Path,
    ) -> tuple[Path, Path]:
        key = directory / f"{stem}-key.pem"
        csr = directory / f"{stem}.csr"
        cert = directory / f"{stem}.pem"
        extensions = directory / f"{stem}.ext"
        extensions.write_text(
            "basicConstraints=critical,CA:TRUE,pathlen:0\n"
            "keyUsage=critical,keyCertSign,cRLSign\n",
            encoding="ascii",
        )
        cls._openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key), cwd=directory)
        cls._openssl("req", "-new", "-key", str(key), "-subj", subject, "-out", str(csr), cwd=directory)
        cls._openssl(
            "x509",
            "-req",
            "-sha256",
            "-days",
            "3650",
            "-in",
            str(csr),
            "-CA",
            str(ca),
            "-CAkey",
            str(ca_key),
            "-CAcreateserial",
            "-extfile",
            str(extensions),
            "-out",
            str(cert),
            cwd=directory,
        )
        return cert, key

    @classmethod
    def _create_valid_fixture(cls, base: Path) -> None:
        config = base / "config"
        certs = base / "certs"
        work = base / "fixture-work"
        config.mkdir(parents=True)
        work.mkdir()
        for name in ("server_tb1", "server_tb2", "client_tb1", "client_tb2"):
            (certs / name).mkdir(parents=True)

        local_ca, local_key = cls._create_ca(work, "local-ca", "/CN=TeddyCloud Root CA/O=Team RevvoX/C=DE")
        tb1_ca, tb1_ca_key = cls._create_ca(work, "tb1-ca", "/CN=Boxine CA/O=Boxine GmbH/C=DE")
        tb2_ca, tb2_ca_key = cls._create_ca(work, "tb2-ca", "/CN=TONIES CA/O=tonies GmbH/C=DE")

        tb1_server_cert, tb1_server_key = cls._create_leaf(
            work, "tb1-server", "/CN=prod.de.tbs.toys/O=Team RevvoX", local_ca, local_key, ("prod.de.tbs.toys",)
        )
        tb2_server_cert, tb2_server_key = cls._create_leaf(
            work, "tb2-server", "/CN=tbs2.tonie.cloud/O=Team RevvoX", local_ca, local_key, ("tbs2.tonie.cloud",)
        )
        mqtt_server_cert, mqtt_server_key = cls._create_leaf(
            work,
            "mqtt-server",
            "/CN=ici.tonie.cloud/O=Team RevvoX",
            local_ca,
            local_key,
            ("ici.tonie.cloud", "ici.dev.tonie.cloud", "ici.stage.tonie.cloud"),
        )
        tb1_client_cert, tb1_client_key = cls._create_leaf(
            work, "tb1-client", "/CN=b'AAAAAAAAAAAA'/O=Boxine GmbH", tb1_ca, tb1_ca_key, der=True
        )
        tb2_client_cert, tb2_client_key = cls._create_leaf(
            work, "tb2-client", "/CN=BBBBBBBBBBBB/O=tonies GmbH", tb2_ca, tb2_ca_key, der=True
        )

        shutil.copy2(local_ca, certs / "server_tb1" / "ca-root.pem")
        shutil.copy2(local_key, certs / "server_tb1" / "ca-key.pem")
        shutil.copy2(tb1_server_cert, certs / "server_tb1" / "teddy-cert.pem")
        shutil.copy2(tb1_server_key, certs / "server_tb1" / "teddy-key.pem")
        cls._openssl("x509", "-in", str(local_ca), "-outform", "DER", "-out", str(certs / "server_tb1" / "ca.der"), cwd=work)
        shutil.copy2(local_ca, certs / "server_tb2" / "ca-root.pem")
        shutil.copy2(local_key, certs / "server_tb2" / "ca-key.pem")
        shutil.copy2(tb2_server_cert, certs / "server_tb2" / "teddy-cert.pem")
        shutil.copy2(tb2_server_key, certs / "server_tb2" / "teddy-key.pem")
        shutil.copy2(mqtt_server_cert, certs / "server_tb2" / "ici.pem")
        shutil.copy2(mqtt_server_key, certs / "server_tb2" / "ici.key")
        cls._openssl("x509", "-in", str(local_ca), "-outform", "DER", "-out", str(certs / "server_tb2" / "ca.der"), cwd=work)

        cls._openssl("x509", "-in", str(tb1_ca), "-outform", "DER", "-out", str(certs / "client_tb1" / "ca.der"), cwd=work)
        shutil.copy2(tb1_client_cert, certs / "client_tb1" / "client.der")
        shutil.copy2(tb1_client_key, certs / "client_tb1" / "private.der")
        cls._openssl("x509", "-in", str(tb2_ca), "-outform", "DER", "-out", str(certs / "client_tb2" / "ca.der"), cwd=work)
        shutil.copy2(tb2_client_cert, certs / "client_tb2" / "client.der")
        shutil.copy2(tb2_client_key, certs / "client_tb2" / "private.der")

        (config / "config.ini").write_text(
            "\n".join(
                (
                    "cloud.enabled=true",
                    "cloud.tb2_v3_enabled=true",
                    "mqtt_server.enabled=true",
                    "mqtt_client_upstream.enabled=true",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        (config / "config.overlay.ini").write_text(
            "\n".join(
                (
                    "overlay.AAAAAAAAAAAA.commonName=AAAAAAAAAAAA",
                    "overlay.AAAAAAAAAAAA.toniebox.boxGeneration=1",
                    "overlay.BBBBBBBBBBBB.commonName=BBBBBBBBBBBB",
                    "overlay.BBBBBBBBBBBB.toniebox.boxGeneration=2",
                )
            )
            + "\n",
            encoding="utf-8",
        )

    def _copy_fixture(self, name: str) -> Path:
        destination = Path(self._workspace.name) / name
        shutil.copytree(self.fixture, destination)
        return destination

    def _doctor(self, base: Path, *, verbose: bool = False) -> subprocess.CompletedProcess[str]:
        args = ["bash", str(DOCTOR), "--base-path", str(base), "--no-color"]
        if verbose:
            args.append("--verbose")
        return run(*args)

    def test_valid_tb1_tb2_roles_and_shared_mqtt_identity(self) -> None:
        result = self._doctor(self.fixture)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("TB1 HTTPS server", result.stdout)
        self.assertIn("TB2 HTTPS server", result.stdout)
        self.assertIn("TB2 ICI-MQTT server", result.stdout)
        self.assertIn("shares the TB2 TONIES identity", result.stdout)
        self.assertIn("Findings: WARN=0 ERROR=0", result.stdout)
        self.assertNotIn("fingerprint=", result.stdout)

    def test_verbose_mode_keeps_details_and_deduplicates_required_sans(self) -> None:
        result = self._doctor(self.fixture, verbose=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("fingerprint=", result.stdout)
        self.assertEqual(result.stdout.count("TB2 HTTPS server: SAN contains tbs2.tonie.cloud"), 1)

    def test_partial_overlay_override_is_an_error(self) -> None:
        base = self._copy_fixture("partial")
        overlay = base / "config" / "config.overlay.ini"
        overlay.write_text(
            overlay.read_text(encoding="utf-8")
            + "overlay.BBBBBBBBBBBB.core.client_cert_tb2.file.crt=certs/client_tb2/client.der\n",
            encoding="utf-8",
        )
        result = self._doctor(base)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("partial TB2 client override (1/3 components)", result.stdout)

    def test_wrong_generation_in_tb1_overlay_is_an_error(self) -> None:
        base = self._copy_fixture("wrong-generation")
        overlay = base / "config" / "config.overlay.ini"
        overlay.write_text(
            overlay.read_text(encoding="utf-8")
            + "\n".join(
                (
                    "overlay.AAAAAAAAAAAA.core.client_cert_tb1.file.ca=certs/client_tb2/ca.der",
                    "overlay.AAAAAAAAAAAA.core.client_cert_tb1.file.crt=certs/client_tb2/client.der",
                    "overlay.AAAAAAAAAAAA.core.client_cert_tb1.file.key=certs/client_tb2/private.der",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        result = self._doctor(base)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("detected certificate generation TB2, expected TB1", result.stdout)

    def test_key_mismatch_is_an_error(self) -> None:
        base = self._copy_fixture("key-mismatch")
        shutil.copy2(base / "certs" / "client_tb1" / "private.der", base / "certs" / "client_tb2" / "private.der")
        result = self._doctor(base)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("leaf certificate and private key do not match", result.stdout)

    def test_chain_mismatch_is_an_error(self) -> None:
        base = self._copy_fixture("chain-mismatch")
        shutil.copy2(base / "certs" / "client_tb1" / "client.der", base / "certs" / "client_tb2" / "client.der")
        result = self._doctor(base)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("leaf certificate does not verify against the configured CA", result.stdout)

    def test_identical_legacy_files_are_warnings(self) -> None:
        base = self._copy_fixture("legacy")
        legacy = base / "certs" / "client"
        legacy.mkdir()
        shutil.copy2(base / "certs" / "client_tb1" / "ca.der", legacy / "ca.der")
        legacy_server = base / "certs" / "server"
        legacy_server.mkdir()
        shutil.copy2(base / "certs" / "server_tb2" / "ici.pem", legacy_server / "ici.pem")
        result = self._doctor(base)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Legacy client: 1 identical duplicate file(s)", result.stdout)
        self.assertIn("Legacy server: 1 identical duplicate file(s)", result.stdout)

        verbose = self._doctor(base, verbose=True)
        self.assertIn("Identical legacy duplicate", verbose.stdout)
        self.assertIn("server/ici.pem and server_tb2/ici.pem", verbose.stdout)

    def test_missing_factory_intermediate_is_informational(self) -> None:
        base = self._copy_fixture("missing-intermediate")
        work = base / "fixture-work"
        root, root_key = self._create_ca(work, "factory-root", "/CN=TONIES Root CA/O=tonies GmbH/C=DE")
        intermediate, intermediate_key = self._create_intermediate(
            work,
            "factory-subca",
            "/CN=TONIES Factory SubCA/O=tonies GmbH/C=DE",
            root,
            root_key,
        )
        leaf, leaf_key = self._create_leaf(
            work,
            "factory-client",
            "/CN=BBBBBBBBBBBB/O=tonies GmbH",
            intermediate,
            intermediate_key,
            der=True,
        )
        self._openssl("x509", "-in", str(root), "-outform", "DER", "-out", str(base / "certs/client_tb2/ca.der"), cwd=work)
        shutil.copy2(leaf, base / "certs/client_tb2/client.der")
        shutil.copy2(leaf_key, base / "certs/client_tb2/private.der")

        result = self._doctor(base, verbose=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("intermediate certificate is not included", result.stdout)
        self.assertNotIn("leaf certificate does not verify against the configured CA", result.stdout)

    def test_unknown_generation_gets_certificate_based_recommendation(self) -> None:
        base = self._copy_fixture("unknown")
        overlay = base / "config" / "config.overlay.ini"
        overlay.write_text(
            "\n".join(
                (
                    "overlay.BBBBBBBBBBBB.commonName=BBBBBBBBBBBB",
                    "overlay.BBBBBBBBBBBB.toniebox.boxGeneration=0",
                    "overlay.BBBBBBBBBBBB.core.client_cert_tb2.file.ca=certs/client_tb2/ca.der",
                    "overlay.BBBBBBBBBBBB.core.client_cert_tb2.file.crt=certs/client_tb2/client.der",
                    "overlay.BBBBBBBBBBBB.core.client_cert_tb2.file.key=certs/client_tb2/private.der",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        result = self._doctor(base)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("recommendation boxGeneration=TB2", result.stdout)

    def test_missing_material_for_disabled_roles_is_not_an_error(self) -> None:
        base = self._copy_fixture("disabled")
        (base / "config" / "config.ini").write_text(
            "\n".join(
                (
                    "cloud.enabled=false",
                    "cloud.tb2_enabled=false",
                    "cloud.tb2_v3_enabled=false",
                    "mqtt_server.enabled=false",
                    "mqtt_client_upstream.enabled=false",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        (base / "config" / "config.overlay.ini").write_text("", encoding="utf-8")
        shutil.rmtree(base / "certs")
        (base / "certs").mkdir()
        result = self._doctor(base)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("  ERROR ", result.stdout)
        self.assertIn("disabled, generation=UNKNOWN", result.stdout)


if __name__ == "__main__":
    unittest.main()
