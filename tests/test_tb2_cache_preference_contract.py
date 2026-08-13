#!/usr/bin/env python3
"""Focused contracts for per-Tonie TAF/native-V3 cache preference."""

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2CachePreferenceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.content_header = (ROOT / "include/contentJson.h").read_text(encoding="utf-8")
        cls.content = (ROOT / "src/contentJson.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.tonies = (ROOT / "src/toniesJson.c").read_text(encoding="utf-8")
        cls.cache = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.web = ROOT / "teddycloud_web"

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_content_json_is_additive_and_defaults_to_auto(self) -> None:
        self.assertIn("char *cache_preference;", self.content_header)
        self.assertIn('CONTENT_JSON_CACHE_PREFERENCE_AUTO "auto"', self.content_header)
        self.assertIn('CONTENT_JSON_CACHE_PREFERENCE_TAF "taf"', self.content_header)
        self.assertIn('CONTENT_JSON_CACHE_PREFERENCE_V3 "v3"', self.content_header)
        self.assertIn('jsonAddStringToObject(contentJson, "cache_preference"', self.content)
        load = self.section(self.content, "error_t load_content_json(", "error_t save_content_json(")
        self.assertIn("strdup(CONTENT_JSON_CACHE_PREFERENCE_AUTO)", load)
        self.assertIn("is_cache_preference_valid(cache_preference)", load)

    def test_model_mapping_is_unique_and_original_audio_only(self) -> None:
        matcher = self.section(
            self.tonies,
            "static toniesJson_item_t *tonies_byAudioIdTrackCountUnique_base(",
            "toniesJson_item_t *tonies_byAudioIdTrackCountUnique(",
        )
        self.assertIn("if (matches == 1)", matcher)
        self.assertIn("exact_matches == 1", matcher)

        completion = self.section(
            self.cloud,
            "bool_t v3_original_content_metadata_complete(",
            "static bool_t v3_native_serve_cached_original(",
        )
        self.assertIn("tonie_info->json.source[0]", completion)
        self.assertIn("tonie_info->json.tonie_model[0]", completion)
        self.assertIn("tonieplay", completion)
        self.assertIn("tonies_byAudioIdTrackCountUnique", completion)

    def test_active_cache_state_requires_validated_complete_files(self) -> None:
        active_info = self.section(
            self.cache,
            "bool_t v3_native_cache_active_info(",
            "static error_t v3_native_remove_tree(",
        )
        self.assertIn("v3_native_cache_read_active_manifest", active_info)
        self.assertIn("route->valid && route->active", active_info)
        self.assertIn("route->chapter_count > 0", active_info)

    def test_tb2_prefers_v3_before_taf_and_taf_can_fallback_to_v3(self) -> None:
        meta = self.section(
            self.cloud,
            "error_t handleCloudContentMetaV3(",
            "error_t handleCloudChapterV3(",
        )
        first_cache = meta.index("v3_native_serve_cached_original")
        local = meta.index("if (local_candidate)")
        second_cache = meta.index("v3_native_serve_cached_original", first_cache + 1)
        self.assertLess(first_cache, local)
        self.assertGreater(second_cache, local)
        self.assertIn("!prefer_taf_cache", meta[:local])
        self.assertIn("prefer_taf_cache", meta[local:second_cache])
        self.assertNotIn("cloud.cacheContentV3 && !cache_stale", meta)

    def test_api_exposes_both_cache_states_and_safe_playlist_fallbacks(self) -> None:
        tag_info = self.section(self.api, "error_t getTagInfoJson(", "error_t handleApiTagInfo(")
        self.assertIn('"cachePreference"', tag_info)
        self.assertIn('"cacheState"', tag_info)
        self.assertIn('"tafComplete"', tag_info)
        self.assertIn('"v3Complete"', tag_info)
        self.assertIn('"v3SourceComplete"', tag_info)
        self.assertIn('"v3Source"', tag_info)
        self.assertIn('"v3ContentVersion"', tag_info)
        self.assertIn('"preferredOriginalKind"', tag_info)
        self.assertIn('"downloadTriggerKind"', tag_info)
        self.assertIn("v3_native_cache_active_library_source", tag_info)
        self.assertIn("v3_download_complete", tag_info)
        self.assertIn("!client_ctx->settings->cloud.cacheToLibraryV3", tag_info)
        self.assertIn("preferred_cache_complete", tag_info)

        playlist = self.section(
            self.api,
            "static void api_add_native_playlist(",
            "static void api_add_native_collection_playlist(",
        )
        self.assertIn("exact_catalog_tracks", playlist)
        self.assertIn('custom_asprintf("Chapter %u"', playlist)

    def test_web_selector_is_hidden_for_manual_sources_and_all_locales_have_text(self) -> None:
        card = (
            self.web / "src/components/tonies/toniecard/TonieCard.tsx"
        ).read_text(encoding="utf-8")
        modal = (
            self.web / "src/components/tonies/toniecard/modals/EditTonieModal.tsx"
        ).read_text(encoding="utf-8")
        self.assertIn("shouldShowCachePreference", card)
        self.assertIn('(selectedSource || "").trim().length === 0', card)
        self.assertIn("showCachePreference &&", modal)
        for locale in ("de", "en", "es", "fr", "tlh"):
            translation = json.loads(
                (self.web / f"public/translations/{locale}.json").read_text(
                    encoding="utf-8"
                )
            )
            edit_modal = translation["tonies"]["editModal"]
            for key in (
                "cachePreference",
                "cachePreferenceAuto",
                "cachePreferenceTaf",
                "cachePreferenceV3",
                "restoreOriginalHint",
                "restoreOriginalTaf",
                "restoreOriginalV3",
            ):
                self.assertTrue(edit_modal[key], f"{locale}: {key}")

    def test_exactly_one_original_cache_source_can_replace_an_assigned_source(self) -> None:
        card = (
            self.web / "src/components/tonies/toniecard/TonieCard.tsx"
        ).read_text(encoding="utf-8")
        modal = (
            self.web / "src/components/tonies/toniecard/modals/EditTonieModal.tsx"
        ).read_text(encoding="utf-8")
        save_flow = (
            self.web
            / "src/components/tonies/toniecard/hooks/useTonieCardSaveFlow.ts"
        ).read_text(encoding="utf-8")
        actions = (
            self.web
            / "src/components/tonies/toniecard/hooks/useTonieCardActions.ts"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "originalV3Source={tonieCard.cacheState?.v3Source}",
            card,
        )
        self.assertIn(
            "preferredOriginalKind={tonieCard.cacheState?.preferredOriginalKind}",
            card,
        )
        self.assertNotIn("originalTafAvailable", card)
        self.assertNotIn("originalV3Available", card)
        self.assertIn('normalizedCachePreference === "auto"', modal)
        self.assertIn("preferredOriginalKind", modal)
        self.assertIn("originalTafLibrarySource", modal)
        self.assertIn("originalV3LibrarySource", modal)
        self.assertIn("const restoreOriginalSource =", modal)
        self.assertIn("onRestoreOriginalSource(restoreOriginalSource)", modal)
        self.assertEqual(modal.count("<RollbackOutlined />"), 1)
        restore = self.section(
            modal, "const handleRestoreOriginal = () => {", "return ("
        )
        self.assertNotIn("onSelectedCachePreferenceChange", restore)
        self.assertNotIn('onSelectedSourceChange("")', restore)
        self.assertIn("restoredOriginalSource", card)
        self.assertIn("selectedSource === restoredOriginalSource", save_flow)
        self.assertNotIn("cacheState?.v3Source", save_flow)
        self.assertNotIn("modelAudioPath) ||", save_flow)
        self.assertIn('tonieCard.downloadTriggerKind === "v3"', actions)
        self.assertIn("await response.json()", actions)
        self.assertIn("sourceAssigned !== true", actions)
        self.assertIn("await fetchUpdatedTonieCard()", actions)
        self.assertIn('isNativeCollectionSource(tonieCard.source || "")', card)
        self.assertIn("handleSourceSave(needsCachePreferenceSave)", save_flow)
        self.assertIn("sourcePayload + cachePreferencePayload", save_flow)
        self.assertIn("needsCachePreferenceSave && !cachePreferenceSaved", save_flow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
