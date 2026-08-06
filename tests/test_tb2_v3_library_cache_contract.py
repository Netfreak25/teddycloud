#!/usr/bin/env python3
"""Focused contracts for importing complete native TB2 versions into the library."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3LibraryCacheContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = (ROOT / "include/v3_native_cache.h").read_text(encoding="utf-8")
        cls.source = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.handler = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(encoding="utf-8")
        cls.columns = (
            ROOT
            / "teddycloud_web/src/components/tonies/filebrowser/helper/Columns.tsx"
        ).read_text(encoding="utf-8")
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(encoding="utf-8")

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_setting_defaults_off_and_depends_on_effective_content_cache(self) -> None:
        self.assertIn("bool cacheToLibraryV3;", self.settings_header)
        self.assertIn('OPTION_BOOL("toniebox2.cacheToLibraryV3"', self.settings)
        self.assertIn("&settings->cloud.cacheToLibraryV3, FALSE", self.settings)
        self.assertIn('!osStrcmp(opt->option_name, "toniebox2.cacheToLibraryV3")', self.api)
        self.assertIn("read_only = !get_settings_ovl(overlay)->cloud.cacheContentV3", self.api)
        gate = self.section(
            self.handler,
            "static void v3_native_import_library_if_enabled(",
            "static void v3_native_meta_response(",
        )
        self.assertIn("!client_ctx->settings->cloud.cacheContentV3", gate)
        self.assertIn("!client_ctx->settings->cloud.cacheToLibraryV3", gate)

    def test_import_accepts_only_a_complete_active_native_generation(self) -> None:
        import_flow = self.section(
            self.source,
            "error_t v3_native_cache_import_active_library(",
            "void v3_native_cache_invalidate(",
        )
        self.assertLess(
            import_flow.index("v3_native_cache_read_active_manifest"),
            import_flow.index("v3_native_parse_manifest"),
        )
        self.assertIn("parsed_version != version", import_flow)
        self.assertNotIn("staging/<overlay>", import_flow)

    def test_library_is_separate_from_taf_and_content_addressed(self) -> None:
        self.assertNotIn("V3_NATIVE_LIBRARY_DIR", self.header)
        self.assertIn('#define V3_NATIVE_LIBRARY_BY_DIR "by"', self.source)
        self.assertIn(
            '#define V3_NATIVE_LIBRARY_CONTENT_HASH_DIR "contentHash"',
            self.source,
        )
        entry = self.section(
            self.source,
            "static char *v3_native_library_entry_json(",
            "static error_t v3_native_library_write_entry(",
        )
        for field in (
            '"origin", "tonies"',
            '"boxGeneration", "tb2"',
            '"format", "ogg-opus"',
            '"contentHash", content_hash',
            '"originalName"',
            '"sha256"',
            '"origins"',
        ):
            self.assertIn(field, entry)
        self.assertNotIn(".taf", entry.lower())
        self.assertIn('"teddycloud_%s_%02" PRIuSIZE ".opus"', self.source)

    def test_collection_hash_is_derived_only_from_ordered_chapter_content(self) -> None:
        prepare = self.section(
            self.source,
            "static error_t v3_native_library_prepare_chapters(",
            "static cJSON *v3_native_library_origin_json(",
        )
        self.assertIn("v3_native_library_hash_file", prepare)
        self.assertIn("V3_NATIVE_LIBRARY_HASH_DOMAIN", prepare)
        self.assertIn("v3_native_library_hash_u32(&context, (uint32_t)chapter_count)", prepare)
        self.assertIn("v3_native_library_hash_u32(&context, (uint32_t)i)", prepare)
        self.assertIn("prepared[i].file_size", prepare)
        self.assertIn("prepared[i].sha256", prepare)
        self.assertNotIn("overlay_id", prepare)
        self.assertNotIn("ruid", prepare.lower())
        self.assertNotIn("version", prepare.lower())

    def test_import_is_staged_compared_and_renamed_after_metadata(self) -> None:
        import_flow = self.section(
            self.source,
            "error_t v3_native_cache_import_active_library(",
            "void v3_native_cache_invalidate(",
        )
        copy = import_flow.index("fsCopyFile(source, target, TRUE)")
        compare = import_flow.index("fsCompareFiles(source, target, NULL)", copy)
        metadata = import_flow.index("v3_native_library_write_entry", compare)
        rename = import_flow.index("fsRenameFile(stage_dir, final_dir)", metadata)
        self.assertLess(copy, compare)
        self.assertLess(compare, metadata)
        self.assertLess(metadata, rename)
        self.assertIn("v3_native_remove_tree(stage_dir)", import_flow)

    def test_origin_is_metadata_and_content_hash_is_the_library_key(self) -> None:
        import_flow = self.section(
            self.source,
            "error_t v3_native_cache_import_active_library(",
            "void v3_native_cache_invalidate(",
        )
        self.assertIn("V3_NATIVE_LIBRARY_BY_DIR", import_flow)
        self.assertIn("V3_NATIVE_LIBRARY_CONTENT_HASH_DIR", import_flow)
        self.assertIn('v3_native_format("%s%c%s", final_parent,', import_flow)
        self.assertIn("PATH_SEPARATOR, content_hash", import_flow)
        self.assertIn("v3_native_library_add_origin", import_flow)
        self.assertNotIn("V3_NATIVE_LIBRARY_DIR", import_flow)

    def test_existing_identical_collection_is_reused_without_overwrite(self) -> None:
        import_flow = self.section(
            self.source,
            "error_t v3_native_cache_import_active_library(",
            "void v3_native_cache_invalidate(",
        )
        existing = self.section(
            import_flow,
            "if (fsDirExists(final_dir))",
            "error = v3_native_remove_tree(stage_dir);",
        )
        self.assertIn("v3_native_library_entry_matches", existing)
        self.assertIn("ERROR_INVALID_FILE", existing)
        self.assertIn("v3_native_library_add_origin", existing)
        self.assertNotIn("v3_native_remove_tree(final_dir)", existing)

    def test_normal_mitm_cached_replay_and_manual_download_share_import_hook(self) -> None:
        self.assertGreaterEqual(
            self.handler.count("v3_native_import_library_if_enabled("), 4
        )
        self.assertIn("v3_native_cache_meta_capture_finish", self.handler)
        self.assertIn("v3_native_cache_chapter_finish", self.handler)
        meta_handler = self.section(
            self.handler,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        replay = meta_handler.index("v3_native_cache_read_active_manifest(")
        self.assertIn(
            "v3_native_import_library_if_enabled(client_ctx, canonical_ruid)",
            meta_handler[replay : replay + 1800],
        )

    def test_common_library_view_hides_staging_and_keeps_taf_actions_taf_only(self) -> None:
        self.assertIn("V3_NATIVE_LIBRARY_STAGING_DIR", self.api)
        self.assertIn('!osStrcmp(special, "library")', self.api)
        self.assertIn("if (record.tafHeader && handleEditTafMetaDataClick)", self.columns)
        self.assertIn("special !== \"library\"", self.columns)

    def test_documentation_covers_import_failures_and_physical_layout(self) -> None:
        normalized_docs = " ".join(self.docs.split())
        for statement in (
            "## Native TB2 library import",
            "by/contentHash",
            ".tb2-native-staging",
            "library-entry.json",
            "collection hash",
            "teddycloud_",
            "Private TAF sources are not eligible",
            "RUID and overlay are provenance",
        ):
            self.assertIn(statement, normalized_docs)


if __name__ == "__main__":
    unittest.main(verbosity=2)
