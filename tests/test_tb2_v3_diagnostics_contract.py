#!/usr/bin/env python3
"""Focused contracts for TB2 V3 operational diagnostics and runbook coverage."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3DiagnosticsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.proxy = (ROOT / "src/tb2_mqtt_passthrough.c").read_text(
            encoding="utf-8"
        )
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(
            encoding="utf-8"
        )

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_content_route_logs_source_cache_policy_version_and_action(self) -> None:
        handler = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        for value in (
            "source=original-cache cache=hit",
            "source=private cache=bypass",
            "source=original-upstream cache=%s",
            "source=none cache=%s",
            "overlay=%u rUID=%s",
            "effectiveVersion=",
            "activeVersion=",
            "requestedVersion=",
            "versionKind=%s",
            "nocloud=",
            "action=",
        ):
            self.assertIn(value, handler)
        self.assertNotIn("authentication_token", "".join(
            line for line in handler.splitlines() if "TB2 V3 content route" in line
        ))

    def test_chapter_diagnostics_compare_active_and_requested_versions(self) -> None:
        handler = self.section(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        self.assertIn("TB2 V3 chapter route source=original-cache cache=hit", handler)
        self.assertIn("activeVersion=%s requestedVersion=%", handler)
        self.assertIn("TB2 V3 chapter cache version mismatch", handler)
        self.assertIn("Rejecting stale or unknown V3 local chapter", handler)
        self.assertIn("Rejecting TONIES V3 chapter fallback for NoCloud", handler)

    def test_source_lifecycle_log_has_no_source_path_or_auth(self) -> None:
        setter = self.section(
            self.api,
            "error_t handleApiContentJsonSet(",
            "bool isHexString(",
        )
        for action in ('"set"', '"removed"', '"changed"'):
            self.assertIn(action, setter)
        self.assertIn("TB2 content source action=%s", setter)
        self.assertIn("nocloud_manual=%s", setter)
        self.assertIn("nocloud_source=%s", setter)
        self.assertIn("nocloud_effective=%s", setter)
        log_start = setter.index('TRACE_INFO("TB2 content source action=%s')
        log_end = setter.index(");", log_start)
        log_statement = setter[log_start:log_end]
        self.assertNotIn("content_json.source,", log_statement)
        self.assertNotIn("content_json.source)", log_statement)
        self.assertNotIn("authentication", log_statement)

    def test_freshness_and_mqtt_decisions_remain_explicit(self) -> None:
        self.assertIn("Marked rUID %s for V3 freshness update", self.cloud)
        self.assertIn("Merged %", self.cloud)
        self.assertIn("TONIES V3 freshness unavailable or incomplete", self.cloud)
        self.assertIn("TB2 MQTT proxy direction=%s packet_type=PUBLISH", self.proxy)
        self.assertIn("action=%s filter=%s", self.proxy)
        self.assertIn('"local_response_consume"', self.proxy)
        self.assertIn('"nocloud_block"', self.proxy)

    def test_runbook_covers_operations_and_acceptance_matrix(self) -> None:
        for heading in (
            "## Runtime diagnostics",
            "## Safe manual cache maintenance",
            "## Production activation verification",
            "## Rollback to a previous complete cache version",
            "## Acceptance coverage",
        ):
            self.assertIn(heading, self.docs)
        for topic in (
            "activeVersion",
            "requestedVersion",
            "traffic.jsonl",
            "staging",
            "active marker",
            "Range",
            "NoCloud",
            "manual download",
            "library",
        ):
            self.assertIn(topic, self.docs)


if __name__ == "__main__":
    unittest.main()
