#!/usr/bin/env python3
"""Focused contracts for content-addressed custom playlist display metadata."""

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "teddycloud_web"


class ContentPlaylistContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playlist = (ROOT / "src/content_playlist.c").read_text(encoding="utf-8")
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.handler = (ROOT / "src/handler.c").read_text(encoding="utf-8")
        cls.server = (ROOT / "src/server.c").read_text(encoding="utf-8")
        cls.controls = (
            WEB
            / "src/components/tonieboxes/tonieboxcard/live/TonieboxLiveControls.tsx"
        ).read_text(encoding="utf-8")
        cls.drawer = (
            WEB / "src/components/tonieboxes/tonieboxcard/live/ChapterDrawer.tsx"
        ).read_text(encoding="utf-8")

    def test_metadata_identity_uses_taf_audio_id_and_sha1(self):
        self.assertIn('"%s%c%08" PRIX32 "-%s.json"', self.playlist)
        self.assertIn("tonie_info->tafHeader->audio_id", self.playlist)
        self.assertIn("tonie_info->tafHeader->sha1_hash.data[i]", self.playlist)

    def test_real_taf_chapter_count_is_authoritative(self):
        self.assertIn("return tonie_info->tafHeader->n_track_page_nums;", self.playlist)
        self.assertIn("track_count != expected_count", self.playlist)
        self.assertIn('cJSON_AddNumberToObject(playlist, "chapterCount", chapter_count)', self.api)

    def test_only_stable_custom_tafs_are_editable(self):
        editable = self.playlist[
            self.playlist.index("bool_t content_playlist_is_editable") :
            self.playlist.index("size_t content_playlist_chapter_count")
        ]
        self.assertIn("CT_SOURCE_TAF", editable)
        self.assertNotIn("CT_SOURCE_TAP_CACHED", editable)
        self.assertNotIn("CT_SOURCE_TAP_STREAM", editable)

    def test_write_is_atomic_and_api_validates_current_overlay_content(self):
        self.assertIn('custom_asprintf("%s.tmp", path)', self.playlist)
        self.assertIn("fsMoveFile(temp_path, path, TRUE)", self.playlist)
        self.assertIn("getTonieInfoFromRuid(ruid, false, client_ctx->settings)", self.api)
        self.assertIn('REQ_POST, "/api/content/playlist/"', self.server)

    def test_custom_playlist_does_not_reuse_original_tonie_tracks(self):
        custom_start = self.controls.index("if (tonie?.playlist?.editable)")
        original_start = self.controls.index("const trackCount = Math.max", custom_start)
        custom_path = self.controls[custom_start:original_start]
        self.assertIn("tonie.playlist.chapterCount", custom_path)
        self.assertIn("tonie.trackSeconds.length || sourceTracks.length", custom_path)
        self.assertNotIn("assignedTracks", custom_path)

    def test_editor_saves_title_and_all_chapter_titles_together(self):
        self.assertIn("editPlaylist", self.drawer)
        self.assertIn("draftTitle.trim()", self.drawer)
        self.assertIn("draftTracks.map", self.drawer)
        self.assertIn("onSavePlaylist", self.drawer)
        self.assertIn("savePlaylist", self.drawer)

    def test_chapter_durations_are_calculated_and_saved_with_the_taf_metadata(self):
        self.assertIn("trackPos->total_seconds", self.handler)
        self.assertIn("tafHeader->ogg_granule_position - correction", self.handler)
        self.assertIn("positions->pos[i + 1]", self.playlist)
        self.assertIn("positions->total_seconds", self.playlist)
        self.assertIn('cJSON_AddArrayToObject(json, "durations")', self.playlist)
        self.assertIn('cJSON_AddArrayToObject(playlist, "durations")', self.api)
        self.assertIn("trackDurations={tonie?.playlist?.durations}", self.controls)
        self.assertIn("chapterDuration", self.drawer)

    def test_custom_title_does_not_fall_back_to_original_tonie_metadata(self):
        custom_start = self.controls.index("const series = customContent")
        custom_title = self.controls[
            custom_start : self.controls.index(": tonie?.tonieInfo.series", custom_start)
        ]
        self.assertIn("tonie?.playlist?.title", custom_title)
        self.assertNotIn("tonieInfo.series", custom_title)
        self.assertNotIn("sourceInfo", custom_title)
        playlist_api = self.api[
            self.api.index("static void api_add_content_playlist") :
            self.api.index("error_t getTagInfoJson", self.api.index("static void api_add_content_playlist"))
        ]
        self.assertNotIn("source_item", playlist_api)
        self.assertIn('has_saved_playlist ? saved.title : ""', playlist_api)

    def test_all_languages_contain_playlist_editor_labels(self):
        required = {
            "cancelPlaylistEdit",
            "chapterDuration",
            "chapterTitle",
            "customContent",
            "editPlaylist",
            "playlistSaveFailed",
            "playlistSaveFailedDetails",
            "playlistSaved",
            "playlistSavedDetails",
            "playlistTitle",
            "savePlaylist",
        }
        for locale_path in (WEB / "public/translations").glob("*.json"):
            locale = json.loads(locale_path.read_text(encoding="utf-8"))
            live = locale["tonieboxes"]["live"]
            self.assertFalse(required - live.keys(), locale_path.name)


if __name__ == "__main__":
    unittest.main()
