#!/usr/bin/env python3
"""Diagnose and repair TB2 native cache/library collections.

The doctor is read-only by default. It repairs only collections whose archived
bytes exactly reproduce an intact TeddyCloud V3 cache generation. With the
explicit ``--apply --recache`` combination it may ask the running TeddyCloud
server to download eligible original TB2 content, then repeats the same byte
validation before writing a repaired library manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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


def archived_audio_matches(
    collection_dir: Path, chapters: list[dict[str, Any]]
) -> bool:
    chapter_dir = collection_dir / "chapters"
    if not chapter_dir.is_dir():
        return False
    expected = {Path(chapter["path"]).name for chapter in chapters}
    actual = {path.name for path in chapter_dir.iterdir() if path.is_file()}
    if actual != expected:
        return False
    for chapter in chapters:
        path = collection_dir / chapter["path"]
        file_hash, size = sha256_file(path)
        if file_hash != chapter["sha256"] or size != chapter["fileSize"]:
            return False
    return True


def archived_tonieplay_matches(
    collection_dir: Path, generation: dict[str, Any]
) -> bool:
    manifest_path = collection_dir / "content-meta.json"
    try:
        if manifest_path.read_bytes() != generation["manifest"]:
            return False
    except OSError:
        return False
    object_dir = collection_dir / "objects"
    if not object_dir.is_dir():
        return False
    expected = {Path(item["path"]).name for item in generation["objects"]}
    actual = {path.name for path in object_dir.iterdir() if path.is_file()}
    if actual != expected:
        return False
    for item in generation["objects"]:
        file_hash, size = sha256_file(collection_dir / item["path"])
        if file_hash != item["sha256"] or size != item["fileSize"]:
            return False
    return True


def build_audio_entry(
    content_hash: str, generation: dict[str, Any], origins: list[dict[str, Any]]
) -> dict[str, Any]:
    return {
        "schemaVersion": AUDIO_LIBRARY_SCHEMA,
        "origin": "tonies",
        "boxGeneration": "tb2",
        "format": "ogg-opus",
        "contentHash": content_hash,
        "chapters": generation["chapters"],
        "origins": origins,
    }


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


def write_repaired_entry(collection_dir: Path, repaired: dict[str, Any]) -> str:
    entry_path = collection_dir / "library-entry.json"
    backup = collection_dir / "library-entry.json.before-browser-repair.bak"
    if not backup.exists():
        shutil.copy2(entry_path, backup)
    temporary = collection_dir / f".library-entry.json.repair-{os.getpid()}.tmp"
    temporary.write_text(
        json.dumps(repaired, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    os.replace(temporary, entry_path)
    return backup.name


def diagnose_collection(
    collection_dir: Path,
    cache_index: dict[str, list[dict[str, Any]]],
    apply: bool,
) -> tuple[str, bool]:
    content_hash = collection_dir.name
    if not is_canonical_hash(content_hash):
        return "skip: directory name is not a canonical content hash", False
    if not is_browser_overwrite(read_json(collection_dir / "library-entry.json")):
        return "skip: manifest is not the recognized browser-overwrite damage", False

    matches = cache_index.get(content_hash, [])
    if not matches:
        return "recache required: no matching intact V3 generation found", False
    kinds = {match["kind"] for match in matches}
    if len(kinds) != 1:
        return "blocked: cache hash resolves to conflicting content kinds", False
    generation = matches[0]
    if generation["kind"] == "audio" and any(
        match["chapters"] != generation["chapters"] for match in matches[1:]
    ):
        return "blocked: matching audio caches disagree on original names", False
    origins = origins_for(matches)
    if not origins:
        return "blocked: matching cache has no valid origin metadata", False

    if generation["kind"] == "audio":
        if not archived_audio_matches(collection_dir, generation["chapters"]):
            return "blocked: archived chapters differ from the intact cache", False
        repaired = build_audio_entry(content_hash, generation, origins)
        details = f"audio, {len(generation['chapters'])} chapters"
    else:
        if not archived_tonieplay_matches(collection_dir, generation):
            return (
                "blocked: archived Tonieplay manifest or objects differ from cache",
                False,
            )
        repaired = build_tonieplay_entry(content_hash, generation, origins)
        details = f"Tonieplay, {len(generation['objects'])} objects"

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
        if is_canonical_hash(path.name) and path.name not in cache_index
    }


def recache_until_resolved(
    api: TeddyCloudApi,
    cache_root: Path,
    candidates: list[Path],
) -> dict[str, list[dict[str, Any]]]:
    cache_index = build_cache_index(cache_root)
    remaining = unresolved_hashes(candidates, cache_index)
    if not remaining:
        print("Recache: every damaged collection already has an intact cache match.")
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
                collection_dir, cache_index, args.apply
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
