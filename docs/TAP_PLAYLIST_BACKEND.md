# TAP Playlist Backend

This document describes the backend behavior for TAP playlist generation, streaming, cache validation and publish handling.

Display metadata for directly assigned custom TAF files is separate from this TB1 TAP playlist subsystem and is documented in [CONTENT_PLAYLIST_METADATA.md](CONTENT_PLAYLIST_METADATA.md).

## Implementation overview

The backend changes are grouped around one behavior: stable TAF-only TAP playlists should be generated fast, streamed from a growing `.taf.tmp`, published safely as a final `.taf`, and then stay valid across freshness checks.

The main backend change is the TAF-only remux path. It avoids PCM decoding and Opus re-encoding, derives a complete final TAF header before streaming, writes Ogg pages with predictable block alignment, and allows the first download to be cache-safe.

The same packet writer now also accepts the independent Ogg/Opus chapters of a
native TB2 library collection. Only the reader offset differs: TAF playlist
entries begin after the 4096-byte TAF header, while native chapters begin at
byte zero. Page writing, Opus compatibility checks, SHA1, track positions,
header prediction and replace-safe publication remain shared.

The HTTP/cloud handling was changed so live TAP generation streams from `.taf.tmp`, keeps that temporary file alive until the response and generator are done, publishes the final file only after successful generation, and treats the TAP `audio_id` as the playlist version source.

V3 freshness handling also keeps the latest reported box inventory per overlay. When a content JSON mapping changes, the backend can compare that stored box-side `audio_id` with the current server-side content version and queue an invalidation for the affected overlay before the next play request. A `source` change is treated as a content mapping change even when the natural `audio_id` stays the same. In that case V3 content-meta uses `alwaysReset`, and the overlay receives a deterministic replacement version tied to the persistent source revision. V3 freshness and content-meta share the same per-Tonie cloud policy. The automatic source lock cannot be bypassed upstream. The source-change marker survives content-meta and chapter delivery; exact-version MQTT playback confirms direct content, while TAP keeps its specialized playback observer.

For the older progressive content path, the fallback remains deliberately conservative. MP3 and non-TAF entries still use FFmpeg generation because their final SHA1, Ogg state and track pages are not known before encoding completes. For stable TAP playlists, the FFmpeg-generated final `.taf` still uses the TAP `audio_id` so the regenerated file can become current after the planned redownload. The TB2 V3 path instead waits for this generation to finish and materializes the complete final TAF before returning content-meta.

## Code changes by file

### `include/fs_ext.h`

- What changed: declares `fsFlushFile(FsFile *file)`.
- Why: TAP remux writes a growing `.taf.tmp` that must become visible to the HTTP reader without bypassing the project file abstraction in TAP code.
- Why not another solution: editing Cyclone FS ports would pull submodule/vendor code into the branch; keeping direct `FILE *` casts in TAP code would keep the portability issue.

### `include/mutex_manager.h`

- What changed: adds `MUTEX_TAP_TARGETS`.
- Why: concurrent requests for the same TAP target must not write or publish the same `.taf.tmp` and final `.taf` at the same time.
- Why not another solution: a single global TAP generation lock would block unrelated playlists, while no target lock leaves real race conditions for two boxes or quick Tonie switches.

### `include/tonie_audio_playlist.h`

- What changed: extends the TAP model with `shuffle`, a backend-only random-candidate limit, runtime index helpers, live-header prediction, replace-safe publish APIs, synchronous final-snapshot materialization for V3 and the native-collection remux entry point.
- Why: the handler and generator need an explicit contract for dynamic playlists, selected runtime order, cache-safe prediction and final publish ownership. V3 additionally needs a complete final TAF and must never treat a growing `.tmp` as a chapter source.
- Why not another solution: mutating the saved playlist order or encoding all state into `stream_ctx_t` would mix persistent data, request-local state and global stream state.

### `include/handler_cloud.h`

- What changed: declares the internal content-mapping freshness trigger used by the content JSON API.
- Why: the API handler should not duplicate V3 freshness inventory or audio-version comparison logic.
- Why not another solution: putting the comparison into the API layer would split freshness rules between HTTP API code and cloud request handling.

### `include/mqtt_server.h`

- What changed: exposes overlay-wide and UID-specific `fresh-tonies` queue helpers.
- Why: pro-active V3 invalidations are overlay decisions, while a content-mapping update must be able to requeue exactly the changed UID even if it was acknowledged earlier on the same MQTT connection.
- Why not another solution: broadcasting over the generic MQTT path would expose box-only commands to diagnostic subscribers; requeueing the full overlay would resend unrelated Tonies.

### `include/settings.h`

- What changed: adds internal runtime arrays for the latest V3 freshness inventory UIDs, their matching audio IDs and per-overlay V3 replacement versions for source-change resets.
- Why: pro-active comparison needs the last box-reported version per overlay without turning that runtime state into user configuration. Source changes with unchanged natural versions need an overlay-local replacement version so the reset can be represented consistently in freshness and content-meta.
- Why not another solution: storing this in public config would persist volatile device state and make unrelated settings exports noisy. Changing the global content version would affect other overlays that may not have reported the same inventory state.

### `src/tonie_audio_playlist_internal.h`

- What changed: contains the internal TAP generator task parameter and task entry point.
- Why: request/task lifecycle state is needed by `handler_cloud.c` and `tonie_audio_playlist.c`, but it is not part of the public TAP playlist model.
- Why not another solution: keeping `tap_generate_param_t` in the public header exposes task-internal fields to unrelated code and makes the public API harder to review.

### `include/toniefile.h`

- What changed: adds `TONIEFILE_MAX_SOURCES`, `toniefile_live_header_t`, and `toniefile_write_taf_header()`.
- Why: live TAP streaming needs a complete header before the HTTP body is sent, including payload size, SHA1, track pages and Ogg state.
- Why not another solution: hardcoded source limits and partial headers caused drift; waiting for `toniefile_close()` before streaming would remove the live first-download behavior.

### `src/fs_ext.c`

- What changed: implements `fsFlushFile()` for the project FS extension layer.
- Why: remux output must be flushed after visible page/header writes so a concurrent HTTP reader can see complete data.
- Why not another solution: removing flushes risks early EOF on growing files; changing Cyclone ports is outside this branch.

### `src/handler.c`

- What changed: treats `.taf.tmp` as a growing live TAF during track-position validation and routes freshness callback updates through the overlay-aware MQTT freshness publisher.
- Why: the live header can contain track pages that point beyond the bytes currently written, so read-ahead EOF is expected while generation is still running. Freshness callbacks need to notify the active box connection even though the HTTP request context is not an MQTT connection.
- Why not another solution: disabling validation globally would hide real corrupt final TAFs; delaying all streaming until the final file exists would remove live TAP behavior. Reusing the direct connection publisher from an HTTP callback would miss active MQTT box sessions.

### `src/handler_api.c`

- What changed: after a successful `/content/json/set/<ruid>` save, the handler advances the persistent source revision, maintains the automatic Source-NoCloud bit and calls the internal V3 freshness trigger only for a real source-string change. Native collection selections are fully validated before this state changes, and `fileIndexV2` annotates complete `by/contentHash` collection directories as one selectable source. The existing content-download action selects TB1 V1/V2 or the TB2 V3 download orchestrator from the requested overlay's box generation.
- Why: content assignment is the point where the server can distinguish a new private generation from a repeated save. The overlay selected by the existing download URL is also the authoritative generation and credential scope for a manual download.
- Why not another solution: deriving changes from TAF Audio-IDs misses same/lower/missing IDs; reacting to generic uploads creates false positives for unassigned files. A separate TB2 endpoint would duplicate the button/API contract and could select different overlay settings.

### `include/contentJson.h` and `src/contentJson.c`

- What changed: persist manual and source-derived NoCloud provenance plus `source_revision`; keep `nocloud` as their compatibility result. Canonical native collection URIs are classified as a distinct non-stream source and malformed native URIs fail closed instead of enabling live streaming.
- Why: removing a source must clear its automatic lock without clearing an independent manual lock, and deterministic versions need restart-safe change identity.
- Why not another solution: one boolean cannot recover provenance; a timestamp is neither deterministic nor reproducible.

### `include/tb2_nocloud_policy.h` and `src/tb2_nocloud_policy.c`

- What changed: expose effective manual/source NoCloud policy and prevent a cloud identity override from bypassing the automatic private-source lock.
- Why: MQTT, freshness and HTTPS must apply the same per-overlay RUID decision.
- Why not another solution: separate endpoint-specific checks would drift and could leak private-content telemetry.

### `include/v3_native_cache.h` and `src/v3_native_cache.c`

- What changed: load a complete active original manifest byte-for-byte, expose its TONIES version, invalidate its active marker on source changes and retain the validated current content-meta route in memory even when native caching is disabled. The same parsed route can now provide a bounded manual-download plan with validated original chapter names, exact sizes and per-chapter auth tokens. It also validates canonical `lib://by/contentHash/.../library-entry.json` sources and their immutable chapter files.
- Why: original content must retain TONIES names/version while private assignments and NoCloud need an exact RUID/version/name association before any chapter can be forwarded; unknown names fail closed until content-meta re-establishes that route. Manual downloads need the exact manifest order and auth while still using the normal capture and activation path.
- Why not another solution: rebuilding the JSON changes the box-visible response; guessing a RUID from an opaque chapter name is unsafe; a separate route/parser/cachewriter would duplicate validation and eventually diverge from normal MITM caching.

### `include/handler_cloud.h` and `src/handler_cloud.c`

- What changed: owns TAP live streaming, per-target locking, prediction-based local `Content-Length`, generator lifecycle, handler-owned publish, freshness decisions, V3 content-meta protection and pro-active V3 freshness invalidation. The freshness comparison is shared by normal checks and content-mapping updates. V3 content-meta and freshness use the same effective server version and the provenance-aware NoCloud policy, including overlay-local deterministic replacement versions after source changes. The V3 freshness request remains one filtered content map: private and effective-NoCloud RUIDs stay local, complete active original-cache versions replace the box version, and the validated TONIES response is merged additively so local stale decisions win. For V3, a live TAP is synchronously materialized under the target lock before immutable Ogg/Opus chapters and the generation descriptor are created. Native library collections instead reference their already immutable chapter files directly and create target-RUID-specific V3 names. For TB1, the handler predicts, streams and replace-safely publishes one hash-keyed derived TAF from the same collection. Source-change invalidations remain pending through content-meta and MQTT playback and are completed only by a successful chapter from the still-current generation. A mapping change queues the exact affected UID for MQTT freshness delivery. The manual TB2 download resolves the shared certificate/content identity, fetches one manifest and its chapters sequentially through the same native-cache capture functions, restores the prior active route after failures and reports stage-specific JSON/log errors.
- Why: the HTTP handler is the only component that owns target serialization, source selection, the effective overlay version and local-versus-TONIES routing. V3 requires an immutable complete generation before content-meta becomes visible; a growing `.taf.tmp` cannot provide that contract. Keeping manual orchestration here lets the existing TB2 TLS identity and cache callbacks remain authoritative.
- Why not another solution: using `.tmp` directly can publish incomplete or changing chapter bytes; letting Meta and Chapter choose shuffle order independently can describe and serve different content; clearing freshness on content-meta, an error response or an unvalidated old chapter can acknowledge the wrong generation. A second downloader with its own files would bypass atomic activation and could damage or replace the previous active version after a partial transfer.

### `tests/test_tb2_v3_manual_download_contract.py`

- What changed: verifies generation dispatch, the unchanged TB1 calls, shared TB2 identity selection, reuse of all native-cache capture stages, bounded chapter auth, active-route restoration, stage-specific API errors and cache documentation.
- Why: the manual action spans the API handler, cloud request callbacks and cache module, so its critical contract is the absence of a second writer and preservation of the previous active generation on every failure stage.
- Why not another solution: a broad end-to-end cloud test would require live TONIES credentials and would be nondeterministic; the focused contract complements the existing native-cache filesystem tests without duplicating them.

### `src/mqtt_server.c`

- What changed: `fresh-tonies` publishing sends one `{"tonie":"<RUID>"}` object per deduplicated cache UID, sequentially with MQTT QoS 1. It waits for `PUBACK`, retries twice at five-second intervals with the same packet ID and `DUP=1`, and closes the connection after the third attempt times out. Playback remains observed as box status but no longer clears source-change state.
- Why: the TB2 protocol expects one Tonie per message and reliable ordered delivery. MQTT acknowledgement and content activation are separate: `PUBACK` advances the connection queue, while a source-change entry remains pending for the successful content lifecycle.
- Why not another solution: a QoS-0 array cannot prove partial delivery, and clearing on publish or `PUBACK` is too early because the box may not request the updated content. Generic topic broadcast would also send box-only invalidations to external MQTT tools.

### `src/settings.c`

- What changed: registers the V3 freshness inventory arrays and V3 replacement-version arrays as internal U64 arrays, and allows setting a U64 array to `NULL, 0` to clear it.
- Why: missing or invalid V3 inventory must remove old runtime state, empty freshness caches are valid states, and replacement versions are runtime state tied to one overlay.
- Why not another solution: keeping stale inventory when a request omits content would let later content changes use outdated box state. Persisting replacement versions as visible settings would expose implementation state and make user configuration harder to reason about.

### `src/json_helper.c`

- What changed: `jsonGetUInt32()` accepts numeric JSON strings with strict unsigned parsing.
- Why: existing WebUI/API paths can persist TAP `audio_id` as a string, and stable freshness depends on reading that value correctly.
- Why not another solution: only fixing the WebUI would not repair existing playlist files; silently accepting signs or partial strings would make invalid versions look valid.

### `src/tonie_audio_playlist.c`

- What changed: implements TAP load validation, shuffle/runtime indices, TAF-only remux, live-header prediction, replace-safe publish, generator task lifecycle and synchronous final-snapshot materialization. The Ogg packet reader can also start at byte zero for independent native-library Opus chapters, while using the same writer and header calculation. The snapshot API selects runtime indices once, generates a complete TAF, publishes it to the final path and returns only that final path. Normal and shuffle-all playlists are limited by the runtime TAF source/chapter limit; shuffle-one playlists may load a larger candidate catalog because only one entry is selected for generation. The FFmpeg fallback passes the TAP `audio_id` into generated finals when the playlist has a stable non-zero version.
- Why: TAF-only playlists need a fast path that can generate cache-safe bytes and metadata without FFmpeg re-encoding. The V3 path additionally needs a complete, stable source before creating content-addressed chapters.
- Why not another solution: raw payload concatenation does not produce a valid final stream; FFmpeg re-encoding is slower and cannot know exact first-download header values before encoding completes. Letting FFmpeg own the final `audio_id` is correct for generic conversion, but not for TAP because the `.tap` file defines the content version. Increasing the runtime source limit would exceed the current TAF chapter/source constraints, while keeping the loader limit at the runtime limit would unnecessarily block large shuffle-one catalogs.

### `src/toniefile.c`

- What changed: supports writing complete TAF headers directly, centralizes source limits, preserves close errors, keeps active-state handling consistent and adds an FFmpeg stream entry point with an explicit `audio_id`.
- Why: remux needs to write a final-equivalent header at stream start, while FFmpeg fallback still relies on normal `toniefile_close()` finalization.
- Why not another solution: duplicating protobuf header writing in playlist code would split TAF format ownership; ignoring close errors can publish invalid output as successful generation. Changing the existing `ffmpeg_stream()` semantics would alter non-TAP behavior, so the explicit-Audio-ID path is added alongside the existing time-based wrapper.

## Stable cache requirements

A TAP playlist is treated as stable-cacheable only when all of these are true:

- `shuffle` is missing or `0`.
- `audio_id` is non-zero.
- The final `.taf` exists.
- The final `.taf` is valid.
- The final `.taf` header `audio_id` matches the TAP `audio_id`.

The TAP `audio_id` is the playlist version source. Changing it makes any final `.taf` with a different header `audio_id` stale.

Freshness is evaluated separately from the server-side stable-cache decision. It can require a box redownload while the already generated final `.taf` remains valid and reusable on the server.

`audio_id == 0`, `shuffle == 1` and `shuffle == 2` intentionally force rebuild and redownload behavior.

`shuffle == 2` selects one random entry at request time. The backend may therefore accept more stored candidate entries for this mode than it can generate in a normal or shuffle-all playlist. The runtime source limit is still enforced after selection, so generated TAF files remain within the TAF chapter/source constraints. Frontend limits may stay lower and are only a UI constraint.

## Remux fast path

TAF-only playlists use the remux path when all selected entries are compatible TAF files. This path does not decode PCM and does not re-encode Opus. It can derive its own final payload, SHA1, track pages, Ogg state and content length before the HTTP response body is sent.

This path is the cache-safe first-download path for TAF-only playlists.

If future per-entry volume handling is implemented, it cannot be applied by this remux fast path. Volume changes require audio processing and therefore must use a re-encode path, even for TAF-only playlists.

## TB2 V3 snapshot and chapter cache

The content-aware TB2 V3 path does not serve a growing TAP stream as a chapter source. When `getTonieInfo()` reports `CT_SOURCE_TAP_STREAM`, the V3 handler keeps the TAP target lock and synchronously creates one complete momentary final TAF:

1. shuffle/runtime indices are selected once;
2. the existing TAP generation path writes a complete temporary TAF;
3. replace-safe publication moves that snapshot to the final TAP path;
4. the final is reopened and validated;
5. its chapters are remuxed into independent immutable Ogg/Opus objects.

`CT_SOURCE_TAP_CACHED` already resolves to the complete final TAF and can skip the snapshot generation. In neither case is `<final>.tmp` passed to the V3 materializer or returned as a V3 source.

### Generation-stable TB2 shuffle

TB1 and TB2 continue to read the same `.tap` file. The TB2 V3 path selects
the runtime indices once, passes that exact selection to the complete TAF
snapshot, and stores the selected source indices with the immutable V3
descriptor. The TAP editor exposes all three existing modes and advances the
playlist `audio_id` only when title, target, shuffle, order, or files actually
change. Editing keeps the original `.tap` filename.

For `shuffle=0`, a prepared descriptor is reused until the TAP revision
changes. For `shuffle=1` and `shuffle=2`, the MQTT playback observer marks a
new selection pending only after a stop or a change to another rUID. Pausing,
resuming, chapter changes, content-meta retries, chapter retries, and Range
requests keep the prepared generation. There is deliberately no timeout-based
reshuffle.

The state is stored atomically below
`v3-local/tap-state/<overlay>/<RUID>.json`. It records the TAP revision and
shuffle mode, selection generation, desired/prepared version, the version
actually reported by MQTT playback, the immediately previous version, and the
pending flag. Dynamic generations choose a version above the TAP, known box,
and retained generation versions. A failed preparation keeps the previous
generation usable and leaves regeneration pending.

TAP chapters use a position-independent Ogg serial only in this TB2 path.
Therefore the same remuxed audio produces the same immutable object hash even
if its shuffle position changes. Direct TAF content retains its existing serial
scheme. The descriptor also freezes the resolved playlist title, chapter
titles, original TAP indices, and real chapter durations so a later TAP edit
cannot alter metadata for a generation that is still playing.

Files involved in this extension:

- `include/tonie_audio_playlist.h`, `src/tonie_audio_playlist.c`: selected-index
  snapshot entry point plus the unchanged TB1 wrapper.
- `include/v3_local_content.h`, `src/v3_local_content.c`: additive TAP metadata,
  fixed TAP serial, immutable descriptors, and atomic per-rUID TAP state.
- `include/handler_cloud.h`, `src/handler_cloud.c`: version choice, preparation,
  freshness, playback confirmation, and three-generation chapter routing.
- `src/mqtt_server.c`: forwards parsed playback transitions to the TAP observer;
  MQTT transport and commands remain unchanged.
- `teddycloud_web` TAP editor: exposes shuffle, bumps semantic revisions, and
  overwrites the edited file.
- `tests/test_tb2_v3_local_content_contract.py`: protects the focused contracts.

### TB2 remote-control playlist metadata

`/api/getTagInfo` accepts an optional `contentVersion`. For a TAP source the
API loads exactly that immutable descriptor; without the parameter it selects
the version last confirmed by MQTT playback and otherwise the prepared
version. The returned `playlist` object is additive: direct custom TAFs report
`kind=direct_taf`, while TAPs report `kind=tap` together with content version,
shuffle mode, edit target, generation-frozen title, tracks, durations, and
chapter count. A missing descriptor produces only a generic TAP fallback and
never borrows original-Tonie track names.

The TB2 card supplies the MQTT playback version whenever the RUID or version
changes. Thus the card title, current chapter, total chapter count, drawer
order, and durations all come from the same generation. Shuffle-one exposes
one chapter; shuffle-all exposes the actual persisted order. TAP editing opens
the existing editor for the original `.tap` path. Direct custom TAF metadata
continues to use the inline playlist-title editor and its existing POST API.
TB1 has no remote-control UI and is unaffected.

Files involved in the remote-control metadata extension:

- `src/handler_api.c`: adds playlist kinds, exact descriptor selection, TAP
  fallback metadata, and the optional `contentVersion` query.
- `teddycloud_web/src/api/apis/TeddyCloudApi.ts` and
  `teddycloud_web/src/types/tonieTypes.ts`: expose the additive API fields and
  versioned lookup without changing the direct-TAF save request.
- `teddycloud_web/src/components/tonieboxes/tonieboxcard/TonieboxCard.tsx`:
  reloads metadata when MQTT reports another content version.
- `teddycloud_web/src/components/tonieboxes/tonieboxcard/live/`: resolves one
  generation for card and drawer, shows the TAP shuffle mode, and separates
  TAP navigation from direct-TAF inline editing.
- `teddycloud_web/src/components/tonies/filebrowser/FileBrowser.tsx`: opens one
  requested TAP in the existing editor and removes only the one-shot query
  parameter.
- `tests/test_content_playlist_contract.py`: protects version selection,
  resolver priority, shuffle display, and edit routing.

V3 chapter objects live below `<cachedir>/v3-local/<full-sha256>.opus`. A generation descriptor at `<cachedir>/v3-local/generations/<overlay>/<RUID>/<effective-version>.json` binds the complete ordered chapter set to one overlay, RUID and version. The protocol name uses the first 20 lowercase digest characters, chapter index and uppercase RUID; the object and descriptor retain the full digest.

The V3 chapter remux copies Opus packets and creates a standalone Ogg stream for each chapter. It does not decode or re-encode audio. All chapter objects are completed before the generation descriptor is published.

This synchronous snapshot is specific to the V3 local-content path. It does not replace the existing progressive TAP streaming behavior used by the older content path. The canonical storage, hash-name, legacy-gate, range and `.part` semantics are documented in [TB2_V3_CONTENT_CACHE.md](TB2_V3_CONTENT_CACHE.md).

## Freshness and versioning

Freshness checks use the TAP `audio_id` as the effective server version for stable playlists. A box cache is current only when its cached version matches the TAP `audio_id` and the server-side final `.taf` is current.

Missing, invalid or stale final `.taf` files force the playlist back into the stream/build path so the server cache can be regenerated.

## FFmpeg fallback

MP3 and non-TAF playlist entries intentionally fall back to FFmpeg generation in the older progressive content path. This remains functional, but it is not a cache-safe first-download path:

- exact final SHA1 is not known before encoding completes,
- exact final track pages are not known before encoding completes,
- the first download requires a later freshness/redownload cycle,
- the box can play its OHOH box error sound because the first live stream is not fully cache-safe.

For stable TAP playlists with `audio_id != 0`, this fallback writes the TAP `audio_id` into the final FFmpeg-generated `.taf`. This keeps the final file aligned with the playlist version after the first generation/redownload cycle and prevents an endless re-encode/freshness loop.

For TAP playlists with `audio_id == 0`, and for generic non-TAP FFmpeg streaming/conversion, the existing time-based FFmpeg `audio_id` behavior is preserved.

This is a deliberate fallback until a reliable prediction or pre-generation strategy exists for non-TAF sources.

TB2 V3 does not expose the progressive result while it is growing. It synchronously completes the FFmpeg-generated TAF first and only then prepares and advertises immutable V3 chapter objects.

## Duplicate TAP target paths

Each stable TAP playlist should use a unique final `filepath`, for example `lib://by/tapID/name.taf`.

If two different `.tap` files write to the same final `.taf`, they share one cache key. Different `audio_id` values will then overwrite the same final header and can cause a freshness loop:

- playlist A builds `target.taf` with `audio_id A`,
- playlist B builds `target.taf` with `audio_id B`,
- playlist A later sees `target.taf` as stale and rebuilds again.

This belongs in tooling or UI validation: warn when multiple `.tap` files point at the same final `filepath`.

## Dadong behavior

Dadong is the box confirmation sound after a successful content download. It is expected for stable TAF-only TAP downloads once freshness and cache state are correct.

The older progressive content path streams the initial response from `.taf.tmp` and publishes the final `.taf` only after generation and HTTP delivery have ended. The TB2 V3 path instead materializes and publishes a complete snapshot before it advertises immutable chapter objects.

If a stable TAF-only playlist finishes green but does not play Dadong, first check whether freshness still marks the UID as outdated or whether the final `.taf` header `audio_id` differs from the TAP `audio_id`.

The backend success criteria remain:

- download ends green,
- no OHOH,
- no unwanted redownload after freshness,
- skip and seek work,
- final `.taf` validates,
- Dadong is present for stable TAF-only playlists after the freshness state is clean.

## JSON numeric strings

The WebUI/API can write numeric fields such as `audio_id` as JSON strings. The backend therefore accepts both forms through `jsonGetUInt32()`:

```json
{ "audio_id": 1781248946 }
{ "audio_id": "1781248946" }
```

The parser is intentionally strict: surrounding whitespace is ignored, but signs, invalid strings, negative values and overflows still resolve to `0`.

## Publish semantics

Generated TAP files are published with replace-safe semantics. Existing finals are moved to a `.replace.bak` backup before the new `.taf.tmp` is renamed into place. On file systems that support replacing rename this may be atomic, but portability is more important than claiming atomic behavior on every supported port.
