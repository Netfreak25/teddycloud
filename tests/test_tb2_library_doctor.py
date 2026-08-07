#!/usr/bin/env python3
"""Focused behavioral tests for the manual TB2 cache/library doctor."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "contrib/repair_tb2_native_library.py"
SPEC = importlib.util.spec_from_file_location("tb2_library_doctor", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
DOCTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DOCTOR)


class Tb2LibraryDoctorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.library = self.root / "library"
        (self.library / "by/contentHash").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def generation(self, overlay: int, ruid: str, version: int) -> Path:
        path = self.cache / "v3-native/versions" / str(overlay) / ruid / str(version)
        (path / "chapters").mkdir(parents=True)
        return path

    @staticmethod
    def write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")

    def test_audio_manifest_is_repaired_only_after_exact_cache_match(self) -> None:
        ruid = "B9DB7029500304E0"
        generation = self.generation(3, ruid, 1763979960)
        audio = b"OggS-test-audio"
        (generation / "chapters/original.opus").write_bytes(audio)
        self.write_json(
            generation / "descriptor.json",
            {
                "schemaVersion": 2,
                "overlay": 3,
                "ruid": ruid,
                "version": 1763979960,
                "contentType": "audio",
                "objects": [
                    {
                        "name": "original.opus",
                        "type": "audio",
                        "filename": "",
                        "fileSize": len(audio),
                        "contentType": "audio/ogg",
                    }
                ],
            },
        )

        cache_index = DOCTOR.build_cache_index(self.cache)
        content_hash, matches = next(iter(cache_index.items()))
        chapter = matches[0]["chapters"][0]
        collection = self.library / "by/contentHash" / content_hash
        (collection / "chapters").mkdir(parents=True)
        (collection / chapter["path"]).write_bytes(audio)
        self.write_json(collection / "library-entry.json", {"_version": 5})

        result, repairable = DOCTOR.diagnose_collection(collection, cache_index, False)
        self.assertTrue(repairable)
        self.assertIn("would repair: audio", result)
        self.assertEqual(
            json.loads((collection / "library-entry.json").read_text()),
            {"_version": 5},
        )

        result, repaired = DOCTOR.diagnose_collection(collection, cache_index, True)
        self.assertTrue(repaired)
        self.assertIn("repaired: audio", result)
        entry = json.loads((collection / "library-entry.json").read_text())
        self.assertEqual(entry["schemaVersion"], 2)
        self.assertEqual(entry["origins"][0]["ruid"], ruid)
        self.assertTrue(
            (collection / "library-entry.json.before-browser-repair.bak").is_file()
        )

    def test_tonieplay_manifest_and_objects_are_reconstructed_from_cache(self) -> None:
        ruid = "47062C7F080104E0"
        generation = self.generation(4, ruid, 9188984)
        game = b"MPY-game-object"
        manifest = json.dumps(
            {
                "contentType": "tonieplay",
                "version": 9188984,
                "title": "Test game",
                "content": [
                    {
                        "name": "main.mpy",
                        "auth": "opaque-auth-value",
                        "type": "mpy",
                        "filename": "main.mpy",
                        "fileSize": len(game),
                    }
                ],
            },
            separators=(",", ":"),
        ).encode()
        (generation / "manifest.json").write_bytes(manifest)
        (generation / "chapters/main.mpy").write_bytes(game)
        self.write_json(
            generation / "descriptor.json",
            {
                "schemaVersion": 2,
                "overlay": 4,
                "ruid": ruid,
                "version": 9188984,
                "contentType": "tonieplay",
                "objects": [
                    {
                        "name": "main.mpy",
                        "type": "mpy",
                        "filename": "main.mpy",
                        "fileSize": len(game),
                        "contentType": "application/octet-stream",
                    }
                ],
            },
        )

        cache_index = DOCTOR.build_cache_index(self.cache)
        content_hash, matches = next(iter(cache_index.items()))
        item = matches[0]["objects"][0]
        collection = self.library / "by/contentHash" / content_hash
        (collection / "objects").mkdir(parents=True)
        (collection / "content-meta.json").write_bytes(manifest)
        (collection / item["path"]).write_bytes(game)
        self.write_json(collection / "library-entry.json", {"_version": 5})

        result, repaired = DOCTOR.diagnose_collection(collection, cache_index, True)
        self.assertTrue(repaired)
        self.assertIn("repaired: Tonieplay", result)
        entry = json.loads((collection / "library-entry.json").read_text())
        self.assertEqual(entry["schemaVersion"], 3)
        self.assertEqual(entry["format"], "tonieplay-v3")
        self.assertEqual(entry["objects"][0]["name"], "main.mpy")
        self.assertEqual((collection / "content-meta.json").read_bytes(), manifest)


if __name__ == "__main__":
    unittest.main(verbosity=2)
