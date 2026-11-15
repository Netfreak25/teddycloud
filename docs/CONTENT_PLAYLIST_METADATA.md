# Content-bound playlist display metadata

This subsystem stores content-specific display data independently of the TB1 TAP playlist subsystem and playback routing. Its TAF part stores names for directly assigned custom files; the separate native part resolves original and assigned V3 content by version. The read-only TAF total duration also supports original cached TAFs without a source assignment.

## Versioned native V3 playlists

Native metadata is separate from figurine identity (`tonieInfo`, model, custom
image). `getTagInfo` resolves the requested `contentVersion`, including retained
older generations, rather than substituting the active newer cache. Without a
requested version the existing content selection remains authoritative. Unknown
native content still returns `playlist.kind = "native_collection"`; clients must
not substitute the figure's catalog tracks or a nearby TAF timeline.

```json
{"playlist":{"kind":"native_collection","contentVersion":1794257325,
 "chapterCount":3,"title":"","tracks":["Opening","","Ending"],
 "durations":[33.125,null,32.875],"metadataState":"partial",
 "metadataReason":"durations_unavailable","metadataRevision":"<sha256>"}}
```

`chapterCount` and `contentVersion` are omitted if structure/version cannot be
resolved. Titles and durations retain manifest order; unknown names are empty,
unknown durations `null`, not zero. Names prefer saved collection data, explicit
manifest/Ogg titles, then a unique content-version catalog match. Equal counts
alone and technical analytics slugs are not titles. Clients use localized
`Chapter N` labels without changing indices.

States are `pending`, `ready`, `partial`, `unavailable`. Names are optional:
known structure and all known durations suffice for `ready`. `totalSeconds`
appears only with all durations known, summing fractions before display rounding.
The existing bounded Ogg/Opus validator reads immutable files once, without
decoding: `(final_granule - pre_skip) / 48000`. Unsupported/incomplete streams
remain unknown; this does not change their playback handling.

Sanitized records live at
`<datadir>/content-metadata/v3-playlists/<scope-hash>/<RUID>/<version>.json`.
The scope includes stable box identity and effective content/library directories.
Records contain chapter identities and attempt state, not auth tokens/raw
manifests. Atomic same-directory publication is limited to 1 MiB. Intrinsic
file durations/Ogg titles can enrich existing Library fields; manifest-specific
names stay per RUID/version. Successfully served assigned collections bind their
actual Library identity to the version, preserving it across source changes.

Actual TB2 playback can submit a task to the existing serial worker. Local data
comes first, followed by an exact-version manifest/missing-audio download only
with existing Source/NoCloud/cache/cloud/certificate permissions. Another cloud
version is not downloaded under the requested version. Metadata-only fills do
not activate a cache generation, change Source, acknowledge freshness or send
controls. List/status reads never enqueue cloud work. Attempts survive restarts;
interrupted attempts are visible rather than retried. Manual V3 refresh resets
the latch and permits rescanning previously unmeasurable files.

`runtime.playback.metadataRevision` is an opaque string for local metadata
reload through existing box polling. Both clients reject stale identities and
versions. TeddyRemote retains fractional seconds internally as milliseconds and
does not apply intro/outro shifts to native playlists. Box `chapterDuration`
and `chapterUntilMs` remain the first choice for current position calculations.

Changes by file:

- `src/v3_playlist_metadata.c`, `include/v3_playlist_metadata.h`: isolated
  version resolution, sanitized persistence and enrichment; no generic store
  or second queue. `include/mutex_manager.h` adds the publication lock.
- `src/encoder_api.c`, `include/encoder_api.h`: optional intrinsic `TITLE`
  extraction during the existing validator pass, without another decoder.
- `src/v3_native_cache.c`, `include/v3_native_cache.h`: passive retained/staging
  snapshots and detached fills rather than activating old versions for inspection.
- `src/original_content_cache.c`, `include/original_content_cache.h`,
  `src/original_content_transfer.c`: owned playback jobs reuse bounded admission,
  policy and transport, with persistent one-attempt state.
- `src/handler_cloud.c`: observe manifests/completed files and bind served
  collections; metadata completions skip route/freshness finalizers.
- `src/mqtt_server.c`: enqueue real playback without file/network I/O in the
  observer. `src/handler_api.c`: exact playlists/revisions, no figure/TAF bleed.
- WebUI playlist helpers and TeddyRemote resolver: consistent nullable durations,
  authoritative counts and revision reloads rather than cloud polling.

Audio-restart investigation and hardware acceptance remain separate. Tests do
not establish either. Existing TAF timing remains as described below.

## TAF behavior

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

## Read-only total duration in tag info

`GET /api/getTagInfo?ruid=<RUID>&overlay=<BOX>&contentVersion=<VERSION>`
optionally returns `tagInfo.totalSeconds`, a JSON integer in seconds on the
same timeline as `tagInfo.trackSeconds`:

```json
{"tagInfo":{"trackSeconds":[0,61,125],"totalSeconds":200}}
```

Clients can derive chapter durations of 61, 64 and 75 seconds. Existing
`playlist.durations` take precedence. This also works without catalog chapter
names or a manually assigned source. The shared tag-index entries expose the
same optional field.

The API reuses the already calculated TAF end time and checks the selected
file's size against its declared payload length. A valid header alone is not
evidence of a complete download. Missing/zero end times, incomplete files,
failed size queries, mismatched chapter counts and inconsistent chapter starts
omit the field; no guessed duration or dummy zero is returned.

Only the selected complete cached or directly assigned TAF supplies this
duration. Native collections, Tonieplay, TAP, streams and unresolved source
assignments must not borrow it from a neighbouring TAF cache. Their existing
playlist and content-version handling remains unchanged. The addition performs
no audio scan, download, hash validation, encoding or metadata write, and does
not change source selection or routing. Older clients can ignore the field;
TeddyRemote 0.1.30 already consumes it.

## Native audio downloads and shared player selection

The info dialog can export complete native audio from either an assigned Library
collection or the read-only original-cache API documented in
[TB2_V3_CONTENT_CACHE.md](TB2_V3_CONTENT_CACHE.md). Exports contain unchanged Opus
chapters in playback order, named `001 - Title.opus` etc.; titles require a complete
matching list. Otherwise the UI uses localized chapter numbers. Filenames are
sanitized, and a failed chapter fetch aborts the entire export.

`teddycloud_web/src/utils/audio/tonieAudio.ts` shares source/preference/fallback
selection between card, information dialog and player. A playable V3 fallback
does not hide the action to download a missing preferred TAF. Library-only player
entries retain Library metadata priority; tonie-bound starts retain their RUID
and custom-image context. No audio conversion or Library manifest rewrite is
performed, and File Manager archive export is unchanged.

## Code changes by file

### `include/content_playlist.h` and `src/content_playlist.c`

- What changed: define and implement validation, content-addressed loading, chapter-duration calculation, and atomic saving of direct-TAF playlist display metadata.
- Why: editable names belong to the actual custom TAF version rather than to a Tonie model or rUID, and the real chapter count must remain tied to that TAF.
- Why this approach: embedding display names into TAF or content JSON would mix UI metadata with audio and routing state; storing it by audio ID plus SHA-1 makes reuse and invalidation follow the assigned TAF bytes.

### `src/handler_api.c`, `include/handler_api.h`, and `src/server.c`

- What changed: `getTagInfoJson()` exposes the exact direct-TAF chapter count, user-saved names or exact TAF catalog defaults, and `POST /api/content/playlist/<RUID>` saves one complete set of names for the selected overlay. Native TB2 collections expose a separate read-only playlist resolved from their recorded origins and chapters. A small internal API helper adds the optional total duration from the same selected TAF and chapter positions, after completeness and timeline checks.
- Why: the server is the only place that can validate the current direct TAF identity and chapter count before accepting edited names.
- Why this approach: accepting a client-provided content hash or chapter count would allow stale browser state to write metadata for the wrong source. Resolving defaults from the exact TAF identity avoids reusing unrelated metadata from the physical Tonie. Reusing the existing calculated end time closes the last-chapter duration gap without a new playlist format or an audio rescan.

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
