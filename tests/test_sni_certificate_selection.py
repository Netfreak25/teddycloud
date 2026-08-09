import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = (ROOT / "src" / "server.c").read_text(encoding="utf-8")
TLS_SERVER = (ROOT / "cyclone" / "cyclone_ssl" / "tls_server.c").read_text(
    encoding="utf-8"
)
TLS_EXTENSIONS = (
    ROOT / "cyclone" / "cyclone_ssl" / "tls_server_extensions.c"
).read_text(encoding="utf-8")
TLS_CONFIG = (ROOT / "include" / "tls_config.h").read_text(encoding="utf-8")


def _function(source: str, name: str) -> str:
    match = re.search(rf"(?:static )?error_t {name}\(.*?\n\}}", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"Function {name} not found")
    return match.group(0)


class SniCertificateSelectionTests(unittest.TestCase):
    def test_mapping_is_always_active_on_box_https_only(self) -> None:
        self.assertNotIn("sni_cert_selection_enabled", SERVER)
        self.assertNotIn("selected_box_generation", SERVER)

        web_callback = _function(SERVER, "httpServerTlsInitCallback")
        box_callback = _function(SERVER, "httpServerBoxTlsInitCallback")
        self.assertNotIn("tlsSetAlpnCallback", web_callback)
        self.assertIn("tlsContext, FALSE", web_callback)
        self.assertIn("tlsSetAlpnCallback", box_callback)
        self.assertIn("httpServerSelectBoxCertificate", box_callback)

    def test_sni_presence_selects_the_box_generation(self) -> None:
        selection = _function(SERVER, "httpServerSelectBoxCertificate")
        self.assertIn("tlsGetServerName(tlsContext)", selection)
        self.assertIn("hostname[0] != '\\0'", selection)
        self.assertIn(
            "httpServerLoadCertificate(tlsContext, useTb2Certificate)",
            selection,
        )
        self.assertNotIn("ERROR_INVALID_NAME", selection)

    def test_exactly_one_certificate_is_loaded(self) -> None:
        self.assertEqual(1, SERVER.count("tlsLoadCertificate("))
        self.assertIn("tlsLoadCertificate(tlsContext, 0", SERVER)
        self.assertNotIn("tlsLoadCertificate(tlsContext, 1", SERVER)

    def test_stock_cyclone_hook_runs_between_sni_and_certificate_selection(self) -> None:
        self.assertIn("#define TLS_SNI_SUPPORT ENABLED", TLS_CONFIG)
        self.assertIn("#define TLS_ALPN_SUPPORT ENABLED", TLS_CONFIG)
        self.assertLess(
            TLS_SERVER.index("tlsParseClientSniExtension"),
            TLS_SERVER.index("tlsParseClientAlpnExtension"),
        )
        self.assertLess(
            TLS_SERVER.index("tlsParseClientAlpnExtension"),
            TLS_SERVER.index("tlsResumeStatefulSession"),
        )

        alpn_parser = _function(TLS_EXTENSIONS, "tlsParseClientAlpnExtension")
        self.assertIn(
            "context->alpnCallback(context, context->selectedProtocol)",
            alpn_parser,
        )


if __name__ == "__main__":
    unittest.main()
