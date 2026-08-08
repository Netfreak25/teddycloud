#!/usr/bin/env python3
"""Focused contracts for assigning native TB2 collections to any Tonie."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2NativeLibrarySourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.content_header = (ROOT / "include/contentJson.h").read_text(
            encoding="utf-8"
        )
        cls.content = (ROOT / "src/contentJson.c").read_text(encoding="utf-8")
        cls.native = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.playlist = (ROOT / "src/tonie_audio_playlist.c").read_text(
            encoding="utf-8"
        )
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(
            encoding="utf-8"
        )
        cls.web_select = (
            ROOT
            / "teddycloud_web/src/components/tonies/common/modals/SelectAudioModal.tsx"
        ).read_text(encoding="utf-8")
        cls.web_browser = (
            ROOT
            / "teddycloud_web/src/components/tonies/filebrowser/FileBrowser.tsx"
        ).read_text(encoding="utf-8")
        cls.web_player = (
            ROOT / "teddycloud_web/src/provider/AudioProvider.tsx"
        ).read_text(encoding="utf-8")
        cls.web_columns = (
            ROOT
            / "teddycloud_web/src/components/tonies/filebrowser/helper/Columns.tsx"
        ).read_text(encoding="utf-8")
        cls.repair_script = (
            ROOT / "contrib/repair_tb2_native_library.py"
        ).read_text(encoding="utf-8")

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_source_uri_is_canonical_and_not_a_stream(self) -> None:
        self.assertIn("CT_SOURCE_NATIVE_COLLECTION", self.content_header)
        classifier = self.section(
            self.content,
            "if (osStrlen(content_json->source) > 0)",
            'if (jsonGetUInt32(contentJson, "_version")',
        )
        self.assertIn('"lib://by/contentHash/"', classifier)
        self.assertIn("CT_SOURCE_NATIVE_COLLECTION", classifier)
        self.assertLess(
            classifier.index('"lib://by/contentHash/"'),
            classifier.index("isValidTaf("),
        )

    def test_loader_validates_descriptor_and_exact_chapter_paths(self) -> None:
        loader = self.section(
            self.native,
            "error_t v3_native_library_collection_load(",
            "void v3_native_cache_invalidate(",
        )
        for marker in (
            '"schemaVersion"',
            '"boxGeneration"',
            '"format"',
            '"contentHash"',
            '"originalName"',
            '"sha256"',
            '"fileSize"',
            '"path"',
            '"teddycloud_%s_%02" PRIuSIZE ".opus"',
            "fsGetFileSize",
            "v3_native_library_hash_file",
        ):
            self.assertIn(marker, loader)

    def test_save_validates_before_mutating_existing_source(self) -> None:
        setter = self.section(
            self.api,
            "error_t handleApiContentJsonSet(",
            "bool isHexString(",
        )
        validation = setter.index("v3_native_library_collection_load(")
        comparison = setter.index("osStrcmp(item_data, current_source)")
        self.assertLess(validation, comparison)
        self.assertIn("TRUE, &collection", setter[validation:comparison])

    def test_existing_source_lifecycle_is_reused_without_new_fields(self) -> None:
        self.assertNotIn("source_revision", self.content_header)
        self.assertNotIn("nocloud_manual", self.content_header)
        self.assertNotIn("nocloud_source", self.content_header)
        setter = self.section(
            self.api,
            "error_t handleApiContentJsonSet(",
            "bool isHexString(",
        )
        self.assertIn("freshness_mark_content_mapping_changed", setter)

    def test_tb2_uses_library_files_with_target_ruid_names(self) -> None:
        generation = self.section(
            self.cloud,
            "static error_t v3_native_collection_generation_load(",
            "static error_t v3_local_prepare_generation_from_source(",
        )
        self.assertIn(
            '"teddycloud_%.20s_%02" PRIuSIZE "_%s.opus"', generation
        )
        self.assertIn("chapter->path = strdup(stored->path)", generation)
        self.assertNotIn("v3_local_content_generation_save", generation)
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        self.assertIn("CT_SOURCE_NATIVE_COLLECTION", meta)
        self.assertIn("v3_native_collection_generation_load", meta)

    def test_tb1_reuses_fast_packet_remux_and_hash_cache(self) -> None:
        reader = self.section(
            self.playlist,
            "static error_t tap_remux_process_source(",
            "static error_t tap_remux_taf(",
        )
        self.assertIn("source_offset", reader)
        wrapper = self.section(
            self.playlist,
            "error_t tap_remux_native_collection(",
            "error_t tap_publish_taf_replace_safe(",
        )
        self.assertIn("tap_remux_taf", wrapper)
        self.assertIn("TRUE", wrapper)
        serve = self.section(
            self.cloud,
            "static error_t serve_native_collection_tb1(",
            "error_t handleCloudContentExt(",
        )
        for marker in (
            "tb1-native-library",
            "tap_remux_native_collection",
            "tap_send_response_stream_unsafe_with_size",
            "tap_publish_taf_replace_safe",
        ):
            self.assertIn(marker, serve)

    def test_file_index_treats_collection_as_one_entry(self) -> None:
        index = self.section(
            self.api,
            "error_t handleApiFileIndexV2(",
            "error_t handleApiFileIndex(",
        )
        for marker in (
            '"nativeCollection"',
            '"source"',
            '"contentHash"',
            '"chapterCount"',
            '"ogg-opus"',
        ):
            self.assertIn(marker, index)

    def test_file_index_never_parses_native_manifests_as_content_sidecars(self) -> None:
        index = self.section(
            self.api,
            "error_t handleApiFileIndexV2(",
            "error_t handleApiFileIndex(",
        )
        generic_metadata = index.index(
            "tonie_info_t *tafInfo = getTonieInfo(filePathAbsolute"
        )
        self.assertLess(index.index("if (is_native_candidate)"), generic_metadata)
        self.assertLess(index.index("if (isNativeCollectionDetail)"), generic_metadata)
        self.assertIn("api_is_native_collection_detail_path", self.api)

    def test_content_json_loader_rejects_native_library_metadata(self) -> None:
        for marker in (
            "content_json_is_native_library_metadata",
            '"library-entry.json"',
            '"content-meta.json"',
            "return ERROR_ACCESS_DENIED",
        ):
            self.assertIn(marker, self.content)

    def test_select_browser_parent_directory_is_clickable(self) -> None:
        name_content = self.web_columns.index("const nameContent =")
        directory_branch = self.web_columns.index(
            ') : record?.isDir ? (', name_content
        )
        next_branch = self.web_columns.index(
            ") : (", directory_branch + 1
        )
        directory_render = self.web_columns[directory_branch:next_branch]
        self.assertIn("handleDirClick(record.name)", directory_render)
        self.assertIn('role="button"', directory_render)

    def test_manual_doctor_is_dry_run_and_gates_recache_explicitly(self) -> None:
        for marker in (
            '"--apply"',
            '"--recache"',
            'parser.error("--recache requires --apply',
            "is_browser_overwrite",
            "build_cache_index",
            "archived_audio_matches",
            "archived_tonieplay_matches",
            "recache_until_resolved",
            "library-entry.json.before-browser-repair.bak",
        ):
            self.assertIn(marker, self.repair_script)

    def test_documentation_covers_both_generations_and_failure_policy(self) -> None:
        for marker in (
            "## Assigning a native collection as Tonie content",
            "lib://by/contentHash/",
            "target RUID",
            "tb1-native-library",
            "never falls back to Boxine or TONIES",
        ):
            self.assertIn(marker, self.docs)

    def test_webui_selects_collection_as_one_source_and_keeps_taf_only_dialogs(self) -> None:
        self.assertIn("file.nativeCollection.source", self.web_select)
        self.assertIn("selectNativeCollections={!requireTafHeader}", self.web_select)
        self.assertIn("hideNativeCollections={requireTafHeader}", self.web_select)

    def test_webui_plays_all_chapters_and_can_close_collection_detail(self) -> None:
        self.assertIn("playPlaybackItem", self.web_player)
        self.assertIn("NATIVE_COLLECTION_ROOT_PATH", self.web_browser)
        self.assertIn("closeNativeCollectionDetail", self.web_browser)


if __name__ == "__main__":
    unittest.main(verbosity=2)
