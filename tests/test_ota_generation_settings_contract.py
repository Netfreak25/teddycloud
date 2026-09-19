#!/usr/bin/env python3
"""Focused contracts for generation-specific OTA policy settings."""

from pathlib import Path
import json
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class OtaGenerationSettingsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.settings = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
        cls.settings_header = (ROOT / "include" / "settings.h").read_text(
            encoding="utf-8"
        )
        cls.handler = (ROOT / "src" / "handler.c").read_text(encoding="utf-8")
        cls.cloud = (ROOT / "src" / "handler_cloud.c").read_text(encoding="utf-8")

    def test_v25_replaces_shared_public_switches_with_generation_settings(self) -> None:
        self.assertIn("#define CONFIG_VERSION 25", self.settings_header)
        for option, default in (
            ("cloud.cacheOtaV1", "TRUE"),
            ("cloud.localOtaV1", "FALSE"),
            ("cloud.cacheOtaV3", "TRUE"),
            ("cloud.localOtaV3", "FALSE"),
        ):
            self.assertIn(f'OPTION_BOOL("{option}"', self.settings)
            option_line = next(
                line
                for line in self.settings.splitlines()
                if f'OPTION_BOOL("{option}"' in line
            )
            self.assertIn(default, option_line)

        self.assertIn('OPTION_INTERNAL_BOOL("cloud.cacheOta"', self.settings)
        self.assertIn('OPTION_INTERNAL_BOOL("cloud.localOta"', self.settings)
        self.assertNotIn('OPTION_BOOL("cloud.cacheOta"', self.settings)
        self.assertNotIn('OPTION_BOOL("cloud.localOta"', self.settings)

    def test_v25_migration_copies_each_legacy_value_to_both_generations(self) -> None:
        migration = function(
            self.settings,
            "static void settings_migrate_ota_generation_settings",
            "static bool settings_migrate_id",
        )
        self.assertIn(
            "settings->cloud.cacheOtaV1 = settings->cloud.cacheOtaLegacy", migration
        )
        self.assertIn(
            "settings->cloud.cacheOtaV3 = settings->cloud.cacheOtaLegacy", migration
        )
        self.assertIn(
            "settings->cloud.localOtaV1 = settings->cloud.localOtaLegacy", migration
        )
        self.assertIn(
            "settings->cloud.localOtaV3 = settings->cloud.localOtaLegacy", migration
        )
        self.assertIn("legacyCache->overlayed", migration)
        self.assertIn("legacyLocal->overlayed", migration)
        for option in (
            "CLOUD_V1_CACHE_OTA_SETTING",
            "CLOUD_V3_CACHE_OTA_SETTING",
            "CLOUD_V1_LOCAL_OTA_SETTING",
            "CLOUD_V3_LOCAL_OTA_SETTING",
        ):
            self.assertIn(f"{option}, settingsId)->overlayed = true", migration)
        self.assertIn("settings->configVersion < 25", self.settings)
        self.assertIn("settings_migrate_ota_generation_settings(settingsId)", self.settings)

        migrated = {
            (cache, local): {
                "v1": (cache, local),
                "v3": (cache, local),
            }
            for cache in (False, True)
            for local in (False, True)
        }
        self.assertEqual(4, len(migrated))
        self.assertTrue(all(values["v1"] == values["v3"] for values in migrated.values()))

    def test_cache_callback_selects_policy_from_ota_api(self) -> None:
        cache_policy = function(
            self.handler,
            "bool_t otaCacheEnabled",
            "bool_t otaLocalDeliveryEnabled",
        )
        self.assertIn("api == V1_OTA", cache_policy)
        self.assertIn("settings->cloud.cacheOtaV1", cache_policy)
        self.assertIn("api == V3_OTA", cache_policy)
        self.assertIn("settings->cloud.cacheOtaV3", cache_policy)
        self.assertIn("return FALSE", cache_policy)

        ota_callback = function(
            self.handler,
            "void cbrCloudOtaHeader",
            "void cbrCloudOtaBody",
        )
        self.assertIn(
            "otaCacheEnabled(ctx->client_ctx->settings, ctx->api)", ota_callback
        )
        self.assertNotIn("cacheOtaLegacy", ota_callback)

    def test_tb1_and_tb2_handlers_use_only_their_generation_policy(self) -> None:
        tb1 = function(
            self.cloud,
            "error_t handleCloudOTA(",
            "error_t handleCloudContent(",
        )
        self.assertIn("otaCacheEnabled(client_ctx->settings, V1_OTA)", tb1)
        self.assertIn("otaLocalDeliveryEnabled(client_ctx->settings, V1_OTA)", tb1)
        self.assertNotIn("V3_OTA", tb1)

        tb2 = self.cloud[self.cloud.index("error_t handleCloudOtaV3(") :]
        self.assertIn("otaCacheEnabled(client_ctx->settings, V3_OTA)", tb2)
        self.assertIn("otaLocalDeliveryEnabled(client_ctx->settings, V3_OTA)", tb2)
        self.assertNotIn("cacheOtaV1", tb2)
        self.assertNotIn("localOtaV1", tb2)

    def test_existing_cache_and_delivery_flow_is_preserved(self) -> None:
        tb1 = function(
            self.cloud,
            "error_t handleCloudOTA(",
            "error_t handleCloudContent(",
        )
        tb2 = self.cloud[self.cloud.index("error_t handleCloudOtaV3(") :]
        for handler, api in ((tb1, "V1_OTA"), (tb2, "V3_OTA")):
            cache_check = handler.index(f"otaCacheEnabled(client_ctx->settings, {api})")
            direct_return = handler.index(
                f"!otaCacheEnabled(client_ctx->settings, {api})", cache_check
            )
            local_check = handler.index(
                f"otaLocalDeliveryEnabled(client_ctx->settings, {api})", direct_return
            )
            local_send = handler.index("httpSendResponseStreamUnsafe", local_check)
            self.assertLess(cache_check, direct_return)
            self.assertLess(direct_return, local_check)
            self.assertLess(local_check, local_send)

    def test_webui_translates_only_the_generation_specific_ota_settings(self) -> None:
        translations = ROOT / "teddycloud_web" / "public" / "translations"
        option_ids = (
            "cloud__enableV1Ota",
            "cloud__cacheOtaV1",
            "cloud__localOtaV1",
            "cloud__enableV3Ota",
            "cloud__cacheOtaV3",
            "cloud__localOtaV3",
        )
        for language in ("de", "en", "es", "fr", "tlh"):
            data = json.loads(
                (translations / f"{language}.json").read_text(encoding="utf-8")
            )["settings"]
            self.assertTrue(data["scopeSections"]["firmwareUpdates"])
            self.assertTrue(data["otaHelp"]["title"])
            self.assertEqual(
                {"direct", "cacheOnly", "cacheAndServe", "offlineLocal", "disabled"},
                set(data["otaHelp"]["rows"]),
            )
            for option_id in option_ids:
                self.assertTrue(data["optionText"][option_id]["label"])
                self.assertTrue(data["optionText"][option_id]["description"])
            self.assertNotIn("cloud__cacheOta", data["optionText"])
            self.assertNotIn("cloud__localOta", data["optionText"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
