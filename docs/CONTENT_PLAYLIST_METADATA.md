# Custom TAF playlist display metadata

This subsystem stores display names for directly assigned custom TAF files. It is independent of the TB1 TAP playlist subsystem and is not used for streams, TONIES original content, or playback routing.

## Behavior

The live TB2 playlist uses the custom TAF header as its authoritative structure. Its chapter count is always `n_track_page_nums`; metadata belonging only to the physical Tonie must not enlarge or relabel custom content. TAF track positions are chapter start offsets and the WebUI labels them accordingly.

When the assigned TAF's exact `audio_id` and SHA-1 identify a catalog entry and
its track count equals the real TAF chapter count, that entry supplies the
initial content and chapter titles. This is content-specific metadata, not a
fallback to the physical Tonie's model. User-saved playlist metadata always
wins. Unknown or count-mismatched TAFs keep the neutral `Chapter N` fallback.

Users can store one content title and one title for every real chapter. The metadata is addressed by the direct TAF content identity and lives at:

```text
<datadir>/content-metadata/taf-playlists/<AUDIO-ID>-<TAF-SHA1>.json
```

The same document stores the calculated duration of every chapter. Durations are derived server-side from consecutive TAF chapter start positions; the final chapter ends at the TAF's final Ogg granule position. The browser never supplies or estimates these values.

```json
{
  "schema": 2,
  "title": "Custom title",
  "tracks": ["Chapter 1", "Chapter 2"],
  "durations": [123, 456]
}
```

Changing the assigned TAF selects a different metadata document, while assigning the exact same TAF reuses its saved names. The metadata does not alter TAF audio, chapter boundaries, V3 manifests, or playback control. Writes use a temporary file followed by the project filesystem move operation so readers never observe a partially written playlist.

## Code changes by file

### `include/content_playlist.h` and `src/content_playlist.c`

- What changed: define and implement validation, content-addressed loading, chapter-duration calculation, and atomic saving of direct-TAF playlist display metadata.
- Why: editable names belong to the actual custom TAF version rather than to a Tonie model or rUID, and the real chapter count must remain tied to that TAF.
- Why this approach: embedding display names into TAF or content JSON would mix UI metadata with audio and routing state; storing it by audio ID plus SHA-1 makes reuse and invalidation follow the assigned TAF bytes.

### `src/handler_api.c`, `include/handler_api.h`, and `src/server.c`

- What changed: `getTagInfoJson()` exposes the exact direct-TAF chapter count, user-saved names or exact TAF catalog defaults, and `POST /api/content/playlist/<RUID>` saves one complete set of names for the selected overlay. Native TB2 collections expose a separate read-only playlist resolved from their recorded origins and chapters.
- Why: the server is the only place that can validate the current direct TAF identity and chapter count before accepting edited names.
- Why this approach: accepting a client-provided content hash or chapter count would allow stale browser state to write metadata for the wrong source. Resolving defaults from the exact TAF identity avoids reusing unrelated metadata from the physical Tonie.

### `include/v3_native_cache.h` and `src/v3_native_cache.c`

- What changed: validated native collections expose their recorded overlay,
  rUID and content-version origins to read-only API consumers.
- Why: the live playlist must resolve source metadata from the collection that
  is actually assigned, including when it is assigned to a different Tonie.
- Why this approach: using the target Tonie's model would silently show the
  wrong title whenever two Tonies share a chapter count; origins are already
  part of the immutable library manifest and require no new persisted format.

### `teddycloud_web/src/components/tonieboxes/tonieboxcard/live`

- What changed: directly assigned custom TAF content consumes only the playlist metadata selected by the backend. The same resolver now renders read-only native-collection playlists. The drawer edits the content title and all real TAF chapter titles, saves them together, and refreshes the current card after success.
- Why: partial per-row saves could leave a mixed title set, and the original Tonie catalog is unrelated after a private TAF is assigned.
- Why this approach: one explicit save keeps the editor simple and matches the backend's content-version-level document; keeping source resolution in the backend prevents the browser from guessing whether catalog metadata belongs to the assigned bytes.
