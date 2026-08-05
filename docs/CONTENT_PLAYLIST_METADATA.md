# Custom TAF playlist display metadata

This subsystem stores display names for directly assigned custom TAF files. It is independent of the TB1 TAP playlist subsystem and is not used for streams, TONIES original content, or playback routing.

## Behavior

The live TB2 playlist uses the custom TAF header as its authoritative structure. Its chapter count is always `n_track_page_nums`; original Tonie titles and their count must not enlarge or relabel custom content. TAF track positions are chapter start offsets and the WebUI labels them accordingly.

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

- What changed: `getTagInfoJson()` exposes the exact direct-TAF chapter count and saved/default names, and `POST /api/content/playlist/<RUID>` saves one complete set of names for the selected overlay.
- Why: the server is the only place that can validate the current direct TAF identity and chapter count before accepting edited names.
- Why this approach: accepting a client-provided content hash or chapter count would allow stale browser state to write metadata for the wrong source.

### `teddycloud_web/src/components/tonieboxes/tonieboxcard/live`

- What changed: directly assigned custom TAF content no longer falls back to original Tonie track names. The drawer edits the content title and all real chapter titles, saves them together, and refreshes the current card after success.
- Why: partial per-row saves could leave a mixed title set, and the original Tonie catalog is unrelated after a private TAF is assigned.
- Why this approach: one explicit save keeps the editor simple and matches the backend's content-version-level document.
