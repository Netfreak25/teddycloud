import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3ManualDownloadContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.cache = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.cache_header = (ROOT / "include/v3_native_cache.h").read_text(
            encoding="utf-8"
        )
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(
            encoding="utf-8"
        )

        api_start = cls.api.index("error_t handleApiContentDownload(")
        api_end = cls.api.index("typedef struct", api_start)
        cls.download_api = cls.api[api_start:api_end]

        manual_start = cls.cloud.index("error_t handleCloudContentDownloadV3(")
        manual_end = cls.cloud.index("error_t handleCloudContentMetaV3(", manual_start)
        cls.manual = cls.cloud[manual_start:manual_end]

    def test_download_action_uses_explicit_preference_or_auto_generation(self):
        tb2_branch = self.download_api.index("automatic_tb2_cache")
        tb1_v1 = self.download_api.index('osSprintf((char *)uri, "/v1/content/%s"')
        self.assertLess(tb2_branch, tb1_v1)
        self.assertIn("prefer_v3_cache || automatic_tb2_cache", self.download_api)
        self.assertIn("CONTENT_JSON_CACHE_PREFERENCE_V3", self.download_api)
        self.assertIn("CONTENT_JSON_CACHE_PREFERENCE_AUTO", self.download_api)
        self.assertIn("handleCloudContentDownloadV3", self.download_api)

    def test_explicit_v3_download_uses_selected_overlay_credentials_not_generation(self):
        initial_validation = self.manual[
            : self.manual.index("if (!settings->cloud.tb2_v3_enabled)")
        ]
        self.assertIn("!tb2_ruid_canonicalize", initial_validation)
        self.assertNotIn("boxGeneration", initial_validation)

    def test_tb1_v1_v2_download_path_remains_unchanged(self):
        self.assertIn('osSprintf((char *)uri, "/v1/content/%s"', self.download_api)
        self.assertIn('osSprintf((char *)uri, "/v2/content/%s"', self.download_api)
        self.assertEqual(self.download_api.count("handleCloudContent(connection"), 2)

    def test_tb2_uses_shared_identity_and_saved_authentication(self):
        self.assertIn("content->_has_cloud_auth", self.manual)
        self.assertIn("tb2_content_identity_resolve", self.manual)
        self.assertIn("identity.auth", self.manual)
        self.assertGreaterEqual(self.manual.count("cloud_request_tb2_get("), 2)

    def test_existing_source_and_nocloud_policy_remain_authoritative(self):
        self.assertIn("content->nocloud", self.manual)
        self.assertIn("content->source", self.manual)
        self.assertNotIn("tb2_nocloud_policy", self.manual)

    def test_manifest_and_chapters_reuse_native_cache_pipeline(self):
        for marker in (
            "v3_native_cache_meta_capture_init",
            "v3_native_cache_meta_capture_finish",
            "v3_native_cache_download_plan_get",
            "v3_native_cache_chapter_prepare",
            "v3_native_cache_chapter_append",
            "v3_native_cache_chapter_finish",
            "v3_native_cache_active_version",
        ):
            self.assertIn(marker, self.cloud)
        for forbidden in ("fsOpenFile", "fsWriteFile", "fsRenameFile"):
            self.assertNotIn(forbidden, self.manual)

    def test_complete_manual_download_imports_then_assigns_the_library_source(self):
        activation = self.manual.index("if (!activated)")
        completion = self.manual[activation:]
        metadata = completion.index("v3_original_content_metadata_complete")
        library_import = completion.index("v3_native_cache_import_active_library")
        assignment = completion.index("v3_manual_download_assign_library_source")
        freshness = completion.index("freshness_mark_content_mapping_changed")
        self.assertLess(
            metadata,
            library_import,
        )
        self.assertLess(library_import, assignment)
        self.assertLess(assignment, freshness)
        self.assertIn("cacheToLibraryV3", completion)
        assigned_guard = completion[completion.rindex("if (assigned)") : freshness]
        self.assertIn("if (assigned)", assigned_guard)

    def test_assignment_reloads_content_under_lock_and_preserves_user_changes(self):
        assignment_start = self.cloud.index(
            "static error_t v3_manual_download_assign_library_source("
        )
        assignment_end = self.cloud.index(
            "error_t handleCloudContentDownloadV3(", assignment_start
        )
        assignment = self.cloud[assignment_start:assignment_end]
        self.assertIn("getTonieInfoFromRuid((char *)ruid, true, settings)", assignment)
        self.assertIn("v3_manual_download_content_unchanged", assignment)
        self.assertIn('"generation_changed"', assignment)
        self.assertIn('"content_changed"', assignment)
        self.assertIn('"preference_changed"', assignment)
        self.assertIn('"overlay_generation_changed"', assignment)
        self.assertIn("save_content_json", assignment)
        self.assertNotIn("nocloud =", assignment)
        self.assertNotIn("live =", assignment)
        unchanged_start = self.cloud.index(
            "static bool_t v3_manual_download_content_unchanged("
        )
        unchanged_end = self.cloud.index(
            "static error_t v3_manual_download_assign_library_source(",
            unchanged_start,
        )
        unchanged = self.cloud[unchanged_start:unchanged_end]
        self.assertIn("current->live == initial->live", unchanged)
        self.assertIn("current->nocloud == initial->nocloud", unchanged)
        self.assertIn("current_cloud_ruid", unchanged)
        self.assertIn("initial_cloud_ruid", unchanged)

    def test_tonieplay_import_failure_keeps_the_completed_cache_successful(self):
        completion = self.manual[self.manual.index("if (tonieplay)") :]
        tonieplay = completion[: completion.index("if (!settings->cloud.cacheToLibraryV3)")]
        self.assertIn('"tonieplay_library_import_failed"', tonieplay)
        self.assertIn("V3_MANUAL_DOWNLOAD_COMPLETE", tonieplay)
        self.assertNotIn("v3_manual_download_assign_library_source", tonieplay)
        self.assertNotIn("freshness_mark_content_mapping_changed", tonieplay)
        self.assertNotIn(
            "V3_MANUAL_DOWNLOAD_LIBRARY", tonieplay
        )

    def test_automatic_cache_import_never_assigns_a_source_or_freshness(self):
        start = self.cloud.index("static void v3_native_import_library_if_enabled(")
        end = self.cloud.index("bool_t v3_original_content_metadata_complete(", start)
        automatic = self.cloud[start:end]
        self.assertIn("v3_native_cache_import_active_library", automatic)
        self.assertNotIn("v3_manual_download_assign_library_source", automatic)
        self.assertNotIn("freshness_mark_content_mapping_changed", automatic)

    def test_download_plan_uses_validated_names_and_opaque_auth(self):
        self.assertIn("V3_NATIVE_CACHE_CHAPTER_AUTH_SIZE", self.cache_header)
        self.assertIn("V3_NATIVE_CACHE_OBJECT_AUTH_SIZE 4096", self.cache_header)
        parser_start = self.cache.index("static error_t v3_native_parse_manifest(")
        parser_end = self.cache.index("static char *v3_native_generation_dir(")
        parser = self.cache[parser_start:parser_end]
        self.assertIn("v3_native_cache_chapter_name_is_safe", parser)
        self.assertIn("v3_native_object_string_fits(auth", parser)
        self.assertNotIn("isalnum(value)", parser)

    def test_incomplete_download_restores_previous_active_route(self):
        restore_start = self.cloud.index(
            "static void v3_manual_download_restore_active_route("
        )
        restore_end = self.cloud.index("error_t handleCloudContentDownloadV3(")
        restore = self.cloud[restore_start:restore_end]
        self.assertIn("v3_native_cache_read_active_manifest", restore)
        self.assertGreaterEqual(self.manual.count("TRUE);"), 4)

    def test_errors_are_stage_specific_for_api_and_logs(self):
        for stage in (
            'return "auth";',
            'return "manifest";',
            'return "chapter";',
            'return "activation";',
            'return "library";',
            'return "assignment";',
        ):
            self.assertIn(stage, self.cloud)
        for field in (
            '"stage"',
            '"message"',
            '"upstreamStatus"',
            '"chaptersCompleted"',
            '"chaptersTotal"',
            '"objectsCompleted"',
            '"objectsTotal"',
            '"object"',
            '"cacheComplete"',
            '"libraryImported"',
            '"sourceAssigned"',
            '"assignmentReason"',
            '"source"',
        ):
            self.assertIn(field, self.cloud)
        self.assertIn("TB2 manual content download failed stage=%s", self.cloud)

    def test_manual_download_is_documented_as_same_cache_pipeline(self):
        self.assertIn("Manual generation-aware download", self.docs)
        self.assertIn("v3_native_cache_meta_capture", self.docs)
        self.assertIn("v3_native_cache_chapter_prepare", self.docs)
        self.assertIn("sourceAssigned", self.docs)

    def test_tag_info_exposes_only_a_validated_assignable_v3_source(self):
        tag_info_start = self.api.index("error_t getTagInfoJson(")
        tag_info_end = self.api.index("error_t handleApiTagInfo(", tag_info_start)
        tag_info = self.api[tag_info_start:tag_info_end]
        self.assertIn("v3_native_cache_active_library_source", tag_info)
        self.assertIn('"v3SourceComplete"', tag_info)
        self.assertIn('"v3Source"', tag_info)
        self.assertIn('"preferredOriginalKind"', tag_info)
        self.assertIn('"downloadTriggerKind"', tag_info)
        self.assertIn("preferred_cache_complete = v3_download_complete", tag_info)
        self.assertIn("v3_source_complete ||", tag_info)
        self.assertIn("!client_ctx->settings->cloud.cacheToLibraryV3", tag_info)
        self.assertIn(
            "tafInfo->json._source_type == CT_SOURCE_NATIVE_COLLECTION ||",
            tag_info,
        )
        source_configured = tag_info[tag_info.index("if (source_configured)") :]
        source_configured = source_configured[
            : source_configured.index("else if (!osStrcmp(cache_preference")
        ]
        self.assertNotIn("v3_native_library_collection_load", source_configured)
        self.assertNotIn("v3_tonieplay_library_collection_load", source_configured)


if __name__ == "__main__":
    unittest.main()
