# Browser audio encoder

The encoder defaults to **TB1 / TAF**. **TB2 / Native Collection** writes separate
Ogg/Opus chapters directly to the native Library; it does not create an intermediate
TAF or assign a Source to any tag. Native encoding is browser-only. Switching back
to TAF restores the previous server-encoding preference.

## Memory and limits

There is no whole-book WASM output buffer. A worker writes to a private temporary
OPFS file through a synchronous sink and checks every write. The browser decodes
one input at a time at 48 kHz, then transfers one 48,000-frame stereo Int16 block
at a time. The AudioContext is reused and closed on success, failure or cancel.

The decoder still needs the complete input file and decoded AudioBuffer. This is
not a streaming decoder and does not promise unlimited input size. The preflight
shows known input sizes and counts, known server limits, estimated duration/output/
disk usage, and unknown values separately. Stereo Float32 PCM alone needs 384,000
bytes/second; 512 MiB triggers a warning. Real decoder memory is higher and free
RAM is not known. VBR and container overhead make output estimates approximate.
The displayed 25% disk reserve is planning headroom, not an upper bound.

OPFS, workers, a secure context and Web Locks are required. No unavailable-OPFS
fallback accumulates output in RAM. Locks protect active jobs in other tabs while
abandoned encoder-owned directories are cleaned on the next start. Metadata reads
use at most two audio elements concurrently and release their object URLs.

`GET /api/encoder/capabilities` exposes the current format/transport bounds:

- TAF has a 4,092-byte protobuf header. The number of chapters is constrained by
  the actual packed header, not a fixed 99-file UI limit. Bounds are checked before
  packing, including space for final fields. More than 99 chapters has not been
  accepted on TB1 hardware and is explicitly warned about.
- TAF upload is limited to `INT32_MAX - 64 KiB` audio bytes (multipart headroom).
- A native chapter is at most `INT32_MAX` bytes. The complete book need not fit a
  single HTTP request. Native descriptor JSON must fit the existing 1 MiB reader.
- Older backends retain the 99-file TAF compatibility guard and cannot enable
  native import. TAP/FFmpeg source arrays are not enlarged by this feature.

Native finalization accounts for partial frames, encoder lookahead, pre-skip and
EOS granule trimming. TAF block-aligned chapter boundaries can introduce a small
amount of silence (less than 60 ms per boundary); input samples are not dropped.
This is not a gapless-concatenation promise.

## Native import contract

All paths are server-selected below the global Library. The Web API's existing
access boundary applies; import IDs are not an added user authentication system.

1. `POST /api/library/native/import` with
   `{"title":"Book","chapters":[{"title":"Chapter one"}]}` returns `{ "id": "..." }`.
2. `POST /api/library/native/import/<id>/chapter/<index>` uploads a raw, complete
   Ogg/Opus file, starting with index 0 in order. Its structure, sequence, Opus
   packets and EOS are validated; duration comes from the file, not the browser.
3. `POST /api/library/native/import/<id>/finish` publishes only a complete set,
   returning `source`, `contentHash` and `reused`.
4. `DELETE /api/library/native/import/<id>` discards the unpublished job.

At most four imports exist in RAM and each has one writer. Random 128-bit IDs
identify a private `.tb2-encoder-upload` staging directory. Twenty-four-hour idle
jobs are expired on the next import operation; startup removes abandoned staging
jobs. There is no restart recovery, automatic retry queue or new worker thread.
An interrupted upload never exposes a partial collection. Files are hashed and
published by same-filesystem rename under the existing Library lock, after network
I/O. Identical complete collections are reused without replacing their metadata.

Custom entries use `origin: "custom"`, empty `origins`, optional root `title`, and
chapter `title` / `durationSeconds`. No TONIES origin or model identity is invented.
Player, playlist and file index prefer this content metadata to catalog fallbacks.
Old entries without these fields remain valid. Original cache, Source assignment,
NoCloud and Freshness confirmation semantics are unchanged.

## TAF upload safety and verification

`/api/tafUpload` receives exactly one multipart file into a random sibling temporary
file. A complete closing boundary, successful flush, complete declared data size,
SHA-1 and bounded ordered chapter markers are required before atomic replacement.
Bad uploads leave an existing target untouched. The server does not publish
playlist sidecars above the existing reader's 64 KiB limit.

Focused native tests exercise production parsing, upload and publication. The
Chrome regression uses the real decoder, worker, OPFS and built WASM against a
local controlled transport connected to the production C handlers. It uses 114
inputs and checks both formats above 80 MiB, duration/audio end and failure paths.
It is not a production or device test. TB1/TB2 hardware acceptance remains separate.

References: [OPFS](https://web.dev/articles/origin-private-file-system),
[synchronous checked writes](https://developer.mozilla.org/en-US/docs/Web/API/FileSystemSyncAccessHandle/write).

## Verification record (2026-11-09)

Chrome 156.0.8078.80 processed 114 stereo/48-kHz WAV files of 40 seconds each:
TAF **104,129,984 bytes**, native chapters together **110,447,304 bytes**. Both
uploads completed through the production C handlers behind a local test transport.
All 114 native durations sum to 4,560 seconds. The TAF SHA-1, payload length, 114
ordered Ogg chapter offsets and EOS match; end granule minus pre-skip is exactly
4,560 seconds. FFmpeg independently decoded the final signal without errors.
Chrome decoded the final native chapter with its expected duration and end signal.
Controlled output-limit and OPFS `QuotaExceededError` failures produced no upload.

Sampled main-page JS heap peaked around 94 MB in both passes; worker JS/backing
storage did not grow with accumulated output. These are browser heap observations,
not a measurement or guarantee of total Chrome process RAM. Large single inputs
remain constrained by decoding memory.

Two pre-existing legacy reader details remain outside this encoder change:
`readTrackPositions()` reports 4,559 seconds for this TAF because it subtracts the
first audio page granule, and TAF-to-TB2 remux may retain 3.5 ms of final packet
padding here. Neither means missing encoded audio. Native direct output avoids
that TAF remux path. The legacy native-to-TB1 fallback also retains its separate
TAP source-count guard; this change targets direct TB2 native playback.
