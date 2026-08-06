#!/usr/bin/env python3
"""Focused contracts for the native TONIES V3 cache."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3NativeCacheContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (ROOT / "include/v3_native_cache.h").read_text(encoding="utf-8")
        cls.source = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.handler = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(encoding="utf-8")
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(encoding="utf-8")

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_functional_setting_is_global_and_overlay_capable(self):
        self.assertIn("bool cacheContentV3;", self.settings_header)
        self.assertIn('OPTION_BOOL("toniebox2.cacheContentV3"', self.settings)
        self.assertIn("&settings->cloud.cacheContentV3, FALSE", self.settings)
        self.assertGreaterEqual(self.handler.count("cloud.cacheContentV3"), 2)

    def test_cache_is_physically_separate_and_keyed_by_overlay_ruid_version(self):
        self.assertIn('#define V3_NATIVE_CACHE_DIR "v3-native"', self.source)
        generation = self.section(
            self.source,
            "static char *v3_native_generation_dir(",
            "static error_t v3_native_write_descriptor(",
        )
        self.assertIn("overlay_id", generation)
        self.assertIn("ruid", generation)
        self.assertIn("version", generation)
        self.assertNotIn('"v3-local"', generation)

    def test_manifest_preserves_safe_original_names_and_rejects_collisions(self):
        parsing = self.section(
            self.source,
            "static error_t v3_native_parse_manifest(",
            "static char *v3_native_generation_dir(",
        )
        self.assertIn("name->valuestring", parsing)
        self.assertIn("v3_native_cache_chapter_name_is_safe", parsing)
        self.assertIn("osStrcasecmp", parsing)
        self.assertIn("ERROR_INVALID_NAME", parsing)

    def test_missing_or_damaged_chapter_cannot_activate(self):
        completeness = self.section(
            self.source,
            "static bool_t v3_native_files_complete(",
            "static error_t v3_native_write_active_marker(",
        )
        self.assertIn("fsGetFileSize", completeness)
        self.assertIn("route->chapters[i].file_size", completeness)
        activation = self.section(
            self.source,
            "static error_t v3_native_activate_route(",
            "void v3_native_cache_meta_capture_init(",
        )
        self.assertIn("if (!v3_native_files_complete(route))", activation)
        self.assertIn("return ERROR_IN_PROGRESS", activation)

    def test_aborted_or_wrong_size_write_never_becomes_final(self):
        finish = self.section(
            self.source,
            "error_t v3_native_cache_chapter_finish(",
            "void v3_native_cache_chapter_abort(",
        )
        self.assertLess(finish.index("capture->written != capture->expected_size"),
                        finish.index("fsRenameFile"))
        abort = self.source[self.source.index("void v3_native_cache_chapter_abort("):]
        self.assertIn("fsDeleteFile(capture->temp_path)", abort)

    def test_new_version_changes_active_marker_only_after_complete_publish(self):
        activation = self.section(
            self.source,
            "static error_t v3_native_activate_route(",
            "void v3_native_cache_meta_capture_init(",
        )
        rename = activation.index("fsRenameFile(route->generation_dir, version_dir)")
        marker = activation.index("v3_native_write_active_marker")
        self.assertLess(rename, marker)
        self.assertNotIn("fsDeleteFile", activation[:marker])

    def test_same_ruid_is_isolated_by_overlay_and_versions_are_immutable(self):
        self.assertIn("staging/<overlay>/<CANONICAL-RUID>/<version>", self.docs)
        self.assertIn("versions/<overlay>/<CANONICAL-RUID>/<version>", self.docs)
        self.assertIn("Complete older version directories are retained", self.docs)

    def test_handlers_capture_meta_and_chapters_but_keep_passthrough(self):
        self.assertIn("v3_native_cache_meta_capture_append", self.handler)
        self.assertIn("v3_native_cache_chapter_append", self.handler)
        self.assertIn("cbrCloudBodyPassthrough", self.handler)


if __name__ == "__main__":
    unittest.main(verbosity=2)
