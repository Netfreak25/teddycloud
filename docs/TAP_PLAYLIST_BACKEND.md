# TAP Playlist Backend

This document describes the backend behavior for TAP playlist generation, streaming, cache validation and publish handling.

## Implementation overview

The backend changes are grouped around one behavior: stable TAF-only TAP playlists should be generated fast, streamed from a growing `.taf.tmp`, published safely as a final `.taf`, and then stay valid across freshness checks.

The main backend change is the TAF-only remux path. It avoids PCM decoding and Opus re-encoding, derives a complete final TAF header before streaming, writes Ogg pages with predictable block alignment, and allows the first download to be cache-safe.

The HTTP/cloud handling was changed so live TAP generation streams from `.taf.tmp`, keeps that temporary file alive until the response and generator are done, publishes the final file only after successful generation, and treats the TAP `audio_id` as the playlist version source.

V3 freshness handling also keeps the latest reported box inventory per overlay. When a content JSON mapping changes, the backend can compare that stored box-side `audio_id` with the current server-side content version and queue an invalidation for the affected overlay before the next play request. A `source` change is treated as a content mapping change even when the effective `audio_id` stays the same. In that case V3 content-meta uses `alwaysReset`, and the overlay receives a temporary replacement version until the natural server version changes again.

The fallback path remains deliberately conservative. MP3 and non-TAF entries still use FFmpeg generation because their final SHA1, Ogg state and track pages are not known before encoding completes. For stable TAP playlists, the FFmpeg-generated final `.taf` still uses the TAP `audio_id` so the regenerated file can become current after the planned redownload.

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

- What changed: extends the TAP model with `shuffle`, a backend-only random-candidate limit, runtime index helpers, live-header prediction and replace-safe publish APIs.
- Why: the handler and generator need an explicit contract for dynamic playlists, selected runtime order, cache-safe prediction and final publish ownership.
- Why not another solution: mutating the saved playlist order or encoding all state into `stream_ctx_t` would mix persistent data, request-local state and global stream state.

### `include/handler_cloud.h`

- What changed: declares the internal content-mapping freshness trigger used by the content JSON API.
- Why: the API handler should not duplicate V3 freshness inventory or audio-version comparison logic.
- Why not another solution: putting the comparison into the API layer would split freshness rules between HTTP API code and cloud request handling.

### `include/mqtt_server.h`

- What changed: exposes success-aware `fresh-tonies` publish helpers, including an overlay-based publisher.
- Why: pro-active V3 invalidations are overlay decisions and must be delivered only to an active box connection for that overlay.
- Why not another solution: broadcasting over the generic MQTT path would expose box-only commands to diagnostic subscribers and would not give reliable delivery feedback.

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

- What changed: after a successful `/content/json/set/<ruid>` save, the handler calls the internal V3 freshness trigger and marks whether the `source` changed.
- Why: content assignment changes are the point where the server knows a box may still hold an older local copy.
- Why not another solution: reacting to generic file uploads would create false positives for files that are not assigned to a rUID.

### `src/handler_cloud.c`

- What changed: owns TAP live streaming, per-target locking, prediction-based local `Content-Length`, generator lifecycle, handler-owned publish, freshness decisions, V3 content-meta protection and pro-active V3 freshness invalidation. The freshness comparison is shared by normal checks and content-mapping updates. V3 content-meta and freshness now use the same effective server version, including overlay-local replacement versions after source changes. Source-change invalidations keep their pending state through content-meta and are cleared after the first matching chapter request.
- Why: the HTTP handler is the only component that knows when delivery has ended, when the generator has finished, when `.taf.tmp` can safely be published or deleted, and which server-side version should be compared with the box-reported inventory. Source changes can require a reset even when the natural `audio_id` is unchanged, and content-meta alone is not enough to prove that the following chapter was redownloaded.
- Why not another solution: letting the generator publish immediately races with HTTP retry/range reads; marking every TAP as fresh causes redownload loops; serving stale finals ignores TAP `audio_id` changes. The predicted stream size is kept as a local per-request settings override so Cyclone remains unchanged. A separate pro-active comparison path would risk diverging from normal freshness semantics. Clearing the pending state at content-meta would allow a later ranged chapter request to reuse stale local bytes.

### `src/cyclone/cyclone_tcp/http/http_server.c`

- What changed: rejects HTTP range requests whose start offset is outside the effective response length with `416` and `Content-Range: bytes */<length>`.
- Why: V3 chapter responses can represent virtual slices, and old local offsets must not underflow `Content-Length` or resume from bytes that no longer belong to the current response.
- Why not another solution: silently clamping to the end would look like a successful resume and could keep stale local data in place; moving the guard into each caller would duplicate range validation across normal files, TAP output and V3 chapter splits.

### `src/cyclone/cyclone_tcp/http/http_server_misc.h`

- What changed: adds the HTTP status phrase used for `416` responses.
- Why: the range guard needs a valid status line for unsatisfiable resume requests.
- Why not another solution: returning a generic client error would hide the exact range problem from clients and logs.

### `src/mqtt_server.c`

- What changed: `fresh-tonies` publishing now builds one JSON array of up to 50 deduplicated rUIDs, targets only active box connections whose certificate mapped to the same overlay and whose subscription matches the box command topic, checks write success, coalesces repeated invalidations in a short debounce window and keeps the publish pending while the affected rUID is the active TB2 playback rUID.
- Why: the command is box-specific, pro-active TAP/content invalidations often arrive in bursts, and the changed flag should only be cleared after the later content request proves the box actually revalidated the stale item. Holding active playback avoids asking the box to invalidate the item it is currently using.
- Why not another solution: individual per-UID publishes increase traffic and make partial delivery ambiguous; generic topic broadcast would also send box-only invalidations to external MQTT tools. Clearing on publish alone would still be too early because the box may not request the updated content immediately.

### `src/settings.c`

- What changed: registers the V3 freshness inventory arrays and V3 replacement-version arrays as internal U64 arrays, and allows setting a U64 array to `NULL, 0` to clear it.
- Why: missing or invalid V3 inventory must remove old runtime state, empty freshness caches are valid states, and replacement versions are runtime state tied to one overlay.
- Why not another solution: keeping stale inventory when a request omits content would let later content changes use outdated box state. Persisting replacement versions as visible settings would expose implementation state and make user configuration harder to reason about.

### `src/json_helper.c`

- What changed: `jsonGetUInt32()` accepts numeric JSON strings with strict unsigned parsing.
- Why: existing WebUI/API paths can persist TAP `audio_id` as a string, and stable freshness depends on reading that value correctly.
- Why not another solution: only fixing the WebUI would not repair existing playlist files; silently accepting signs or partial strings would make invalid versions look valid.

### `src/tonie_audio_playlist.c`

- What changed: implements TAP load validation, shuffle/runtime indices, TAF-only remux, live-header prediction, replace-safe publish and generator task lifecycle. Normal and shuffle-all playlists are limited by the runtime TAF source/chapter limit; shuffle-one playlists may load a larger candidate catalog because only one entry is selected for generation. The FFmpeg fallback passes the TAP `audio_id` into generated finals when the playlist has a stable non-zero version.
- Why: TAF-only playlists need a fast path that can generate cache-safe bytes and metadata without FFmpeg re-encoding.
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
- No active freshness reason marks the UID as outdated.

The TAP `audio_id` is the playlist version source. Changing it makes any final `.taf` with a different header `audio_id` stale.

`audio_id == 0`, `shuffle == 1` and `shuffle == 2` intentionally force rebuild and redownload behavior.

`shuffle == 2` selects one random entry at request time. The backend may therefore accept more stored candidate entries for this mode than it can generate in a normal or shuffle-all playlist. The runtime source limit is still enforced after selection, so generated TAF files remain within the TAF chapter/source constraints. Frontend limits may stay lower and are only a UI constraint.

## Remux fast path

TAF-only playlists use the remux path when all selected entries are compatible TAF files. This path does not decode PCM and does not re-encode Opus. It can derive its own final payload, SHA1, track pages, Ogg state and content length before the HTTP response body is sent.

This path is the cache-safe first-download path for TAF-only playlists.

If future per-entry volume handling is implemented, it cannot be applied by this remux fast path. Volume changes require audio processing and therefore must use a re-encode path, even for TAF-only playlists.

## Freshness and versioning

Freshness checks use the TAP `audio_id` as the effective server version for stable playlists. A box cache is current only when its cached version matches the TAP `audio_id` and the server-side final `.taf` is current.

Missing, invalid or stale final `.taf` files force the playlist back into the stream/build path so the server cache can be regenerated.

## FFmpeg fallback

MP3 and non-TAF playlist entries intentionally fall back to FFmpeg generation. This remains functional, but it is not a cache-safe first-download path:

- exact final SHA1 is not known before encoding completes,
- exact final track pages are not known before encoding completes,
- the first download requires a later freshness/redownload cycle,
- the box can play its OHOH box error sound because the first live stream is not fully cache-safe.

For stable TAP playlists with `audio_id != 0`, this fallback writes the TAP `audio_id` into the final FFmpeg-generated `.taf`. This keeps the final file aligned with the playlist version after the first generation/redownload cycle and prevents an endless re-encode/freshness loop.

For TAP playlists with `audio_id == 0`, and for generic non-TAP FFmpeg streaming/conversion, the existing time-based FFmpeg `audio_id` behavior is preserved.

This is a deliberate fallback until a reliable prediction or pre-generation strategy exists for non-TAF sources.

## Duplicate TAP target paths

Each stable TAP playlist should use a unique final `filepath`, for example `lib://by/tapID/name.taf`.

If two different `.tap` files write to the same final `.taf`, they share one cache key. Different `audio_id` values will then overwrite the same final header and can cause a freshness loop:

- playlist A builds `target.taf` with `audio_id A`,
- playlist B builds `target.taf` with `audio_id B`,
- playlist A later sees `target.taf` as stale and rebuilds again.

This belongs in tooling or UI validation: warn when multiple `.tap` files point at the same final `filepath`.

## Dadong behavior

Dadong is the box confirmation sound after a successful content download. It is expected for stable TAF-only TAP downloads once freshness and cache state are correct.

The backend streams the initial response from `.taf.tmp` and publishes the final `.taf` only after generation and HTTP delivery have ended.

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
