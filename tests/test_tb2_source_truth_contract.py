#!/usr/bin/env python3
"""Focused contracts for TB2 source selection, versions and NoCloud provenance."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2SourceTruthContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.content_header = (ROOT / "include/contentJson.h").read_text(encoding="utf-8")
        cls.content = (ROOT / "src/contentJson.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.policy = (ROOT / "src/tb2_nocloud_policy.c").read_text(encoding="utf-8")
        cls.native = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_nocloud_provenance_is_persisted_and_effective_value_is_derived(self):
        for field in ("nocloud_manual", "nocloud_source", "source_revision"):
            self.assertIn(field, self.content_header)
            self.assertIn(f'"{field}"', self.content)
        refresh = self.section(
            self.content,
            "void content_json_refresh_nocloud(",
            "void content_json_set_manual_nocloud(",
        )
        self.assertIn("nocloud_manual ||", refresh)
        self.assertIn("nocloud_source", refresh)

    def test_legacy_values_are_read_without_a_bulk_migration(self):
        self.assertIn("legacy_nocloud", self.content)
        self.assertIn("content_json->nocloud_source = osStrlen(content_json->source) > 0", self.content)
        self.assertIn("legacy_nocloud &&", self.content)
        self.assertIn("!content_json->nocloud_source", self.content)

    def test_each_changed_source_updates_revision_and_automatic_lock(self):
        setter = self.section(
            self.api,
            "error_t handleApiContentJsonSet(",
            "bool isHexString(",
        )
        compare = setter.index("osStrcmp(item_data, current_source)")
        revision = setter.index("content_json.source_revision++", compare)
        lock = setter.index("content_json_set_source_nocloud", revision)
        mark = setter.index("source_changed = true", lock)
        self.assertLess(compare, revision)
        self.assertLess(revision, lock)
        self.assertLess(lock, mark)
        self.assertIn("item_data[0] != '\\0'", setter)

    def test_manual_lock_survives_source_removal_and_legacy_ui_repeat_is_not_manual(self):
        setter = self.section(
            self.api,
            "error_t handleApiContentJsonSet(",
            "bool isHexString(",
        )
        self.assertIn("content_json.nocloud_manual", setter)
        self.assertIn("repeated_source_lock", setter)
        policy = self.policy[
            self.policy.index("bool_t tb2_nocloud_policy_blocks_upstream(") :
        ]
        self.assertIn("policy->source_nocloud ||", policy)
        self.assertIn("policy->manual_nocloud && !policy->cloud_override", policy)

    def test_effective_version_is_deterministic_and_not_ordered_by_audio_id(self):
        version = self.section(
            self.cloud,
            "static bool_t freshness_new_forced_version(",
            "static bool_t freshness_get_effective_server_audio_id(",
        )
        self.assertIn("source_revision", version)
        self.assertIn("freshness_version_hash_bytes", version)
        self.assertNotIn("time(NULL)", version)
        self.assertNotIn("minimum", version)
        self.assertIn("candidate == naturalServerAudioId", version)
        self.assertIn("candidate == boxAudioId", version)
        self.assertIn("candidate == previousVersion", version)

    def test_missing_audio_id_still_gets_a_forced_version(self):
        setter = self.section(
            self.cloud,
            "static bool_t freshness_set_forced_version_for_source_change(",
            "static bool_t freshness_source_changed_contains_ruid(",
        )
        self.assertIn("uint32_t naturalAudioId = 0", setter)
        self.assertNotIn("return FALSE;\n    }\n\n    uint32_t previousForcedAudioId", setter)

    def test_source_is_authoritative_and_never_falls_back_to_tonies(self):
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        self.assertIn("bool_t source_configured", meta)
        self.assertIn("bool_t local_candidate = source_configured &&", meta)
        self.assertIn("Configured V3 source", meta)
        configured_error = meta.index("else if (source_configured)")
        upstream = meta.index("cloud_request_tb2_get", configured_error)
        self.assertLess(configured_error, upstream)

    def test_routing_order_and_nocloud_gate_are_explicit(self):
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        cache = meta.index("v3_native_cache_read_active_manifest")
        private = meta.index("bool_t local_candidate = source_configured &&")
        upstream = meta.index("cloud_request_tb2_get", private)
        self.assertLess(cache, private)
        self.assertLess(private, upstream)
        self.assertIn("cloud_access_allowed", meta)
        self.assertIn("source_configured || !cloud_access_allowed", meta)

    def test_original_source_uses_native_manifest_before_tonies(self):
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        cache = meta.index("v3_native_cache_read_active_manifest")
        upstream = meta.index("cloud_request_tb2_get", cache)
        self.assertLess(cache, upstream)
        self.assertIn("httpWriteResponse(connection, manifest, manifest_length", meta)
        self.assertNotIn("teddycloud_", meta[: meta.index("sendLocalContentMetaV3")])

    def test_original_freshness_never_uses_a_local_taf_audio_id(self):
        evaluate = self.section(
            self.cloud,
            "static void freshness_evaluate_tonie(",
            "void process_freshness_check(",
        )
        original = evaluate.index("GENERATION_TB2 && !source_configured")
        early_return = evaluate.index("return;", original)
        natural = evaluate.index("freshness_get_natural_server_audio_id", early_return)
        self.assertLess(early_return, natural)
        self.assertIn("v3_native_cache_active_version", evaluate[original:early_return])

    def test_source_change_invalidates_original_route_and_stays_pending(self):
        marker = self.section(
            self.cloud,
            "static bool_t freshness_mark_content_mapping_changed_for_overlay(",
            "void freshness_mark_content_mapping_changed(",
        )
        self.assertIn("v3_native_cache_invalidate", marker)
        self.assertIn("freshness_cache_add_source_changed_uid", marker)
        self.assertNotIn("freshness_confirm_v3_content_version", self.cloud)
        self.assertIn("fsDeleteFile(marker_path)", self.native)

    def test_marker_clears_only_after_matching_successful_chapter(self):
        confirm = self.section(
            self.cloud,
            "static bool_t freshness_confirm_source_change_after_chapter(",
            "static void freshness_evaluate_tonie(",
        )
        self.assertIn("freshness_source_changed_contains_ruid", confirm)
        self.assertIn("source_configured == private_source", confirm)
        self.assertIn("freshness_v3_content_meta_version", confirm)
        self.assertIn("freshness_cache_remove_uid", confirm)

        chapter = self.section(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        success = chapter.index("connection->response.statusCode == 200")
        confirmation = chapter.index("freshness_confirm_source_change_after_chapter", success)
        self.assertLess(success, confirmation)
        self.assertIn("v3_native_cache_route_matches", chapter)


if __name__ == "__main__":
    unittest.main(verbosity=2)
