#!/usr/bin/env python3
"""Focused contracts for RUID-bound comments and custom images."""

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "teddycloud_web"


class TonieUserMetadataContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.metadata = (ROOT / "src/tonie_user_metadata.c").read_text(
            encoding="utf-8"
        )
        cls.metadata_header = (ROOT / "include/tonie_user_metadata.h").read_text(
            encoding="utf-8"
        )
        cls.api = (ROOT / "src/handler_api.c").read_text(encoding="utf-8")
        cls.server = (ROOT / "src/server.c").read_text(encoding="utf-8")
        cls.web_api = (WEB / "src/api/apis/TeddyCloudApi.ts").read_text(
            encoding="utf-8"
        )
        cls.editor = (
            WEB
            / "src/components/tonies/toniecard/modals/EditTonieModal.tsx"
        ).read_text(encoding="utf-8")
        cls.card = (
            WEB / "src/components/tonies/toniecard/TonieCard.tsx"
        ).read_text(encoding="utf-8")
        cls.image_utils = (
            WEB / "src/components/tonies/common/utils/imagePathUtils.ts"
        ).read_text(encoding="utf-8")

    def test_ruid_is_validated_and_canonicalized_once_for_storage(self) -> None:
        self.assertIn("TONIE_USER_RUID_LENGTH 16U", self.metadata_header)
        canonicalizer = self.metadata[
            self.metadata.index("error_t tonie_user_canonicalize_ruid") :
            self.metadata.index("static bool_t tonie_user_utf8_continuation")
        ]
        self.assertIn("isxdigit(value)", canonicalizer)
        self.assertIn("toupper(value)", canonicalizer)
        self.assertIn('TONIE_USER_METADATA_DIRECTORY "tonie-metadata"', self.metadata)
        self.assertIn('TONIE_USER_IMAGE_DIRECTORY "ruid"', self.metadata)

    def test_comment_validation_is_unicode_aware_and_atomic(self) -> None:
        self.assertIn("TONIE_USER_COMMENT_MAX_CHARACTERS 255U", self.metadata_header)
        self.assertIn("tonie_user_utf8_character_count", self.metadata)
        self.assertIn("osStrchr(comment, '\\r')", self.metadata)
        self.assertIn("osStrchr(comment, '\\n')", self.metadata)
        self.assertIn('temporary ? ".tmp" : ""', self.metadata)
        self.assertIn("fsMoveFile(temporary_path, path, true)", self.metadata)
        self.assertIn("if (normalized[0] == '\\0')", self.metadata)

    def test_image_upload_is_bounded_magic_checked_and_replace_safe(self) -> None:
        self.assertIn("TONIE_USER_IMAGE_MAX_SIZE (5U * 1024U * 1024U)", self.metadata_header)
        for mime in ("image/png", "image/jpeg", "image/webp", "image/gif"):
            self.assertIn(mime, self.metadata)
        upload = self.api[
            self.api.index("error_t handleApiTonieImageUpload") :
            self.api.index("error_t handleApiTonieImageRemove")
        ]
        self.assertIn("multipart_handle", upload)
        self.assertIn("mutex_lock_id(upload.final_path)", upload)
        self.assertIn("upload.completed", upload)
        self.assertRegex(
            self.api,
            r"fsMoveFile\(upload\.temporary_path,\s*upload\.final_path,\s*true\)",
        )

    def test_api_is_global_and_does_not_touch_content_state(self) -> None:
        for route in (
            'REQ_POST, "/api/tonie/metadata/"',
            'REQ_POST, "/api/tonie/image/upload/"',
            'REQ_POST, "/api/tonie/image/remove/"',
            'REQ_GET, "/api/tonie/image/"',
        ):
            self.assertIn(route, self.server)

        handlers = self.api[
            self.api.index("error_t handleApiTonieMetadata") :
            self.api.index("static bool jsonIsNonEmptyString")
        ]
        self.assertIn("get_settings()->internal.configdirfull", handlers)
        self.assertIn("get_settings()->internal.wwwdirfull", handlers)
        for forbidden in ("freshness_", "nocloud", "source_changed"):
            self.assertNotIn(forbidden, handlers)

        self.assertIn('cJSON_AddStringToObject(jsonEntry, "comment"', self.api)
        self.assertIn('jsonEntry, "customImage"', self.api)

    def test_web_editor_and_displays_use_the_new_fields(self) -> None:
        self.assertIn("apiPostTonieMetadata", self.web_api)
        self.assertIn("apiUploadTonieImage", self.web_api)
        self.assertIn("apiRemoveTonieImage", self.web_api)
        self.assertIn("Array.from(selectedComment).length", self.editor)
        self.assertIn("customImageEnabled", self.editor)
        self.assertIn("CommentOutlined", self.card)
        self.assertIn("resolveTonieDisplayPicture", self.card)
        self.assertIn("[customImage, modelPicture]", self.image_utils)
        self.assertIn("resolveContentDisplayPicture", self.image_utils)
        self.assertIn("prepareTonieImage(file)", self.editor)
        self.assertIn("imageProcessing", self.editor)
        self.assertIn("imageProcessingError", self.editor)

    def test_artwork_lookup_is_read_only_and_refreshes_local_mqtt_only(self) -> None:
        picture = (ROOT / "src/tonie_picture.c").read_text(encoding="utf-8")
        state = (ROOT / "src/toniebox_state.c").read_text(encoding="utf-8")
        for forbidden in ("getTonieInfo(", "load_content_json(", "save_content_json(",
                          "freshness_", "mqtt_server_", "nocloud"):
            self.assertNotIn(forbidden, picture)
        self.assertIn("sha256Update", picture)
        self.assertIn('image/%s?v=%s', picture)
        self.assertIn('mqtt_sendBoxEvent("ContentPicture", url, client_ctx)', state)
        self.assertIn("if (picture_changed) tbs_publish_content_picture(client_ctx)", state)
        self.assertIn("playback_state.tonie, ruid", state)
        self.assertEqual(self.api.count("tbs_refresh_content_picture(ruid);"), 2)

    def test_native_artwork_requires_explicit_tag_context(self) -> None:
        page = (WEB / "src/pages/tonies/TeddyAudioPlayerPage.tsx").read_text(encoding="utf-8")
        self.assertIn("const imageTag = tonieRuid", page)
        self.assertIn("imageTag?.customImage", page)
        self.assertNotIn("assigned?.customImage", page)
        self.assertIn('params.set("ruid", currentItem.tonieRuid)', page)
        self.assertIn("tonieRuid: tonieCard.ruid", self.card)

    def test_all_supported_languages_contain_the_new_copy(self) -> None:
        required = {
            "commentTooltip",
            "comment",
            "customImage",
            "userDataSaved",
            "userDataSaveFailed",
        }
        for language in ("de", "en", "fr", "es", "tlh"):
            translations = json.loads(
                (WEB / f"public/translations/{language}.json").read_text(
                    encoding="utf-8"
                )
            )["tonies"]
            present = {
                "commentTooltip": translations.get("commentTooltip"),
                "comment": translations.get("editModal", {}).get("comment"),
                "customImage": translations.get("editModal", {}).get("customImage"),
                "userDataSaved": translations.get("messages", {}).get("userDataSaved"),
                "userDataSaveFailed": translations.get("messages", {}).get(
                    "userDataSaveFailed"
                ),
            }
            self.assertEqual(required, {key for key, value in present.items() if value}, language)
            self.assertIn("512", translations["editModal"]["customImageHint"])
            self.assertTrue(translations["editModal"]["customImageProcessingFailed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
