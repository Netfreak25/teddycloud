#!/usr/bin/env python3
"""Focused contracts for generation-aware client certificate uploads."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CertificateUploadGenerationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.handler = (ROOT / "src" / "handler_api.c").read_text(encoding="utf-8")
        cls.header = (ROOT / "include" / "handler_api.h").read_text(encoding="utf-8")
        cls.web_api = (ROOT / "teddycloud_web" / "src" / "api" / "apis" / "TeddyCloudApi.ts").read_text(encoding="utf-8")
        cls.web_drop = (ROOT / "teddycloud_web" / "src" / "components" / "common" / "form" / "CertificatesDragAndDrop.tsx").read_text(encoding="utf-8")
        cls.web_modal = (ROOT / "teddycloud_web" / "src" / "components" / "tonieboxes" / "common" / "modals" / "CertificatesModal.tsx").read_text(encoding="utf-8")
        cls.web_models = (ROOT / "teddycloud_web" / "src" / "hooks" / "useBoxModels.ts").read_text(encoding="utf-8")

    def test_upload_directory_uses_generation_root_and_box_id(self):
        helper = self.handler[
            self.handler.index("static char *certificate_upload_directory") :
            self.handler.index("error_t handleApiUploadCert")
        ]
        self.assertIn('settings_get_string("internal.certdirfull")', helper)
        self.assertIn('settings_get_string("internal.certdirfull_tb2")', helper)
        self.assertIn("settings_canonicalize_box_id", helper)
        self.assertIn("osStringToLower(boxId)", helper)
        self.assertIn('custom_asprintf("%s%c%s", baseDirectory, PATH_SEPARATOR, boxId)', helper)

    def test_webui_can_request_non_destructive_upload(self):
        upload = self.handler[
            self.handler.index("error_t handleApiUploadCert") :
            self.handler.index("error_t file_save_start_suffix")
        ]
        self.assertIn('queryGet(queryString, "overwrite"', upload)
        self.assertIn("ctx.reject_existing = !allowOverwrite", upload)
        self.assertIn("statusCode = 409", upload)
        self.assertIn('"Certificate already exists"', upload)

    def test_existing_file_is_rejected_before_opening(self):
        file_start = self.handler[
            self.handler.index("error_t file_save_start") :
            self.handler.index("error_t file_save_add")
        ]
        self.assertLess(file_start.index("if (ctx->reject_existing)"), file_start.index("fsOpenFile"))
        self.assertIn("ctx->existing_file = true", file_start)
        self.assertIn("bool reject_existing;", self.header)
        self.assertIn("bool existing_file;", self.header)

    def test_generation_specific_certificate_settings_remain_separate(self):
        cert_end = self.handler[
            self.handler.index("error_t file_save_end_cert") :
            self.handler.index("static char *certificate_upload_directory")
        ]
        for generation in ("tb1", "tb2"):
            for suffix in ("ca", "crt", "key"):
                self.assertIn(f'"core.client_cert_{generation}.file.{suffix}"', cert_end)

    def test_webui_sets_generation_and_shares_overwrite_confirmation(self):
        self.assertIn('"toniebox.boxGeneration"', self.web_modal)
        self.assertIn('nextGeneration === "tb2" ? 2 : 1', self.web_modal)
        self.assertIn("overwriteConfirmation.current", self.web_drop)
        self.assertIn("error.response.status !== 409", self.web_drop)
        self.assertIn('queryParameters["overwrite"] = overwrite', self.web_api)

    def test_tb2_model_recommendation_is_explicit(self):
        self.assertIn('item.generation === "tb1" || item.generation === "tb2"', self.web_models)
        self.assertIn('item.name?.startsWith("TB2 - ")', self.web_models)
        self.assertIn('recommendedGeneration === "tb2"', self.web_modal)


if __name__ == "__main__":
    unittest.main(verbosity=2)
