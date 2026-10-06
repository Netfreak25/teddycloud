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
            "static bool_t v3_native_cache_files_complete(",
            "static bool_t v3_native_origin_list_has(",
        )
        self.assertIn("fsGetFileSize", completeness)
        self.assertIn("route->chapters[i].file_size", completeness)
        activation = self.section(
            self.source,
            "static error_t v3_native_activate_route(",
            "void v3_native_cache_meta_capture_init(",
        )
        self.assertIn("if (!v3_native_cache_files_complete(route))", activation)
        self.assertIn("return ERROR_IN_PROGRESS", activation)
        self.assertIn("route->invalidated || !route->selected", activation)

    def test_aborted_or_wrong_size_write_never_becomes_final(self):
        finish = self.section(
            self.source,
            "error_t v3_native_cache_chapter_finish(",
            "void v3_native_cache_chapter_abort(",
        )
        self.assertLess(finish.index("capture->written != capture->expected_size"),
                        finish.index("fsRenameFile"))
        abort = self.source[self.source.index("void v3_native_cache_chapter_abort("):]
        self.assertLess(finish.index("!owner_matches || !route->valid"),
                        finish.index("fsRenameFile"))
        self.assertIn("v3_native_route_find(capture->route_handle)", finish)
        self.assertIn("v3_native_capture_paths_free(capture, capture->owns_capture)", abort)
        self.assertIn("capture->owns_capture && route != NULL", abort)
        self.assertIn("!osStrcmp(route->ruid, capture->ruid)", abort)
        self.assertIn("v3_native_route_unpin(route)", abort)

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

    def test_recovery_is_only_a_single_box_delivery_retry(self):
        helper = self.section(self.handler, "static v3_native_cache_chapter_action_t v3_native_chapter_prepare_recover(",
                              "static int v3_native_auth_hex_value(")
        self.assertLess(helper.index("if (action != V3_NATIVE_CHAPTER_BYPASS)"),
                        helper.index("v3_native_cache_recover_chapter_route("))
        self.assertEqual(helper.count("v3_native_cache_chapter_prepare("), 2)
        self.assertIn("settings->cloud.cacheContentV3", helper)
        self.assertIn("ERROR_OUT_OF_RESOURCES", helper)
        chapter = self.section(self.handler, "error_t handleCloudChapterV3(", "error_t handleCloudOtaV3(")
        original = chapter[chapter.index("native_action = v3_native_chapter_prepare_recover("):]
        self.assertIn("return v3_local_write_empty_status(connection, 503)", original)
        self.assertLess(original.index("v3_native_original_content_allowed("),
                        original.index("v3_native_import_library_if_enabled("))
        self.assertLess(original.index("v3_native_original_content_allowed("),
                        original.index("httpSendResponseStreamUnsafe("))
        readers = self.section(self.source, "static error_t v3_native_snapshot_load_locked(",
                               "error_t v3_native_cache_open_active_manifest(")
        self.assertNotIn("recover_chapter_route", readers)

    def test_recovery_has_no_new_storage_or_unbounded_library_scan(self):
        recovery = self.section(self.source, "static error_t v3_native_recovery_snapshot(",
                                "v3_native_cache_chapter_action_t v3_native_cache_chapter_prepare_plan(")
        self.assertIn("V3_NATIVE_CACHE_META_LIMIT", recovery)
        self.assertIn('"versions", name, auth', recovery)
        self.assertIn('"staging", name, auth', recovery)
        self.assertNotIn("v3_native_write_descriptor", recovery)
        self.assertNotIn("fsReadFile", recovery)
        self.assertNotIn("cloud_request", recovery)
        self.assertLess(recovery.index("mutex_lock(MUTEX_V3_NATIVE_LIBRARY)"),
                        recovery.index("mutex_lock(MUTEX_V3_NATIVE_CACHE)"))
        self.assertIn("complete && capture_enabled", recovery)


if __name__ == "__main__":
    unittest.main(verbosity=2)
