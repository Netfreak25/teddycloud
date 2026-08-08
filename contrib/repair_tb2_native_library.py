#!/usr/bin/env python3
"""Diagnose and repair TB2 native cache/library collections.

The doctor is read-only by default. Audio metadata can be reconstructed from
content-addressed chapter files alone. Replacing damaged collection files is a
separate, explicit operation and requires an exact TeddyCloud V3 cache match.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import ssl
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

AUDIO_HASH_DOMAIN = b"TeddyCloud TB2 library collection v1"
TONIEPLAY_HASH_DOMAIN = b"TeddyCloud TB2 Tonieplay collection v1"
HASH_LENGTH = 64
CONTENT_JSON_VERSION = 5
AUDIO_LIBRARY_SCHEMA = 2
TONIEPLAY_LIBRARY_SCHEMA = 3
TB2_GENERATION = "2"
METADATA_KEYS = (
    "gameId",
    "tonieplayEngineVersion",
    "language",
    "tonieSalesId",
    "title",
    "name",
)
AUDIO_CHAPTER_PATTERN = re.compile(
    r"^teddycloud_([0-9a-f]{64})_([0-9]+)\.opus$"
)
ENTRY_BACKUP_NAME = "library-entry.json.before-browser-repair.bak"


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def hash_u32(digest: Any, value: int) -> None:
    digest.update(struct.pack(">I", value))


def audio_collection_hash(chapters: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    digest.update(AUDIO_HASH_DOMAIN)
    hash_u32(digest, len(chapters))
    for index, chapter in enumerate(chapters):
        hash_u32(digest, index)
        hash_u32(digest, chapter["fileSize"])
        digest.update(bytes.fromhex(chapter["sha256"]))
    return digest.hexdigest()


def tonieplay_collection_hash(manifest: bytes, objects: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    digest.update(TONIEPLAY_HASH_DOMAIN)
    hash_u32(digest, len(manifest))
    digest.update(manifest)
    hash_u32(digest, len(objects))
    for index, item in enumerate(objects):
        hash_u32(digest, index)
        hash_u32(digest, item["fileSize"])
        digest.update(bytes.fromhex(item["sha256"]))
    return digest.hexdigest()


def read_json(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def is_canonical_hash(value: str) -> bool:
    return len(value) == HASH_LENGTH and all(
        character in "0123456789abcdef" for character in value
    )


def is_browser_overwrite(value: dict[str, Any] | None) -> bool:
    return bool(
        value
        and value.get("_version") == CONTENT_JSON_VERSION
        and "schemaVersion" not in value
        and "format" not in value
        and "chapters" not in value
        and "objects" not in value
    )


def cache_origin(descriptor: dict[str, Any]) -> dict[str, Any] | None:
    overlay = descriptor.get("overlay")
    ruid = descriptor.get("ruid")
    version = descriptor.get("version")
    if (
        isinstance(overlay, int)
        and isinstance(ruid, str)
        and len(ruid) == 16
        and all(character in "0123456789abcdefABCDEF" for character in ruid)
        and isinstance(version, int)
        and 0 <= version <= 0xFFFFFFFF
    ):
        return {
            "overlay": overlay,
            "ruid": ruid.upper(),
            "contentVersion": version,
        }
    return None


def load_cache_generation(generation_dir: Path) -> dict[str, Any] | None:
    descriptor = read_json(generation_dir / "descriptor.json")
    entries = descriptor.get("objects") if descriptor else None
    origin = cache_origin(descriptor) if descriptor else None
    if not isinstance(entries, list) or not entries or origin is None:
        return None

    cached_objects: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            return None
        name = entry.get("name")
        expected_size = entry.get("fileSize")
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(expected_size, int)
            or expected_size <= 0
        ):
            return None
        source = generation_dir / "chapters" / name
        if not source.is_file():
            return None
        file_hash, actual_size = sha256_file(source)
        if actual_size != expected_size:
            return None
        cached_objects.append(
            {
                "index": index,
                "name": name,
                "type": entry.get("type") if isinstance(entry.get("type"), str) else "",
                "filename": (
                    entry.get("filename")
                    if isinstance(entry.get("filename"), str)
                    else ""
                ),
                "contentType": (
                    entry.get("contentType")
                    if isinstance(entry.get("contentType"), str)
                    else ""
                ),
                "sha256": file_hash,
                "fileSize": actual_size,
                "cachePath": source,
            }
        )

    all_audio = all(item["type"] == "audio" for item in cached_objects)
    is_tonieplay = descriptor.get("contentType") == "tonieplay" or not all_audio
    if not is_tonieplay:
        chapters = [
            {
                "index": item["index"],
                "originalName": item["name"],
                "sha256": item["sha256"],
                "fileSize": item["fileSize"],
                "path": (
                    f"chapters/teddycloud_{item['sha256']}_{item['index']:02d}.opus"
                ),
            }
            for item in cached_objects
        ]
        return {
            "kind": "audio",
            "contentHash": audio_collection_hash(chapters),
            "origin": origin,
            "chapters": chapters,
            "cachePaths": [item["cachePath"] for item in cached_objects],
        }

    manifest_path = generation_dir / "manifest.json"
    try:
        manifest = manifest_path.read_bytes()
        manifest_json = json.loads(manifest.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    content = manifest_json.get("content") if isinstance(manifest_json, dict) else None
    if not isinstance(content, list) or len(content) != len(cached_objects):
        return None

    objects: list[dict[str, Any]] = []
    for index, (raw, cached) in enumerate(zip(content, cached_objects)):
        if (
            not isinstance(raw, dict)
            or raw.get("name") != cached["name"]
            or raw.get("fileSize") != cached["fileSize"]
        ):
            return None
        item_type = raw.get("type") if isinstance(raw.get("type"), str) else ""
        filename = raw.get("filename") if isinstance(raw.get("filename"), str) else ""
        item = {
            "index": index,
            "name": cached["name"],
            "sha256": cached["sha256"],
            "fileSize": cached["fileSize"],
            "contentType": cached["contentType"]
            or ("audio/ogg" if item_type == "audio" else "application/octet-stream"),
            "path": f"objects/{index}-{cached['sha256']}.bin",
        }
        if item_type:
            item["type"] = item_type
        if filename:
            item["filename"] = filename
        objects.append(item)

    return {
        "kind": "tonieplay",
        "contentHash": tonieplay_collection_hash(manifest, objects),
        "origin": origin,
        "version": origin["contentVersion"],
        "contentType": (
            descriptor.get("contentType")
            if isinstance(descriptor.get("contentType"), str)
            else "tonieplay"
        ),
        "manifest": manifest,
        "manifestJson": manifest_json,
        "objects": objects,
        "cachePaths": [item["cachePath"] for item in cached_objects],
    }


def build_cache_index(cache_root: Path) -> dict[str, list[dict[str, Any]]]:
    versions = cache_root / "v3-native" / "versions"
    index: dict[str, list[dict[str, Any]]] = {}
    if not versions.is_dir():
        return index
    for descriptor_path in versions.glob("*/*/*/descriptor.json"):
        generation = load_cache_generation(descriptor_path.parent)
        if generation is not None:
            index.setdefault(generation["contentHash"], []).append(generation)
    return index


def origins_for(matches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    origins: list[dict[str, Any]] = []
    for match in matches:
        origin = match["origin"]
        if origin not in origins:
            origins.append(origin)
    return origins


def tag_uid(ruid: str) -> str:
    octets = [ruid[index : index + 2] for index in range(0, len(ruid), 2)]
    return ":".join(reversed(octets))


def format_origins(origins: list[dict[str, Any]]) -> str:
    return "; ".join(
        "overlay={overlay} RUID={ruid} Tag-UID={tag_uid} version={version}".format(
            overlay=origin["overlay"],
            ruid=origin["ruid"],
            tag_uid=tag_uid(origin["ruid"]),
            version=origin["contentVersion"],
        )
        for origin in origins
    )


def archived_audio_differences(
    collection_dir: Path, chapters: list[dict[str, Any]]
) -> list[str]:
    differences: list[str] = []
    chapter_dir = collection_dir / "chapters"
    if not chapter_dir.is_dir():
        return ["chapters directory is missing"]
    expected = {Path(chapter["path"]).name for chapter in chapters}
    actual = {path.name for path in chapter_dir.iterdir() if path.is_file()}
    for name in sorted(expected - actual):
        differences.append(f"chapter is missing: {name}")
    for name in sorted(actual - expected):
        differences.append(f"unexpected chapter: {name}")
    for chapter in chapters:
        path = collection_dir / chapter["path"]
        if not path.is_file():
            continue
        file_hash, size = sha256_file(path)
        if size != chapter["fileSize"]:
            differences.append(
                f"chapter size differs: {path.name} "
                f"expected={chapter['fileSize']} actual={size}"
            )
        if file_hash != chapter["sha256"]:
            differences.append(f"chapter SHA-256 differs: {path.name}")
    return differences


def archived_audio_matches(
    collection_dir: Path, chapters: list[dict[str, Any]]
) -> bool:
    return not archived_audio_differences(collection_dir, chapters)


def archived_tonieplay_differences(
    collection_dir: Path, generation: dict[str, Any]
) -> list[str]:
    differences: list[str] = []
    manifest_path = collection_dir / "content-meta.json"
    try:
        manifest = manifest_path.read_bytes()
    except OSError:
        differences.append("content-meta.json is missing or unreadable")
    else:
        if manifest != generation["manifest"]:
            differences.append(
                "content-meta.json differs "
                f"expected_sha256={hashlib.sha256(generation['manifest']).hexdigest()} "
                f"actual_sha256={hashlib.sha256(manifest).hexdigest()}"
            )
    object_dir = collection_dir / "objects"
    if not object_dir.is_dir():
        return differences + ["objects directory is missing"]
    expected = {Path(item["path"]).name for item in generation["objects"]}
    actual = {path.name for path in object_dir.iterdir() if path.is_file()}
    if len(actual) != len(expected):
        differences.append(
            f"object count differs: expected={len(expected)} actual={len(actual)}"
        )
    for name in sorted(expected - actual):
        differences.append(f"object is missing: {name}")
    for name in sorted(actual - expected):
        differences.append(f"unexpected object: {name}")
    for item in generation["objects"]:
        path = collection_dir / item["path"]
        if not path.is_file():
            continue
        file_hash, size = sha256_file(path)
        if size != item["fileSize"]:
            differences.append(
                f"object size differs: {path.name} "
                f"expected={item['fileSize']} actual={size}"
            )
        if file_hash != item["sha256"]:
            differences.append(f"object SHA-256 differs: {path.name}")
    return differences


def archived_tonieplay_matches(
    collection_dir: Path, generation: dict[str, Any]
) -> bool:
    return not archived_tonieplay_differences(collection_dir, generation)


def build_audio_entry(
    content_hash: str,
    generation: dict[str, Any],
    origins: list[dict[str, Any]],
    recovered: bool = False,
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "schemaVersion": AUDIO_LIBRARY_SCHEMA,
        "origin": "tonies",
        "boxGeneration": "tb2",
        "format": "ogg-opus",
        "contentHash": content_hash,
        "chapters": generation["chapters"],
        "origins": origins,
    }
    if recovered:
        entry["recovered"] = True
    return entry


def build_tonieplay_entry(
    content_hash: str, generation: dict[str, Any], origins: list[dict[str, Any]]
) -> dict[str, Any]:
    manifest = generation["manifest"]
    raw = generation["manifestJson"]
    metadata = {key: raw[key] for key in METADATA_KEYS if key in raw}
    entry: dict[str, Any] = {
        "schemaVersion": TONIEPLAY_LIBRARY_SCHEMA,
        "origin": "tonies",
        "boxGeneration": "tb2",
        "format": "tonieplay-v3",
        "contentHash": content_hash,
        "contentType": generation["contentType"],
        "contentVersion": generation["version"],
        "manifest": {
            "path": "content-meta.json",
            "sha256": hashlib.sha256(manifest).hexdigest(),
            "fileSize": len(manifest),
        },
        "objects": generation["objects"],
        "origins": origins,
    }
    if metadata:
        entry["metadata"] = metadata
    return entry


def valid_origins(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        return []
    origins: list[dict[str, Any]] = []
    for item in value:
        if not isinstance(item, dict):
            return []
        overlay = item.get("overlay")
        ruid = item.get("ruid")
        version = item.get("contentVersion")
        if (
            not isinstance(overlay, int)
            or overlay < 0
            or not isinstance(ruid, str)
            or len(ruid) != 16
            or any(character not in "0123456789abcdefABCDEF" for character in ruid)
            or not isinstance(version, int)
            or not 0 <= version <= 0xFFFFFFFF
        ):
            return []
        origins.append(
            {
                "overlay": overlay,
                "ruid": ruid.upper(),
                "contentVersion": version,
            }
        )
    return origins


def recover_audio_metadata(
    collection_dir: Path, chapters: list[dict[str, Any]]
) -> tuple[list[str], list[dict[str, Any]]]:
    backup = read_json(collection_dir / ENTRY_BACKUP_NAME)
    entries = backup.get("chapters") if backup else None
    if (
        not isinstance(backup, dict)
        or backup.get("schemaVersion") != AUDIO_LIBRARY_SCHEMA
        or backup.get("boxGeneration") != "tb2"
        or backup.get("format") != "ogg-opus"
        or backup.get("contentHash") != collection_dir.name
        or not isinstance(entries, list)
        or len(entries) != len(chapters)
    ):
        return [], []

    names: list[str] = []
    for index, (entry, chapter) in enumerate(zip(entries, chapters)):
        expected_path = chapter["path"]
        if (
            not isinstance(entry, dict)
            or entry.get("index") != index
            or entry.get("sha256") != chapter["sha256"]
            or entry.get("fileSize") != chapter["fileSize"]
            or entry.get("path") != expected_path
            or not isinstance(entry.get("originalName"), str)
            or not entry["originalName"]
        ):
            return [], []
        names.append(entry["originalName"])
    return names, valid_origins(backup.get("origins"))


def reconstruct_archived_audio(
    collection_dir: Path,
) -> tuple[dict[str, Any] | None, list[dict[str, Any]], str]:
    chapter_dir = collection_dir / "chapters"
    if not chapter_dir.is_dir():
        return None, [], "chapters directory is missing"

    parsed: list[tuple[int, str, Path]] = []
    for path in sorted(chapter_dir.iterdir()):
        if not path.is_file():
            return None, [], f"unexpected chapter entry: {path.name}"
        match = AUDIO_CHAPTER_PATTERN.fullmatch(path.name)
        if match is None:
            return None, [], f"unexpected chapter filename: {path.name}"
        embedded_hash, index_text = match.groups()
        index = int(index_text)
        if index_text != f"{index:02d}":
            return None, [], f"non-canonical chapter index: {path.name}"
        parsed.append((index, embedded_hash, path))
    if not parsed:
        return None, [], "chapters directory is empty"

    parsed.sort(key=lambda item: item[0])
    indices = [item[0] for item in parsed]
    if indices != list(range(len(parsed))):
        return None, [], "chapter indices are not contiguous from zero"

    chapters: list[dict[str, Any]] = []
    for index, embedded_hash, path in parsed:
        actual_hash, size = sha256_file(path)
        if actual_hash != embedded_hash:
            return None, [], f"chapter filename SHA-256 differs: {path.name}"
        expected_name = f"teddycloud_{actual_hash}_{index:02d}.opus"
        if path.name != expected_name:
            return None, [], f"non-canonical chapter filename: {path.name}"
        chapters.append(
            {
                "index": index,
                "originalName": f"Chapter {index + 1}",
                "sha256": actual_hash,
                "fileSize": size,
                "path": f"chapters/{expected_name}",
            }
        )

    computed_hash = audio_collection_hash(chapters)
    if computed_hash != collection_dir.name:
        return (
            None,
            [],
            "archived chapters calculate to a different collection hash "
            f"({computed_hash})",
        )

    names, origins = recover_audio_metadata(collection_dir, chapters)
    if names:
        for chapter, name in zip(chapters, names):
            chapter["originalName"] = name
    generation = {
        "kind": "audio",
        "contentHash": computed_hash,
        "chapters": chapters,
    }
    return generation, origins, ""


def generation_payload_signature(generation: dict[str, Any]) -> tuple[Any, ...]:
    if generation["kind"] == "audio":
        return (
            "audio",
            json.dumps(generation["chapters"], sort_keys=True),
        )
    return (
        "tonieplay",
        generation["manifest"],
        json.dumps(generation["objects"], sort_keys=True),
        generation["contentType"],
        generation["version"],
    )


def consistent_cache_generation(
    matches: list[dict[str, Any]],
) -> tuple[dict[str, Any] | None, str]:
    if not matches:
        return None, "no matching intact V3 generation found"
    kinds = {match["kind"] for match in matches}
    if len(kinds) != 1:
        return None, "cache hash resolves to conflicting content kinds"
    signature = generation_payload_signature(matches[0])
    if any(generation_payload_signature(match) != signature for match in matches[1:]):
        return None, "matching cache generations disagree on content or metadata"
    return matches[0], ""


def format_differences(differences: list[str]) -> str:
    return "; ".join(differences)


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(f".{path.name}.repair-{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    os.replace(temporary, path)


def write_repaired_entry(collection_dir: Path, repaired: dict[str, Any]) -> str:
    entry_path = collection_dir / "library-entry.json"
    backup = collection_dir / ENTRY_BACKUP_NAME
    if entry_path.exists() and not backup.exists():
        shutil.copy2(entry_path, backup)
    write_json_atomic(entry_path, repaired)
    return backup.name


def doctor_backup_path(library_root: Path, content_hash: str) -> Path:
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%S.%fZ"
    )
    return library_root / ".doctor-backups" / timestamp / content_hash


def restore_collection_files(
    collection_dir: Path,
    generation: dict[str, Any],
    origins: list[dict[str, Any]],
) -> Path:
    library_root = collection_dir.parents[2]
    stage_root = library_root / ".doctor-staging"
    stage = stage_root / f"{collection_dir.name}-{os.getpid()}"
    displaced = stage_root / f"{collection_dir.name}-original-{os.getpid()}"
    backup = doctor_backup_path(library_root, collection_dir.name)
    shutil.rmtree(stage, ignore_errors=True)
    shutil.rmtree(displaced, ignore_errors=True)
    stage.mkdir(parents=True)

    try:
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(collection_dir, backup)

        if generation["kind"] == "audio":
            chapter_dir = stage / "chapters"
            chapter_dir.mkdir()
            for source, chapter in zip(
                generation["cachePaths"], generation["chapters"]
            ):
                shutil.copy2(source, stage / chapter["path"])
            entry = build_audio_entry(
                collection_dir.name, generation, origins
            )
            differences = archived_audio_differences(
                stage, generation["chapters"]
            )
        else:
            object_dir = stage / "objects"
            object_dir.mkdir()
            (stage / "content-meta.json").write_bytes(generation["manifest"])
            for source, item in zip(
                generation["cachePaths"], generation["objects"]
            ):
                shutil.copy2(source, stage / item["path"])
            entry = build_tonieplay_entry(
                collection_dir.name, generation, origins
            )
            differences = archived_tonieplay_differences(stage, generation)
        if differences:
            raise OSError(
                "staged restore validation failed: "
                + format_differences(differences)
            )
        write_json_atomic(stage / "library-entry.json", entry)

        os.replace(collection_dir, displaced)
        try:
            os.replace(stage, collection_dir)
        except OSError:
            os.replace(displaced, collection_dir)
            raise
        shutil.rmtree(displaced, ignore_errors=True)
        return backup
    except Exception:
        shutil.rmtree(stage, ignore_errors=True)
        if displaced.exists() and not collection_dir.exists():
            os.replace(displaced, collection_dir)
        raise


def diagnose_collection(
    collection_dir: Path,
    cache_index: dict[str, list[dict[str, Any]]],
    apply: bool,
    restore_files: bool = False,
) -> tuple[str, bool]:
    content_hash = collection_dir.name
    if not is_canonical_hash(content_hash):
        return "skip: directory name is not a canonical content hash", False
    if not is_browser_overwrite(read_json(collection_dir / "library-entry.json")):
        return "skip: manifest is not the recognized browser-overwrite damage", False

    matches = cache_index.get(content_hash, [])
    generation, cache_error = consistent_cache_generation(matches)

    if generation is None and matches:
        return f"blocked: {cache_error}", False

    if generation is None:
        reconstructed, origins, reconstruction_error = reconstruct_archived_audio(
            collection_dir
        )
        if reconstructed is not None:
            repaired = build_audio_entry(
                content_hash,
                reconstructed,
                origins,
                recovered=not origins,
            )
            details = (
                f"audio reconstructed from archive, "
                f"{len(reconstructed['chapters'])} chapters"
            )
            origin_text = format_origins(origins)
            if not apply:
                return f"would repair: {details}, origins=[{origin_text}]", True
            backup = write_repaired_entry(collection_dir, repaired)
            return (
                f"repaired: {details}, origins=[{origin_text}], backup={backup}",
                True,
            )

        if (collection_dir / "chapters").exists():
            return (
                "blocked: archived audio cannot be reconstructed: "
                + reconstruction_error,
                False,
            )
        return f"recache required: {cache_error}", False

    origins = origins_for(matches)
    if not origins:
        return "blocked: matching cache has no valid origin metadata", False

    if generation["kind"] == "audio":
        differences = archived_audio_differences(
            collection_dir, generation["chapters"]
        )
        repaired = build_audio_entry(content_hash, generation, origins)
        details = f"audio, {len(generation['chapters'])} chapters"
    else:
        differences = archived_tonieplay_differences(collection_dir, generation)
        repaired = build_tonieplay_entry(content_hash, generation, origins)
        details = f"Tonieplay, {len(generation['objects'])} objects"

    if differences:
        difference_text = format_differences(differences)
        if not apply:
            return (
                f"would restore files: {details}, differences=[{difference_text}]; "
                "requires --apply --restore-files",
                True,
            )
        if not restore_files:
            return (
                f"blocked: collection files differ from exact cache: "
                f"{difference_text}; use --apply --restore-files",
                False,
            )
        backup = restore_collection_files(collection_dir, generation, origins)
        return (
            f"restored files: {details}, origins=[{format_origins(origins)}], "
            f"backup={backup}",
            True,
        )

    if not apply:
        return (
            f"would repair: {details}, origins=[{format_origins(origins)}]",
            True,
        )
    backup = write_repaired_entry(collection_dir, repaired)
    return (
        f"repaired: {details}, origins=[{format_origins(origins)}], backup={backup}",
        True,
    )


class TeddyCloudApi:
    """Small read/download-only client for the local TeddyCloud API."""

    def __init__(
        self,
        base_url: str,
        insecure: bool,
        headers: list[str],
        timeout: int,
    ) -> None:
        self.base_url = base_url.rstrip("/") + "/"
        self.timeout = timeout
        self.headers: dict[str, str] = {}
        for header in headers:
            name, separator, value = header.partition(":")
            if not separator or not name.strip():
                raise ValueError(f"invalid API header: {header!r}")
            self.headers[name.strip()] = value.strip()
        self.context = (
            ssl._create_unverified_context()
            if insecure
            else ssl.create_default_context()
        )

    def get(self, path: str) -> bytes:
        url = urllib.parse.urljoin(self.base_url, path.lstrip("/"))
        request = urllib.request.Request(url, headers=self.headers)
        with urllib.request.urlopen(
            request, context=self.context, timeout=self.timeout
        ) as response:
            return response.read()

    def get_json(self, path: str) -> dict[str, Any]:
        value = json.loads(self.get(path).decode("utf-8"))
        if not isinstance(value, dict):
            raise TypeError(f"API returned no object for {path}")
        return value

    def eligible_tb2_downloads(self) -> list[tuple[str, str, str]]:
        boxes = self.get_json("/api/getBoxes").get("boxes")
        if not isinstance(boxes, list):
            raise TypeError("/api/getBoxes returned no boxes array")
        downloads: list[tuple[str, str, str]] = []
        seen: set[str] = set()
        for box in boxes:
            overlay = box.get("ID") if isinstance(box, dict) else None
            if not isinstance(overlay, str) or not overlay:
                continue
            encoded = urllib.parse.quote(overlay, safe="")
            generation = (
                self.get(f"/api/settings/get/toniebox.boxGeneration?overlay={encoded}")
                .decode("utf-8", errors="replace")
                .strip()
            )
            if generation != TB2_GENERATION:
                continue
            tags = self.get_json(f"/api/getTagIndex?overlay={encoded}").get("tags")
            if not isinstance(tags, list):
                continue
            for tag in tags:
                if not isinstance(tag, dict):
                    continue
                path = tag.get("downloadTriggerUrl")
                ruid = tag.get("ruid")
                if (
                    isinstance(path, str)
                    and path
                    and path not in seen
                    and isinstance(ruid, str)
                ):
                    seen.add(path)
                    downloads.append((overlay, ruid.upper(), path))
        return downloads

    def trigger_download(self, path: str) -> dict[str, Any]:
        return self.get_json(path)


def unresolved_hashes(
    candidates: list[Path], cache_index: dict[str, list[dict[str, Any]]]
) -> set[str]:
    return {
        path.name
        for path in candidates
        if is_canonical_hash(path.name)
        and path.name not in cache_index
        and reconstruct_archived_audio(path)[0] is None
    }


def recache_until_resolved(
    api: TeddyCloudApi,
    cache_root: Path,
    candidates: list[Path],
) -> dict[str, list[dict[str, Any]]]:
    cache_index = build_cache_index(cache_root)
    remaining = unresolved_hashes(candidates, cache_index)
    if not remaining:
        print(
            "Recache: every damaged collection is locally repairable "
            "or already has an intact cache match."
        )
        return cache_index

    downloads = api.eligible_tb2_downloads()
    print(
        f"Recache: {len(remaining)} unresolved collection(s), "
        f"{len(downloads)} eligible TB2 original-content download(s)."
    )
    for overlay, ruid, path in downloads:
        print(f"Recache: requesting overlay={overlay} RUID={ruid}")
        try:
            result = api.trigger_download(path)
        except (OSError, TypeError, ValueError, urllib.error.URLError) as error:
            print(f"Recache: failed overlay={overlay} RUID={ruid}: {error}")
            continue
        if not result.get("success"):
            print(
                f"Recache: rejected overlay={overlay} RUID={ruid} "
                f"stage={result.get('stage', '')} message={result.get('message', '')}"
            )
            continue
        print(
            f"Recache: completed overlay={overlay} RUID={ruid} "
            f"objects={result.get('objectsCompleted', 0)}/"
            f"{result.get('objectsTotal', 0)}"
        )
        cache_index = build_cache_index(cache_root)
        remaining = unresolved_hashes(candidates, cache_index)
        if not remaining:
            print("Recache: all damaged collections now have intact cache matches.")
            break
    if remaining:
        print("Recache unresolved: " + ", ".join(sorted(remaining)))
    return cache_index


def select_candidates(collection_root: Path, requested: list[str]) -> list[Path]:
    invalid = [value for value in requested if not is_canonical_hash(value)]
    if invalid:
        raise ValueError("invalid content hash: " + ", ".join(invalid))
    candidates = sorted(path for path in collection_root.iterdir() if path.is_dir())
    if requested:
        selected = set(requested)
        candidates = [path for path in candidates if path.name in selected]
    return [
        path
        for path in candidates
        if is_browser_overwrite(read_json(path / "library-entry.json"))
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library-root", type=Path, required=True)
    parser.add_argument("--cache-root", type=Path, required=True)
    parser.add_argument("--content-hash", action="append", default=[])
    parser.add_argument(
        "--apply", action="store_true", help="write verified manifest repairs"
    )
    parser.add_argument(
        "--recache",
        action="store_true",
        help="ask TeddyCloud to download eligible original TB2 content",
    )
    parser.add_argument(
        "--restore-files",
        action="store_true",
        help="replace damaged collection files from one exact V3 cache match",
    )
    parser.add_argument("--api-url", default="https://127.0.0.1:8443")
    parser.add_argument(
        "--api-header",
        action="append",
        default=[],
        help="additional API header as 'Name: value'",
    )
    parser.add_argument("--insecure", action="store_true")
    parser.add_argument("--api-timeout", type=int, default=900)
    args = parser.parse_args()

    if args.recache and not args.apply:
        parser.error("--recache requires --apply because it changes the V3 cache")
    if args.restore_files and not args.apply:
        parser.error(
            "--restore-files requires --apply because it replaces collection files"
        )

    collection_root = args.library_root.resolve() / "by" / "contentHash"
    cache_root = args.cache_root.resolve()
    if not collection_root.is_dir() or not cache_root.is_dir():
        parser.error("library or cache root does not exist")

    try:
        candidates = select_candidates(collection_root, args.content_hash)
    except ValueError as error:
        parser.error(str(error))
    if not candidates:
        print("No damaged native TB2 collection manifests found.")
        return 0

    cache_index = build_cache_index(cache_root)
    if args.recache:
        try:
            api = TeddyCloudApi(
                args.api_url,
                args.insecure,
                args.api_header,
                args.api_timeout,
            )
            cache_index = recache_until_resolved(api, cache_root, candidates)
        except (OSError, TypeError, ValueError, urllib.error.URLError) as error:
            print(f"Recache setup failed: {error}", file=sys.stderr)
            return 3

    actionable = 0
    unresolved = 0
    for collection_dir in candidates:
        try:
            result, can_repair = diagnose_collection(
                collection_dir, cache_index, args.apply, args.restore_files
            )
        except OSError as error:
            result, can_repair = f"blocked: filesystem error: {error}", False
        print(f"{collection_dir.name}: {result}")
        actionable += int(can_repair)
        unresolved += int(not can_repair)

    print(
        f"Doctor summary: damaged={len(candidates)} "
        f"{'repaired' if args.apply else 'repairable'}={actionable} "
        f"unresolved={unresolved}"
    )
    return 0 if unresolved == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
