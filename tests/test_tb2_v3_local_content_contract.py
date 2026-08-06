#!/usr/bin/env python3
"""Static contracts for immutable, content-addressed TB2 V3 local content."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3LocalContentContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.local_header = (ROOT / "include/v3_local_content.h").read_text(
            encoding="utf-8"
        )
        cls.local_source = (ROOT / "src/v3_local_content.c").read_text(
            encoding="utf-8"
        )
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.cloud_header = (ROOT / "include/handler_cloud.h").read_text(
            encoding="utf-8"
        )
        cls.mqtt = (ROOT / "src/mqtt_server.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(
            encoding="utf-8"
        )

    @staticmethod
    def function(source: str, start: str, end: str) -> str:
        start_index = source.index(start)
        end_index = source.find(end, start_index + len(start))
        if end_index < 0:
            end_index = len(source)
        return source[start_index:end_index]

    def test_manifest_name_is_derived_from_exact_chapter_bytes(self):
        prepare = self.function(
            self.local_source,
            "error_t v3_local_content_prepare(",
            "void v3_local_content_generation_free(",
        )
        self.assertIn("sha256Update(&writer->sha256", self.local_source)
        self.assertIn(
            "sha256Final(&writer.sha256, descriptor->sha256)", self.local_source
        )
        self.assertIn("v3_local_digest_to_hex", self.local_source)
        self.assertIn(
            '"teddycloud_%.20s_%02" PRIuSIZE "_%s.opus"',
            prepare,
        )
        self.assertIn("prepared[i].descriptor.sha256_hex", prepare)
        self.assertIn("generation->ruid", prepare)
        self.assertIn("canonical[i] = (char)toupper(value)", self.local_source)

    def test_object_path_uses_the_complete_sha256(self):
        prepare = self.function(
            self.local_source,
            "error_t v3_local_content_prepare(",
            "void v3_local_content_generation_free(",
        )
        self.assertIn('#define V3_LOCAL_CONTENT_CACHE_DIR "v3-local"', self.local_source)
        object_path = re.search(
            r'prepared\[i\]\.final_path\s*=\s*v3_local_format_alloc\('
            r'"%s%c%s\.opus"[\s\S]*?prepared\[i\]\.descriptor\.sha256_hex\);',
            prepare,
        )
        self.assertIsNotNone(object_path)
        self.assertNotIn("%.20s", object_path.group(0))

    def test_generation_descriptor_is_exposed_only_after_atomic_publish(self):
        prepare = self.function(
            self.local_source,
            "error_t v3_local_content_prepare(",
            "void v3_local_content_generation_free(",
        )
        remux = prepare.index("v3_local_remux_chapter(")
        publish = prepare.index("v3_local_publish_prepared(")
        expose = prepare.index("generation->chapters = osAllocMem")
        self.assertLess(remux, publish)
        self.assertLess(publish, expose)
        self.assertIn(
            "fsRenameFile(prepared[i].temp_path, prepared[i].final_path)",
            self.local_source,
        )
        self.assertNotIn("fsMoveFile(", self.local_source)
        self.assertIn("v3_local_prepared_free(prepared, chapter_count, TRUE)", prepare)
        self.assertRegex(
            prepare,
            r"if \(error != NO_ERROR\)\s*\{\s*"
            r"v3_local_content_generation_free\(generation\);",
        )
        self.assertIn("No descriptor is returned unless every", self.local_header)

    def test_each_chapter_is_an_independent_ogg_opus_stream(self):
        self.assertIn("ogg_stream_init(&writer.stream", self.local_source)
        self.assertIn("v3_local_writer_write_headers(&writer)", self.local_source)
        self.assertIn('osMemcmp(packet, "OpusHead", 8)', self.local_source)
        self.assertIn('osMemcmp(packet, "OpusTags", 8)', self.local_source)
        self.assertIn("writer->granule_position += writer->pending_samples", self.local_source)
        self.assertIn("writer->pending_audio.data", self.local_source)
        self.assertIn("v3_local_writer_emit_pending(&writer, TRUE)", self.local_source)
        self.assertIn("ogg_stream_clear(&writer->stream)", self.local_source)
        self.assertIn(
            "adjusted_opus_head[V3_LOCAL_CONTENT_OPUS_HEAD_PRESKIP_OFFSET] = 0",
            self.local_source,
        )
        self.assertNotIn("opus_encode(", self.local_source)

    def test_duplicate_taf_chapter_markers_are_compacted(self):
        validation = self.function(
            self.local_source,
            "static error_t v3_local_validate_taf(",
            "error_t v3_local_content_prepare(",
        )
        self.assertIn("start_page < previous_start", validation)
        self.assertIn(
            "chapter_starts[*chapter_count - 1] == start_page", validation
        )
        self.assertIn("chapter_starts[(*chapter_count)++] = start_page", validation)

    def test_descriptor_load_does_not_rehash_all_chapter_payloads(self):
        object_validation = self.function(
            self.local_source,
            "static error_t v3_local_validate_object(",
            "static uint32_t v3_local_chapter_serial(",
        )
        self.assertIn("fsGetFileSize(expected_path, &actual_size)", object_validation)
        self.assertNotIn("sha256Update", object_validation)

    def test_local_chapter_miss_is_a_strict_404_without_tonies_fallback(self):
        chapter = self.function(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        local_start = chapter.index('"teddycloud_"')
        cloud_start = chapter.index("cloud.enableV3Chapter", local_start)
        local_path = chapter[local_start:cloud_start]

        self.assertIn("v3_local_content_find_chapter", local_path)
        self.assertIn("v3_local_write_empty_status(connection, 404)", local_path)
        status_helper = self.function(
            self.cloud,
            "static error_t v3_local_write_empty_status(",
            "static error_t v3_local_reject_legacy_range(",
        )
        self.assertIn("connection->response.statusCode = status_code", status_helper)
        self.assertIn("return httpWriteResponse", status_helper)
        self.assertNotIn("handleCloudContentExt", local_path)
        self.assertNotIn("cloud_request_tb2_get", local_path)

    def test_legacy_chapter_names_are_gated_per_ruid(self):
        self.assertIn("uint64_t *v3HashedChapterUids", self.settings_header)
        self.assertIn('"internal.v3HashedChapterUids"', self.settings)

        meta = self.function(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        chapter = self.function(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        self.assertIn("sendLocalContentMetaV3", meta)
        transition = self.function(
            self.cloud,
            "static bool_t v3_local_hashed_uid_add(",
            "static error_t v3_local_write_empty_status(",
        )
        self.assertIn('"internal.v3HashedChapterUids"', transition)
        self.assertIn("freshness_settings_array_add_uid", transition)
        self.assertIn("v3_local_hashed_uid_add(settings, ruid)", self.cloud)
        self.assertIn('"internal.v3HashedChapterUids"', chapter)
        self.assertIn("freshness_settings_array_contains", chapter)
        self.assertIn("freshness_source_changed_contains_ruid", chapter)
        self.assertIn("teddycloud_%02", chapter)

    def test_v3_path_no_longer_activates_virtual_taf_splitting(self):
        v3_handlers = self.function(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudOtaV3(",
        )
        for obsolete in (
            "taf_chapter_split",
            "taf_chapter_start_offset",
            "taf_chapter_end_offset",
            "taf_chapter_header_size",
            "Splitting TAF for chapter",
        ):
            self.assertNotIn(obsolete, v3_handlers)

    def test_legacy_resume_is_rejected_before_any_new_bytes_are_appended(self):
        chapter = self.function(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        self.assertIn("V3_LOCAL_CHAPTER_LEGACY", chapter)
        self.assertIn("connection->request.Range.start > 0", chapter)
        self.assertIn("v3_local_reject_legacy_range", chapter)
        range_reject = self.function(
            self.cloud,
            "static error_t v3_local_reject_legacy_range(",
            "static error_t v3_local_prepare_generation_from_source(",
        )
        self.assertIn("connection->response.statusCode = 416", range_reject)
        self.assertIn('"bytes */%" PRIu32', range_reject)

    def test_source_change_survives_meta_and_chapter_requests(self):
        cleanup = self.function(
            self.cloud,
            "static void freshness_clear_cache_after_content_request(",
            "static void freshness_store_v3_inventory(",
        )
        source_guard = cleanup.index("freshness_source_changed_contains_ruid")
        removal = cleanup.index("freshness_clear_cache_after_v3_request")
        self.assertLess(source_guard, removal)
        self.assertNotIn("api == V3_CONTENT_META &&", cleanup)

    def test_only_exact_playback_version_confirms_source_change(self):
        confirmation = self.function(
            self.cloud,
            "bool_t freshness_confirm_v3_content_version(",
            "static void freshness_evaluate_tonie(",
        )
        self.assertIn(
            "freshness_cache_source_changed_contains(settings, uid)", confirmation
        )
        self.assertIn("freshness_v3_content_meta_version", confirmation)
        mismatch = confirmation.index("content_version != expectedVersion")
        removal = confirmation.index("freshness_cache_remove_uid(settings, uid)")
        self.assertLess(mismatch, removal)
        self.assertRegex(
            confirmation[mismatch:removal],
            r"content_version != expectedVersion\)[\s\S]*?return FALSE;",
        )
        self.assertIn("freshness_confirm_v3_content_version", self.cloud_header)

        playback = self.function(
            self.mqtt,
            "static error_t handle_mqtt_publish_playback_state(",
            "static error_t handle_mqtt_publish_generic(",
        )
        valid = playback.index("if (content_version_valid)")
        confirm = playback.index("freshness_confirm_v3_content_version", valid)
        self.assertLess(valid, confirm)

    def test_tb2_freshness_compares_raw_content_versions(self):
        evaluation = self.function(
            self.cloud,
            "static void freshness_evaluate_tonie(",
            "void process_freshness_check(",
        )
        tb2_policy = evaluation.index("if (!use_tb1_audio_id_policy)")
        tb1_policy = evaluation.index("else if (tap_has_audio_id)", tb2_policy)
        self.assertIn(
            "tonieInfo->updated = boxAudioIdRaw != effectiveServerAudioId;",
            evaluation[tb2_policy:tb1_policy],
        )

    def test_pending_source_change_allocates_version_after_materialization(self):
        content_version = self.function(
            self.cloud,
            "static uint32_t freshness_v3_content_meta_version(",
            "bool_t freshness_confirm_v3_content_version(",
        )
        marker = content_version.index("freshness_cache_source_changed_contains")
        allocate = content_version.index(
            "freshness_set_forced_version_for_source_change", marker
        )
        resolve = content_version.index(
            "freshness_get_effective_server_audio_id", allocate
        )
        self.assertLess(marker, allocate)
        self.assertLess(allocate, resolve)
        self.assertIn("return 0;", content_version[allocate:])

        meta = self.function(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        chapter = self.function(
            self.cloud,
            "error_t handleCloudChapterV3(",
            "error_t handleCloudOtaV3(",
        )
        self.assertIn("contentVersion == 0", meta)
        self.assertIn("if (effectiveVersion == 0)", chapter)


if __name__ == "__main__":
    unittest.main(verbosity=2)
