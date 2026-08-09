import ctypes
import _ctypes
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARSER_SOURCE = ROOT / "src" / "tls_client_hello.c"


def _extension(extension_type: int, payload: bytes) -> bytes:
    return extension_type.to_bytes(2, "big") + len(payload).to_bytes(2, "big") + payload


def _client_hello(hostname: str | None, duplicate_sni: bool = False) -> bytes:
    body = b"\x03\x03" + bytes(range(32)) + b"\x00"
    body += b"\x00\x02\x00\x2f" + b"\x01\x00"
    if hostname is not None:
        encoded = hostname.encode("ascii")
        name = b"\x00" + len(encoded).to_bytes(2, "big") + encoded
        sni = _extension(0, len(name).to_bytes(2, "big") + name)
        extensions = sni + (sni if duplicate_sni else b"")
        body += len(extensions).to_bytes(2, "big") + extensions
    return b"\x01" + len(body).to_bytes(3, "big") + body


def _records(handshake: bytes, split_at: int | None = None) -> bytes:
    parts = [handshake] if split_at is None else [handshake[:split_at], handshake[split_at:]]
    return b"".join(
        b"\x16\x03\x03" + len(part).to_bytes(2, "big") + part for part in parts
    )


class SniCertificateSelectionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = (ROOT / "src" / "server.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cls.tls_config = (ROOT / "include" / "tls_config.h").read_text(
            encoding="utf-8"
        )

    def test_setting_and_routing_contract(self) -> None:
        self.assertIn(
            'OPTION_BOOL(CORE_SERVER_SNI_CERT_SELECTION_SETTING', self.settings
        )
        self.assertIn('"tbs2.tonie.cloud"', self.server)
        self.assertIn("ERROR_INVALID_NAME", self.server)
        self.assertIn("GENERATION_TB1", self.server)
        self.assertIn("GENERATION_TB2", self.server)
        self.assertIn("httpServerLoadTb1Certificate(tlsContext, 0)", self.server)
        self.assertIn("httpServerLoadTb2Certificate(tlsContext, 0, TRUE)", self.server)

    def test_peek_parser_is_bounded_and_non_consuming(self) -> None:
        parser = PARSER_SOURCE.read_text(encoding="utf-8")
        self.assertIn("TLS_CLIENT_HELLO_MAX_PEEK_SIZE 32768U", (
            ROOT / "include" / "tls_client_hello.h"
        ).read_text(encoding="utf-8"))
        self.assertGreaterEqual(parser.count("SOCKET_FLAG_PEEK"), 2)
        self.assertNotRegex(parser, r"socketReceive\([^;]+,\s*0\s*\)")
        self.assertIn("TLS_CLIENT_HELLO_INCOMPLETE", parser)

    def test_generation_cycle_and_user_agent_guard(self) -> None:
        for key in (
            "internal.sniGenerationCycle",
            "internal.sniGenerationAppliedCycle",
            "internal.sniGenerationEnabledLatched",
        ):
            self.assertIn(key, self.settings)
        self.assertIn("settings_apply_sni_detected_generation", self.server)
        self.assertIn(
            '!settings_get_bool("core.server.sni_cert_selection_enabled")',
            self.server,
        )
        self.assertIn("selected_box_generation", self.tls_config)

    def test_web_layout_and_translations(self) -> None:
        layout = json.loads(
            (
                ROOT
                / "teddycloud_web"
                / "src"
                / "components"
                / "common"
                / "form"
                / "settingsLayout.json"
            ).read_text(encoding="utf-8")
        )
        server_section = next(
            section for section in layout["sections"] if section["id"] == "global.server"
        )
        self.assertIn("core.server.sni_cert_selection_enabled", server_section["ids"])
        translation_key = "core__server__sni_cert_selection_enabled"
        for language in ("de", "en", "es", "fr", "tlh"):
            translations = json.loads(
                (
                    ROOT
                    / "teddycloud_web"
                    / "public"
                    / "translations"
                    / f"{language}.json"
                ).read_text(encoding="utf-8")
            )
            self.assertIn(translation_key, translations["settings"]["optionText"])


@unittest.skipUnless(
    shutil.which("cc") or shutil.which("cl"), "C compiler not available"
)
class SniClientHelloParserVectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tempdir = tempfile.TemporaryDirectory()
        temp = Path(cls.tempdir.name)
        if shutil.which("cc"):
            cls.library_path = temp / "tls_client_hello_test.so"
            command = [
                "cc",
                "-shared",
                "-fPIC",
                "-std=c11",
                "-DTLS_CLIENT_HELLO_PARSER_ONLY",
                "-Iinclude",
                str(PARSER_SOURCE),
                "-o",
                str(cls.library_path),
            ]
        else:
            cls.library_path = temp / "tls_client_hello_test.dll"
            command = [
                "cl",
                "/nologo",
                "/LD",
                "/std:c11",
                "/DTLS_CLIENT_HELLO_PARSER_ONLY",
                "/Iinclude",
                str(PARSER_SOURCE),
                "/link",
                f"/OUT:{cls.library_path}",
                "/EXPORT:tls_client_hello_parse_sni",
            ]
        subprocess.run(
            command,
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        cls.library = ctypes.CDLL(str(cls.library_path))
        cls.parse = cls.library.tls_client_hello_parse_sni
        cls.parse.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        cls.parse.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(ctypes, "windll"):
            _ctypes.FreeLibrary(cls.library._handle)
        del cls.library
        cls.tempdir.cleanup()

    def parse_records(self, records: bytes) -> tuple[int, str, int]:
        raw = (ctypes.c_uint8 * len(records)).from_buffer_copy(records)
        scratch = (ctypes.c_uint8 * 32768)()
        hostname = ctypes.create_string_buffer(256)
        required = ctypes.c_size_t()
        result = self.parse(
            raw,
            len(records),
            scratch,
            len(scratch),
            hostname,
            len(hostname),
            ctypes.byref(required),
        )
        return result, hostname.value.decode("ascii"), required.value

    def test_no_sni_and_valid_sni(self) -> None:
        self.assertEqual((1, ""), self.parse_records(_records(_client_hello(None)))[:2])
        self.assertEqual(
            (2, "tbs2.tonie.cloud"),
            self.parse_records(_records(_client_hello("tbs2.tonie.cloud")))[:2],
        )
        self.assertEqual(
            (2, "TBS2.TONIE.CLOUD"),
            self.parse_records(_records(_client_hello("TBS2.TONIE.CLOUD")))[:2],
        )

    def test_fragmented_records(self) -> None:
        hello = _client_hello("tbs2.tonie.cloud")
        self.assertEqual(
            (2, "tbs2.tonie.cloud"), self.parse_records(_records(hello, 11))[:2]
        )

    def test_malformed_and_size_limit(self) -> None:
        self.assertEqual(
            3, self.parse_records(_records(_client_hello("tbs2.tonie.cloud", True)))[0]
        )
        oversized_header = b"\x16\x03\x03\x7f\xfc"
        self.assertEqual(4, self.parse_records(oversized_header)[0])


if __name__ == "__main__":
    unittest.main()
