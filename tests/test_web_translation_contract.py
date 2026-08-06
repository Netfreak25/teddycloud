import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSLATION_DIR = ROOT / "teddycloud_web" / "public" / "translations"
BUNDLE_DIR = ROOT / "contrib" / "data" / "www" / "web"
BUNDLE_TRANSLATION_DIR = BUNDLE_DIR / "translations"
SETTINGS_PATH = ROOT / "src" / "settings.c"
I18N_PATH = ROOT / "teddycloud_web" / "src" / "i18n.ts"

FULL_LANGUAGES = ("en", "de", "fr", "es")
PARTIAL_LANGUAGES = ("tlh",)
ALL_LANGUAGES = FULL_LANGUAGES + PARTIAL_LANGUAGES

PUBLIC_OPTION_PATTERN = re.compile(
    r"\bOPTION_(?:BOOL|STRING|UNSIGNED|SIGNED|FLOAT|READONLY_BOOL|READONLY_STRING)"
    r'\(\s*(?:"([^"]+)"|([A-Z][A-Z0-9_]+))'
)
DEFINE_PATTERN = re.compile(
    r'^\s*#define\s+([A-Z][A-Z0-9_]+)\s+"([^"]+)"', re.MULTILINE
)
PLACEHOLDER_PATTERN = re.compile(r"{{\s*([^}\s]+)\s*}}")


def load_public_setting_ids() -> set[str]:
    source = SETTINGS_PATH.read_text(encoding="utf-8")
    source_without_line_comments = "\n".join(
        line.split("//", 1)[0] for line in source.splitlines()
    )
    constants = dict(DEFINE_PATTERN.findall(source_without_line_comments))
    option_ids: set[str] = set()

    for literal_id, constant_name in PUBLIC_OPTION_PATTERN.findall(
        source_without_line_comments
    ):
        option_id = literal_id or constants.get(constant_name)
        if option_id is None:
            raise AssertionError(
                f"Unresolved settings identifier constant: {constant_name}"
            )
        option_ids.add(option_id)

    return option_ids


def flatten(value: object, parent_key: str = "") -> dict[str, object]:
    flattened: dict[str, object] = {}
    if isinstance(value, dict):
        for key, entry in value.items():
            full_key = f"{parent_key}.{key}" if parent_key else key
            flattened.update(flatten(entry, full_key))
    elif isinstance(value, list):
        for index, entry in enumerate(value):
            flattened.update(flatten(entry, f"{parent_key}[{index}]"))
    else:
        flattened[parent_key] = value
    return flattened


class WebTranslationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.translations = {
            language: json.loads(
                (TRANSLATION_DIR / f"{language}.json").read_text(encoding="utf-8")
            )
            for language in ALL_LANGUAGES
        }
        cls.flattened = {
            language: flatten(translations)
            for language, translations in cls.translations.items()
        }

    def test_full_languages_have_the_same_keys(self) -> None:
        english_keys = set(self.flattened["en"])
        self.assertTrue(english_keys)
        for language in FULL_LANGUAGES[1:]:
            self.assertEqual(english_keys, set(self.flattened[language]), language)

    def test_partial_languages_only_contain_known_fallback_keys(self) -> None:
        english_keys = set(self.flattened["en"])
        for language in PARTIAL_LANGUAGES:
            self.assertTrue(set(self.flattened[language]), language)
            self.assertLessEqual(set(self.flattened[language]), english_keys, language)

        i18n_source = I18N_PATH.read_text(encoding="utf-8")
        self.assertIn('fallbackLng: "en"', i18n_source)
        for language in ALL_LANGUAGES:
            self.assertIn(f'"{language}"', i18n_source)

    def test_klingon_has_visible_curated_ui_vocabulary(self) -> None:
        klingon = self.flattened["tlh"]
        expected = {
            "home.navigationTitle": "juH",
            "language.change": "Hol choH",
            "settings.navigationTitle": "DuHmey",
            "settings.save": "pol",
            "settings.discard": "polHa'",
            "settings.scopeSections.logging": "QonoSmey",
            "settings.scopeSections.security": "Hung",
            "settings.notifications.colStatus": "Dotlh",
        }
        for key, value in expected.items():
            self.assertEqual(value, klingon.get(key), key)

        english = self.flattened["en"]
        localized_values = [
            value
            for key, value in klingon.items()
            if str(value) != str(english[key])
        ]
        self.assertGreaterEqual(len(localized_values), 50)

    def test_removed_elvish_locales_are_not_exposed(self) -> None:
        removed_languages = ("sjn", "qya")
        i18n_source = I18N_PATH.read_text(encoding="utf-8")
        language_switcher = (
            ROOT
            / "teddycloud_web"
            / "src"
            / "components"
            / "common"
            / "header"
            / "StyledLanguageSwitcher.tsx"
        ).read_text(encoding="utf-8")
        translation_utils = (
            ROOT
            / "teddycloud_web"
            / "src"
            / "components"
            / "community"
            / "translation"
            / "utils"
            / "TranslationUtils.ts"
        ).read_text(encoding="utf-8")

        for language in removed_languages:
            self.assertNotIn(f'"{language}"', i18n_source)
            self.assertNotIn(f'key: "{language}"', language_switcher)
            self.assertNotIn(f'"{language}"', translation_utils)
            self.assertFalse((TRANSLATION_DIR / f"{language}.json").exists())
            self.assertFalse((BUNDLE_TRANSLATION_DIR / f"{language}.json").exists())

    def test_translations_preserve_placeholder_names(self) -> None:
        english = self.flattened["en"]
        for language in ALL_LANGUAGES:
            for key, localized_value in self.flattened[language].items():
                expected = sorted(PLACEHOLDER_PATTERN.findall(str(english[key])))
                actual = sorted(PLACEHOLDER_PATTERN.findall(str(localized_value)))
                self.assertEqual(expected, actual, f"{language}: {key}")

    def test_new_localized_settings_contain_no_encoding_replacements(self) -> None:
        for language in FULL_LANGUAGES[1:]:
            localized_sections = (
                self.translations[language]["settings"]["optionText"],
                self.translations[language]["settings"]["mqttForwarding"]["groups"],
            )
            for section in localized_sections:
                for key, value in flatten(section).items():
                    self.assertNotIn("\ufffd", str(value), f"{language}: {key}")
                    self.assertNotIn("?", str(value), f"{language}: {key}")

    def test_every_public_setting_is_translated_in_full_languages(self) -> None:
        public_setting_ids = load_public_setting_ids()
        for language in FULL_LANGUAGES:
            option_text = self.translations[language]["settings"]["optionText"]
            translated_ids = {option_id.replace("__", ".") for option_id in option_text}
            self.assertEqual(public_setting_ids, translated_ids, language)
            for option_id, text in option_text.items():
                self.assertEqual({"label", "description"}, set(text), option_id)
                self.assertTrue(text["label"], option_id)
                self.assertTrue(text["description"], option_id)

    def test_tb2_certificate_labels_have_no_redundant_suffix(self) -> None:
        for language in FULL_LANGUAGES:
            option_text = self.translations[language]["settings"]["optionText"]
            for option_id, text in option_text.items():
                is_tb2_certificate = (
                    option_id == "core__certdir_tb2"
                    or option_id.startswith("core__server_cert_tb2__")
                    or option_id.startswith("core__client_cert_tb2__")
                )
                if is_tb2_certificate:
                    self.assertNotRegex(
                        f'{text["label"]} {text["description"]}', r"\(TB2\)"
                    )

    def test_new_status_texts_are_localized(self) -> None:
        english = self.translations["en"]["server"]
        for language in FULL_LANGUAGES[1:]:
            localized = self.translations[language]["server"]
            for section in (
                "tb2HttpsMode",
                "tb2HttpsStatus",
                "mqttUpstreamStatus",
            ):
                for key, english_text in english[section].items():
                    self.assertNotEqual(
                        english_text,
                        localized[section][key],
                        f"{language}: server.{section}.{key}",
                    )

    def test_bundled_translations_match_the_webui_sources(self) -> None:
        for language in ALL_LANGUAGES:
            source = TRANSLATION_DIR / f"{language}.json"
            bundled = BUNDLE_TRANSLATION_DIR / f"{language}.json"
            self.assertEqual(
                json.loads(source.read_text(encoding="utf-8")),
                json.loads(bundled.read_text(encoding="utf-8")),
                language,
            )

    def test_bundle_excludes_the_dev_banner_and_references_an_existing_asset(self) -> None:
        index = (BUNDLE_DIR / "index.html").read_text(encoding="utf-8")
        self.assertNotIn('id="tc-dev-environment-banner"', index)
        self.assertNotIn("tc-dev-banner-height", index)
        asset_match = re.search(r'src="/web/(assets/index-[^"]+\.js)"', index)
        self.assertIsNotNone(asset_match)
        self.assertTrue((BUNDLE_DIR / asset_match.group(1)).is_file())


if __name__ == "__main__":
    unittest.main()
