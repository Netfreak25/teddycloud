#!/usr/bin/env python3
"""Focused behavioral tests for the manual TB2 cache/library doctor."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def damaged_collection(self, content_hash: str) -> Path:
        collection = self.library / "by/contentHash" / content_hash
        collection.mkdir(parents=True)
        self.write_json(collection / "library-entry.json", {"_version": 5})
        return collection

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

    def test_audio_manifest_is_reconstructed_from_canonical_archive(self) -> None:
        payloads = [b"OggS-first", b"OggS-second"]
        chapters = []
        for index, payload in enumerate(payloads):
            digest = hashlib.sha256(payload).hexdigest()
            chapters.append(
                {
                    "index": index,
                    "originalName": f"Chapter {index + 1}",
                    "sha256": digest,
                    "fileSize": len(payload),
                    "path": f"chapters/teddycloud_{digest}_{index:02d}.opus",
                }
            )
        content_hash = DOCTOR.audio_collection_hash(chapters)
        collection = self.damaged_collection(content_hash)
        (collection / "chapters").mkdir()
        for chapter, payload in zip(chapters, payloads):
            (collection / chapter["path"]).write_bytes(payload)

        result, repairable = DOCTOR.diagnose_collection(collection, {}, False)
        self.assertTrue(repairable)
        self.assertIn("audio reconstructed from archive", result)

        before = [(collection / chapter["path"]).read_bytes() for chapter in chapters]
        result, repaired = DOCTOR.diagnose_collection(collection, {}, True)
        self.assertTrue(repaired)
        self.assertIn("origins=[]", result)
        entry = json.loads((collection / "library-entry.json").read_text())
        self.assertTrue(entry["recovered"])
        self.assertEqual(entry["origins"], [])
        self.assertEqual(
            [chapter["originalName"] for chapter in entry["chapters"]],
            ["Chapter 1", "Chapter 2"],
        )
        self.assertEqual(
            [(collection / chapter["path"]).read_bytes() for chapter in chapters],
            before,
        )

    def test_audio_reconstruction_rejects_non_contiguous_indices(self) -> None:
        payload = b"OggS-gap"
        digest = hashlib.sha256(payload).hexdigest()
        collection = self.damaged_collection("a" * 64)
        (collection / "chapters").mkdir()
        (collection / f"chapters/teddycloud_{digest}_01.opus").write_bytes(
            payload
        )

        result, repairable = DOCTOR.diagnose_collection(collection, {}, False)
        self.assertFalse(repairable)
        self.assertIn("indices are not contiguous", result)
        self.assertEqual(
            json.loads((collection / "library-entry.json").read_text()),
            {"_version": 5},
        )

    def test_audio_reconstruction_rejects_filename_hash_mismatch(self) -> None:
        payload = b"OggS-hash-mismatch"
        collection = self.damaged_collection("b" * 64)
        (collection / "chapters").mkdir()
        (collection / f"chapters/teddycloud_{'0' * 64}_00.opus").write_bytes(
            payload
        )

        result, repairable = DOCTOR.diagnose_collection(collection, {}, False)
        self.assertFalse(repairable)
        self.assertIn("filename SHA-256 differs", result)

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

    def test_tonieplay_file_restore_is_explicit_and_fully_backed_up(self) -> None:
        ruid = "47062C7F080104E0"
        generation = self.generation(4, ruid, 9188984)
        game = b"MPY-game-object"
        manifest = json.dumps(
            {
                "contentType": "tonieplay",
                "version": 9188984,
                "content": [
                    {
                        "name": "main.mpy",
                        "type": "mpy",
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
                        "fileSize": len(game),
                        "contentType": "application/octet-stream",
                    }
                ],
            },
        )
        cache_index = DOCTOR.build_cache_index(self.cache)
        content_hash, matches = next(iter(cache_index.items()))
        item = matches[0]["objects"][0]
        collection = self.damaged_collection(content_hash)
        (collection / "objects").mkdir()
        (collection / "content-meta.json").write_bytes(b"wrong manifest")
        (collection / item["path"]).write_bytes(b"wrong object")
        (collection / "objects/unexpected.bin").write_bytes(b"extra")

        result, repairable = DOCTOR.diagnose_collection(
            collection, cache_index, False
        )
        self.assertTrue(repairable)
        for marker in (
            "content-meta.json differs",
            "object count differs",
            "unexpected object",
            "object size differs",
            "object SHA-256 differs",
        ):
            self.assertIn(marker, result)

        result, repaired = DOCTOR.diagnose_collection(
            collection, cache_index, True
        )
        self.assertFalse(repaired)
        self.assertIn("use --apply --restore-files", result)
        self.assertEqual(
            (collection / "content-meta.json").read_bytes(), b"wrong manifest"
        )

        result, restored = DOCTOR.diagnose_collection(
            collection, cache_index, True, restore_files=True
        )
        self.assertTrue(restored)
        self.assertIn("restored files: Tonieplay", result)
        self.assertEqual((collection / "content-meta.json").read_bytes(), manifest)
        self.assertEqual((collection / item["path"]).read_bytes(), game)
        self.assertFalse((collection / "objects/unexpected.bin").exists())
        backups = list((self.library / ".doctor-backups").glob("*/*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(
            (backups[0] / "content-meta.json").read_bytes(), b"wrong manifest"
        )
        self.assertEqual(
            (backups[0] / "objects/unexpected.bin").read_bytes(), b"extra"
        )

    def test_failed_atomic_restore_puts_original_collection_back(self) -> None:
        payload = b"OggS-cache-copy"
        digest = hashlib.sha256(payload).hexdigest()
        chapter = {
            "index": 0,
            "originalName": "Recovered chapter",
            "sha256": digest,
            "fileSize": len(payload),
            "path": f"chapters/teddycloud_{digest}_00.opus",
        }
        content_hash = DOCTOR.audio_collection_hash([chapter])
        collection = self.damaged_collection(content_hash)
        (collection / "original-marker.txt").write_text("keep", encoding="utf-8")
        cache_file = self.root / "cache-object.opus"
        cache_file.write_bytes(payload)
        generation = {
            "kind": "audio",
            "contentHash": content_hash,
            "chapters": [chapter],
            "cachePaths": [cache_file],
        }
        origin = {
            "overlay": 3,
            "ruid": "B9DB7029500304E0",
            "contentVersion": 1,
        }
        real_replace = os.replace

        def fail_final_swap(source: object, target: object) -> None:
            source_path = Path(source)
            target_path = Path(target)
            if (
                source_path.parent.name == ".doctor-staging"
                and source_path.name.startswith(f"{content_hash}-")
                and "-original-" not in source_path.name
                and target_path == collection
            ):
                raise OSError("simulated final directory swap failure")
            real_replace(source, target)

        with mock.patch.object(DOCTOR.os, "replace", side_effect=fail_final_swap):
            with self.assertRaisesRegex(OSError, "simulated final"):
                DOCTOR.restore_collection_files(collection, generation, [origin])

        self.assertTrue(collection.is_dir())
        self.assertEqual(
            (collection / "original-marker.txt").read_text(encoding="utf-8"),
            "keep",
        )
        self.assertEqual(
            json.loads((collection / "library-entry.json").read_text()),
            {"_version": 5},
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
