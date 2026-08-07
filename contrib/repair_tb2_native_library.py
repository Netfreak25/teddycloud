#!/usr/bin/env python3
"""Repair TB2 audio library manifests overwritten by the file browser bug.

The command is dry-run by default. It only repairs a collection when an intact
V3 cache generation reproduces the collection hash and every archived chapter.
Tonieplay collections are deliberately not reconstructed because their raw
manifest and auth values cannot be inferred safely from object files alone.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path
from typing import Any


COLLECTION_HASH_DOMAIN = b"TeddyCloud TB2 library collection v1"
HASH_LENGTH = 64
CONTENT_JSON_VERSION = 5


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def collection_hash(chapters: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    digest.update(COLLECTION_HASH_DOMAIN)
    digest.update(struct.pack(">I", len(chapters)))
    for index, chapter in enumerate(chapters):
        digest.update(struct.pack(">I", index))
        digest.update(struct.pack(">I", chapter["fileSize"]))
        digest.update(bytes.fromhex(chapter["sha256"]))
    return digest.hexdigest()


def read_json(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def is_browser_overwrite(value: dict[str, Any] | None) -> bool:
    return bool(
        value
        and value.get("_version") == CONTENT_JSON_VERSION
        and "schemaVersion" not in value
        and "format" not in value
        and "chapters" not in value
        and "objects" not in value
    )


def load_audio_generation(generation_dir: Path) -> tuple[list[dict[str, Any]], dict[str, Any]] | None:
    descriptor = read_json(generation_dir / "descriptor.json")
    objects = descriptor.get("objects") if descriptor else None
    if not isinstance(objects, list) or not objects:
        return None

    chapters: list[dict[str, Any]] = []
    for index, item in enumerate(objects):
        if not isinstance(item, dict) or item.get("type") != "audio":
            return None
        name = item.get("name")
        expected_size = item.get("fileSize")
        if not isinstance(name, str) or not isinstance(expected_size, int) or expected_size <= 0:
            return None
        source = generation_dir / "chapters" / name
        if not source.is_file():
            return None
        digest, size = sha256_file(source)
        if size != expected_size:
            return None
        chapters.append(
            {
                "index": index,
                "originalName": name,
                "sha256": digest,
                "fileSize": size,
                "path": f"chapters/teddycloud_{digest}_{index:02d}.opus",
            }
        )
    return chapters, descriptor


def find_matching_generations(cache_root: Path, wanted_hash: str) -> list[tuple[list[dict[str, Any]], dict[str, Any]]]:
    versions = cache_root / "v3-native" / "versions"
    matches: list[tuple[list[dict[str, Any]], dict[str, Any]]] = []
    if not versions.is_dir():
        return matches
    for descriptor_path in versions.glob("*/*/*/descriptor.json"):
        generation = load_audio_generation(descriptor_path.parent)
        if generation and collection_hash(generation[0]) == wanted_hash:
            matches.append(generation)
    return matches


def archived_chapters_match(collection_dir: Path, chapters: list[dict[str, Any]]) -> bool:
    chapter_dir = collection_dir / "chapters"
    expected_names = {Path(chapter["path"]).name for chapter in chapters}
    actual_names = {path.name for path in chapter_dir.iterdir() if path.is_file()} if chapter_dir.is_dir() else set()
    if actual_names != expected_names:
        return False
    for chapter in chapters:
        path = collection_dir / chapter["path"]
        digest, size = sha256_file(path)
        if digest != chapter["sha256"] or size != chapter["fileSize"]:
            return False
    return True


def repair_collection(collection_dir: Path, cache_root: Path, apply: bool) -> str:
    content_hash = collection_dir.name
    if len(content_hash) != HASH_LENGTH or any(char not in "0123456789abcdef" for char in content_hash):
        return "skip: directory name is not a canonical content hash"

    entry_path = collection_dir / "library-entry.json"
    if not is_browser_overwrite(read_json(entry_path)):
        return "skip: manifest is not the recognized browser-overwrite damage"
    if (collection_dir / "objects").is_dir():
        return "manual recache required: Tonieplay metadata cannot be reconstructed safely"

    matches = find_matching_generations(cache_root, content_hash)
    if not matches:
        return "manual recache required: no matching intact V3 audio generation found"

    chapters = matches[0][0]
    if not archived_chapters_match(collection_dir, chapters):
        return "skip: archived chapters do not exactly match the intact V3 generation"

    origins: list[dict[str, Any]] = []
    for _, descriptor in matches:
        origin = {
            "overlay": descriptor.get("overlay"),
            "ruid": descriptor.get("ruid"),
            "contentVersion": descriptor.get("version"),
        }
        if (
            isinstance(origin["overlay"], int)
            and isinstance(origin["ruid"], str)
            and isinstance(origin["contentVersion"], int)
            and origin not in origins
        ):
            origins.append(origin)
    if not origins:
        return "skip: matching generation has no valid origin metadata"

    repaired = {
        "schemaVersion": 2,
        "origin": "tonies",
        "boxGeneration": "tb2",
        "format": "ogg-opus",
        "contentHash": content_hash,
        "chapters": chapters,
        "origins": origins,
    }
    if not apply:
        return f"would repair: {len(chapters)} chapters, {len(origins)} origin(s)"

    backup = collection_dir / "library-entry.json.before-browser-repair.bak"
    if not backup.exists():
        shutil.copy2(entry_path, backup)
    temporary = collection_dir / f".library-entry.json.repair-{os.getpid()}.tmp"
    temporary.write_text(json.dumps(repaired, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    os.replace(temporary, entry_path)
    return f"repaired: {len(chapters)} chapters, backup={backup.name}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library-root", type=Path, required=True)
    parser.add_argument("--cache-root", type=Path, required=True)
    parser.add_argument("--content-hash", action="append", default=[])
    parser.add_argument("--apply", action="store_true", help="write repairs; otherwise dry-run")
    args = parser.parse_args()

    collection_root = args.library_root.resolve() / "by" / "contentHash"
    cache_root = args.cache_root.resolve()
    if not collection_root.is_dir() or not cache_root.is_dir():
        parser.error("library or cache root does not exist")

    requested = set(args.content_hash)
    candidates = sorted(path for path in collection_root.iterdir() if path.is_dir())
    if requested:
        candidates = [path for path in candidates if path.name in requested]
    if not candidates:
        print("No matching collection directories found.")
        return 1

    repaired = 0
    for collection_dir in candidates:
        result = repair_collection(collection_dir, cache_root, args.apply)
        print(f"{collection_dir.name}: {result}")
        repaired += int(result.startswith("repaired:") or result.startswith("would repair:"))
    return 0 if repaired else 2


if __name__ == "__main__":
    sys.exit(main())
