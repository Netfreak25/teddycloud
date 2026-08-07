#!/usr/bin/env python3
"""Focused source contracts for the type-agnostic TB2 Tonieplay cache."""

from pathlib import Path
import json
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2TonieplayCacheContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = (ROOT / "include/v3_native_cache.h").read_text(encoding="utf-8")
        cls.content_header = (ROOT / "include/contentJson.h").read_text(
            encoding="utf-8"
        )
        cls.cache = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.content = (ROOT / "src/contentJson.c").read_text(encoding="utf-8")
        cls.settings = (ROOT / "src/settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include/settings.h").read_text(encoding="utf-8")
        cls.web_types = (
            ROOT / "teddycloud_web/src/types/fileBrowserTypes.ts"
        ).read_text(encoding="utf-8")
        cls.web_browser = (
            ROOT / "teddycloud_web/src/components/tonies/filebrowser/FileBrowser.tsx"
        ).read_text(encoding="utf-8")
        cls.web_columns = (
            ROOT
            / "teddycloud_web/src/components/tonies/filebrowser/helper/Columns.tsx"
        ).read_text(encoding="utf-8")
        cls.web_select = (
            ROOT
            / "teddycloud_web/src/components/tonies/common/modals/SelectAudioModal.tsx"
        ).read_text(encoding="utf-8")

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_manifest_parser_requires_every_content_object(self) -> None:
        parser = self.section(
            self.cache,
            "static error_t v3_native_parse_manifest(",
            "static bool_t v3_native_objects_all_audio(",
        )
        self.assertIn("cJSON_GetArraySize(content)", parser)
        self.assertNotIn('osStrcmp(type->valuestring, "audio")', parser)
        for field in ("name", "auth", "type", "filename", "fileSize"):
            self.assertIn(f'"{field}"', parser)
        self.assertIn("cJSON *item = NULL;", parser)
        self.assertIn('"application/octet-stream"', parser)
        self.assertIn('"audio/ogg"', parser)

    def test_v3_content_accepts_only_known_tonieplay_uid_family(self) -> None:
        detector = self.section(
            self.cloud,
            '#define TONIE_UID_SUFFIX "0304E0"',
            "static bool_t tonie_cloud_access_allowed(",
        )
        self.assertIn('#define TONIEPLAY_UID_SUFFIX "0104E0"', detector)
        self.assertIn("osStrcasecmp", detector)
        self.assertIn("settings->cloud.markCustomTagByUid", detector)
        self.assertIn("!system_ruid && !tonie_uid && !tonieplay_uid", detector)
        self.assertIn(
            "allow_tonieplay_uid &&\n                               ruid_has_suffix(ruid, TONIEPLAY_UID_SUFFIX)",
            detector,
        )

        content_meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        self.assertIn(
            "getTonieInfoForRequest(connection, uri, RUID_URI_CONTENT_META_BEGIN, queryString, client_ctx, noPassword, TRUE",
            content_meta,
        )

        tb1_content = self.section(
            self.cloud,
            "error_t handleCloudContentExt(",
            "error_t handleCloudContent(",
        )
        self.assertIn(
            "getTonieInfoForRequest(connection, uri, ruid_begin, queryString, client_ctx, noPassword, FALSE",
            tb1_content,
        )

    def test_staging_is_complete_only_for_every_object(self) -> None:
        prepare = self.section(
            self.cache,
            "v3_native_cache_chapter_action_t v3_native_cache_chapter_prepare(",
            "void v3_native_cache_object_content_type(",
        )
        self.assertIn("V3_NATIVE_CHAPTER_STAGED", prepare)
        self.assertIn("route->chapters[index].capturing", prepare)
        self.assertIn("FS_FILE_MODE_TRUNC", prepare)
        complete = self.section(
            self.cache,
            "static bool_t v3_native_files_complete(",
            "static error_t v3_native_read_active_marker(",
        )
        self.assertIn("route->chapter_count", complete)
        self.assertIn("route->chapters[i].file_size", complete)

    def test_legacy_schema_accepts_audio_only(self) -> None:
        loader = self.section(
            self.cache,
            "error_t v3_native_cache_read_active_manifest(",
            "bool_t v3_native_cache_active_version(",
        )
        self.assertIn("V3_NATIVE_CACHE_SCHEMA_LEGACY_AUDIO", loader)
        self.assertIn("!v3_native_objects_all_audio(chapters, chapter_count)", loader)

    def test_observed_mime_is_persisted_and_served(self) -> None:
        self.assertIn("v3_native_cache_object_content_type", self.cloud)
        self.assertIn('osStrcasecmp(header, "Content-Type")', self.cloud)
        self.assertIn('"contentType"', self.cache)
        self.assertIn('"application/octet-stream"', self.cloud)

    def test_tonieplay_library_uses_raw_manifest_and_generic_objects(self) -> None:
        importer = self.section(
            self.cache,
            "error_t v3_native_cache_import_active_tonieplay_library(",
            "error_t v3_tonieplay_library_activate(",
        )
        for marker in (
            "V3_TONIEPLAY_LIBRARY_HASH_DOMAIN",
            "content-meta.json",
            "manifest_length",
            "v3_native_library_hash_file",
            "fsCompareFiles",
            "fsRenameFile(stage_dir, final_dir)",
        ):
            self.assertIn(marker, importer)
        self.assertIn('"objects"', self.cache)
        self.assertIn('"tonieplay-v3"', self.cache)
        self.assertIn("sha256Update(&context, manifest, manifest_length)", importer)

    def test_assignment_is_tb2_only_nocloud_and_never_falls_back(self) -> None:
        self.assertIn("CT_SOURCE_TONIEPLAY_COLLECTION", self.content_header)
        self.assertIn("CT_SOURCE_TONIEPLAY_COLLECTION", self.content)
        setter = self.section(
            self.api, "error_t handleApiContentJsonSet(", "bool isHexString("
        )
        self.assertIn("GENERATION_TB2", setter)
        self.assertIn("content_json.nocloud = true", setter)
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        self.assertIn("v3_tonieplay_library_activate", meta)
        chapter = self.cloud[self.cloud.index("error_t handleCloudChapterV3(") :]
        self.assertIn("v3_tonieplay_library_resolve", chapter)
        self.assertIn("v3_tonieplay_library_route_assigned", chapter)
        self.assertIn("v3_local_write_empty_status(connection, 404)", chapter)

    def test_separate_setting_and_library_api_are_exposed(self) -> None:
        self.assertIn("bool cacheTonieplayToLibraryV3;", self.settings_header)
        self.assertIn(
            'OPTION_BOOL("toniebox2.cacheTonieplayToLibraryV3"', self.settings
        )
        self.assertIn('"tb2_tonieplay_collection"', self.api)
        self.assertIn('"tonieplayCollection"', self.api)
        self.assertIn('"objectCount"', self.api)

    def test_webui_has_no_tonieplay_audio_play_action(self) -> None:
        self.assertIn("TonieplayCollectionSummary", self.web_types)
        self.assertIn("downloadTonieplayCollectionZip", self.web_browser)
        actions = self.section(
            self.web_columns,
            'if (mode === "full" && (record.nativeCollection || record.tonieplayCollection))',
            'if (\n                !record.isDir',
        )
        self.assertIn("record.nativeCollection &&", actions)
        self.assertNotIn("record.tonieplayCollection && (\n                            <Tooltip title={t(\"fileBrowser.playFile\")}", actions)
        self.assertIn("file.tonieplayCollection.source", self.web_select)

    def test_all_translation_files_are_valid_and_contain_tonieplay_keys(self) -> None:
        for language in ("en", "de", "fr", "es", "tlh"):
            path = ROOT / f"teddycloud_web/public/translations/{language}.json"
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertIn("tonieplayCollection", payload["fileBrowser"])
            self.assertIn("tonieplayCollection", payload["tonies"]["selectFileModal"])
            self.assertIn(
                "toniebox2__cacheTonieplayToLibraryV3",
                payload["settings"]["optionText"],
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
