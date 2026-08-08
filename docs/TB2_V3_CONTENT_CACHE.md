# TB2 V3 Local Content Cache

This is the canonical description of TeddyCloud's local TB2 V3 content path. It covers local `content-meta`, immutable chapter objects, generation descriptors, TAP snapshots, compatibility with old local chapter names and HTTP range handling.

The transparent TB2 proxy is not part of this path. Local content routing is implemented by the content-aware V3 cloud path.

## Native TONIES original-content cache

`toniebox2.cacheContentV3` enables a physically separate cache for unmodified
TONIES V3 manifests and every object referenced by `content[]`. It defaults to `false` and can be
overridden per TB2 overlay. The existing local custom-content pipeline below
is evaluated first and is not changed by this cache.

The native cache uses the existing schema and layout below
`settings->internal.cachedirfull`:

```text
v3-native/
  staging/<overlay>/<CANONICAL-RUID>/<version>/
    manifest.json
    descriptor.json
    chapters/<original-name>.part
  versions/<overlay>/<CANONICAL-RUID>/<version>/
    manifest.json
    descriptor.json
    chapters/<original-name>
  active/<overlay>/<CANONICAL-RUID>.json
```

Manifest bytes, content version and safe original TONIES object names remain
unchanged. An object is first written to `.part`, checked against the manifest
size and renamed inside staging. The generation directory is published under
`versions` only after every expected object is complete; the active marker is
written last. A failed or interrupted new generation therefore cannot replace
the previous active generation. Complete older version directories are retained.

For an original Tonie without a configured local source, a complete active
manifest and its chapters are served locally. A cache miss or an RUID already
marked stale by the existing Freshness implementation is forwarded to TONIES
and captured. The Freshness algorithm itself is not replaced or extended by
the native cache. Local `teddycloud_` chapter names never enter this store and
never fall through to TONIES.

## Native TB2 library import

`toniebox2.cacheToLibraryV3` defaults to `false` and is effective only while
`toniebox2.cacheContentV3` is enabled. Only a complete active original
generation may be imported. Private TAF sources are not eligible.

The common library keeps TB1 TAFs and TB2-native collections physically
separate:

```text
<library>/
  by/audioID/<audio-id>.taf
  by/contentHash/<collection-hash>/
    library-entry.json
    chapters/teddycloud_<chapter-sha256>_<index>.opus
  .tb2-native-staging/
```

The collection hash is derived from the ordered chapter bytes, sizes and
indices. RUID and overlay are provenance stored in `library-entry.json`, not
part of the reusable content identity. Identical content from another RUID or
overlay therefore reuses the existing collection. Import copies every chapter
to hidden staging, compares the copy, writes metadata last and then renames the
complete directory. Existing inconsistent collections are rejected rather
than overwritten.

`library-entry.json` and Tonieplay `content-meta.json` are protected metadata,
not normal Tonie content sidecars. The generic content-JSON loader rejects
these canonical `by/contentHash/<hash>` paths before parsing or writing them.
This protects the manifest even when an older API or another browser caller
reaches the collection outside the specialized V2 library listing. The WebUI
also makes directory names, including `..`, directly clickable in selection
dialogs; opening a collection can therefore never trap the source picker in
its read-only detail directory.

## Manual generation-aware download

The existing content-download API keeps the TB1 V1/V2 TAF path unchanged. For
a selected TB2 overlay it requests V3 content-meta and every referenced chapter
with that overlay's saved identity and authentication data.

Manual TB2 downloads deliberately reuse the normal cache pipeline:

- `v3_native_cache_meta_capture` validates and stages the original manifest;
- `v3_native_cache_chapter_prepare` validates each advertised chapter and
  writes it into the same staged generation as live MITM traffic;
- the route becomes active only after all advertised chapters are complete.

If authentication, manifest transfer, a chapter transfer or activation fails,
the incomplete staging data is never selected. A previously active complete
generation remains active. Original-content download is rejected while the
selected Tonie has a private source or effective NoCloud protection, so the
manual action cannot bypass the established content policy.

The response reports `objectsCompleted`, `objectsTotal` and `object` while
retaining the existing `chaptersCompleted`, `chaptersTotal` and `chapter`
fields as compatibility aliases. Complete staged objects survive reconnects;
an incomplete `.part` is replaced by the next complete HTTP `200` transfer.
Only one capture may write a given object at a time. Cache failures are logged
but never interrupt the live TONIES response.

## Manual cache/library doctor

`contrib/repair_tb2_native_library.py` is an operator-invoked doctor for native
TB2 library manifests damaged by the former file-browser sidecar bug. Its
default mode is read-only. It scans cache generations once, derives audio and
Tonieplay collection hashes from the exact stored bytes, validates every
archived library object and reports which manifests can be reconstructed or
restored.

An audio collection does not require a remaining V3 cache generation when all
archived chapters use the canonical
`teddycloud_<sha256>_<index>.opus` format. The doctor verifies contiguous
indices, every embedded and actual SHA-256, every size and the collection hash
before reconstructing metadata. A valid pre-repair backup supplies chapter
names and origins. Otherwise the names become `Chapter N`, `origins` remains
empty and the entry is marked `recovered: true`. A regular later import may add
the real origin without replacing the verified chapter files.

`--apply` writes only byte-verified repairs and preserves the damaged manifest
as `library-entry.json.before-browser-repair.bak`. `--recache` additionally asks
the running TeddyCloud API to execute eligible server-side TB2 V3 downloads;
it is rejected unless `--apply` is also present. Recache never changes a
Tonie's source or NoCloud policy. Tags that are not already eligible for an
original-content download remain unresolved instead of being forced through
TONIES.

Differences in Tonieplay collections are reported individually for manifest,
object count and filenames, missing or extra objects, sizes and SHA-256 values.
Normal `--apply` never changes chapter, manifest or object bytes. The explicit
`--apply --restore-files` mode may replace them only from cache generations
whose collection hash and payload are exact and whose duplicate matches are
byte-identical. Before replacement it copies the complete collection to
`library/.doctor-backups/<UTC>/<contentHash>/`, builds and validates a complete
staging directory and swaps the directory atomically. The same explicit mode
can restore damaged audio chapters. Failure leaves the original collection in
place or restores it before returning an error. `--restore-files` can be
combined with `--recache` when a matching generation first has to be fetched.

For Docker, run the doctor in a temporary Python container with the TeddyCloud
volumes attached. Use read-only volumes for diagnosis. Recache needs the live
TeddyCloud container, host networking and an explicitly configured local API
URL. Self-signed local HTTPS requires the explicit `--insecure` flag.

## Tonieplay library collections

`toniebox2.cacheTonieplayToLibraryV3` defaults to `false`, is overlay-capable
and is usable only while `toniebox2.cacheContentV3` is effective. It is separate
from `toniebox2.cacheToLibraryV3`, which remains audio-only.

```text
library/by/contentHash/<tonieplay-hash>/
  library-entry.json
  content-meta.json
  objects/<index>-<object-sha256>.bin
```

The raw `content-meta.json` is stored byte-for-byte, including every unknown
field and opaque object `auth` value. `library-entry.json` uses
`format: "tonieplay-v3"` and records manifest hash and size, content version,
known game metadata, provenance and the ordered generic object list with name,
optional type and filename, hash, size, MIME and path. The collection hash uses
a dedicated Tonieplay domain, the raw manifest and the ordered object sizes and
hashes. Consequently, changed auth values intentionally change the collection
identity.

The FileBrowser exposes the directory as one `tb2_tonieplay_collection` with
Open, ZIP download and Delete actions, but no audio Play action. Its detail view
is read-only. ZIP export includes the raw manifest and therefore warns that
original TONIES auth values are sensitive.

The same canonical `lib://by/contentHash/<hash>/library-entry.json` URI can be
assigned to any valid content RUID of a TB2 overlay. Full manifest, size and
SHA-256 validation happens before assignment. The local V3 response preserves
all manifest values and rewrites only the top-level version to the effective
local Freshness version. Exact object names are resolved from the assigned
collection; any other name receives a local `404`, and NoCloud never falls back
to TONIES. Freshness is confirmed only by MQTT playback of that exact version.

The captured protocol does not prove that TONIES games are RUID-portable. The
cross-RUID assignment is an explicit TeddyCloud capability whose real device
compatibility must be verified separately.

## Assigning a native collection as Tonie content

A complete imported collection can be selected through the existing Tonie
source picker. Its persisted source is content-addressed:

```text
lib://by/contentHash/<64-lowercase-sha256>/library-entry.json
```

Before assignment TeddyCloud validates the descriptor, ordered chapter paths,
sizes and every chapter SHA-256. Invalid input leaves the previous source
unchanged. The assignment then uses the existing source-change, automatic
NoCloud and Freshness hooks; it introduces no second source lifecycle. If an
assigned collection later becomes incomplete or damaged, delivery fails
locally and never falls back to Boxine or TONIES.

For TB2, V3 content-meta references the collection's immutable chapter files
directly. The box-visible names bind the stored chapter hash and index to the
target RUID:

```text
teddycloud_<first-20-chapter-sha256>_<index>_<TARGET-RUID>.opus
```

Chapter requests must match the current source, target RUID and effective
version. Existing immutable-file handling supplies `200`, `206`, `416`,
`Content-Length` and `Content-Range`. No collection chapter is copied into the
original TONIES cache or forwarded upstream.

For TB1, the same independent Ogg/Opus chapters are fed into the existing TAP
packet remux without decoding or re-encoding. The derived TAF is streamed with
its predicted size and published replace-safely at:

```text
<cachedirfull>/tb1-native-library/<collection-sha256>.taf
```

Different Tonies using the same collection share this derived file. A failed
temporary conversion never replaces a valid existing TAF.

`/api/fileIndexV2?special=library&path=by/contentHash` exposes every complete
collection directory as one logical `tb2_native_collection`; its chapters are
not individual source choices. The narrow delete endpoint removes only the
selected collection and its derived TB1 TAF, not the original V3 cache.

## Invariants

- A V3 chapter is an independent Ogg/Opus stream. It is not a TAF file and is never stored with a `.taf` extension.
- Chapter objects are immutable and addressed by the SHA-256 of the exact bytes served to the box.
- A generation is scoped by overlay, canonical uppercase RUID and effective content version.
- A generation descriptor becomes visible only after every referenced chapter object has been prepared and published.
- A local `teddycloud_...` chapter request is never forwarded to TONIES. Unknown, stale or malformed local names receive a local error.
- Growing TAP `.tmp` files and incomplete TAF files are never used as V3 chapter sources.
- Freshness source-change state survives content-meta and chapter downloads. It is cleared only after MQTT playback reports the exact effective content version.

## Storage layout

All paths are below `settings->internal.cachedirfull`:

```text
v3-local/
  <64 lowercase SHA-256 hex>.opus
  generations/
    <overlay>/
      <CANONICAL-UPPERCASE-RUID>/
        <effective-version>.json
```

The object filename contains the complete SHA-256. Objects can therefore be shared safely by multiple overlays, RUIDs or versions when their bytes are identical.

The generation descriptor contains:

- `schemaVersion`
- `overlay`
- canonical uppercase `ruid`
- `effectiveVersion`
- an ordered `chapters` array with `index`, manifest `name`, full `sha256` and `fileSize`

Filesystem paths are not serialized. On load, they are derived from the full digest and validated against the expected size, digest-derived path, name, index and RUID. Audio payloads are not rehashed on every chapter request because objects are content-addressed and published immutably after their digest was calculated during generation.

## Manifest chapter names

New local V3 manifests use:

```text
teddycloud_<first 20 lowercase SHA-256 hex>_<two-digit decimal index>_<CANONICAL-UPPERCASE-RUID>.opus
```

The shortened 20-hex digest is used only in the protocol-visible name. The cache object and generation descriptor retain the full SHA-256.

The digest is calculated from the completed independent `.opus` file, so any change to audio packets, Ogg headers, chapter boundaries or ordering creates a different name. Reassigning different custom content therefore cannot silently reuse the previous local chapter key.

## TAF to independent Ogg/Opus chapters

`v3_local_content_prepare()` validates the complete TAF and its chapter boundaries before creating output. Each TAF chapter is parsed as Ogg packets and remuxed into an independent Ogg stream:

- Opus packets are copied without PCM decoding.
- There is no Opus re-encoding.
- Every chapter receives the required `OpusHead` and `OpusTags` packets.
- Later chapters use a zero Opus pre-skip so the beginning is not discarded a second time after splitting the continuous source stream.
- Repeated TAF chapter block markers are compacted; decreasing or out-of-range markers remain invalid.
- Ogg serial, page sequence and granule positions are generated for the independent chapter stream.
- SHA-256 and `fileSize` cover the exact final bytes written to the `.opus` object.

This replaces the former virtual TAF slicing behavior. The HTTP layer serves a normal immutable file and no longer needs virtual chapter start/end offsets or a synthetic 512-byte prefix.

## Atomic complete generations

Chapter generation has two publication levels:

1. Every chapter is written and flushed to a unique temporary object in `v3-local`.
2. Only after all chapters were prepared successfully are the immutable objects renamed within `v3-local` to their full-digest names; there is no copy fallback into a visible destination.
3. The in-memory generation is exposed only after all object publications succeeded.
4. The generation JSON is written and flushed to a unique temporary descriptor and then renamed to `<effective-version>.json`.

An existing immutable object is accepted only when its bytes are identical. An existing descriptor for the same overlay, RUID and version is likewise accepted only when it is identical. Conflicting content aborts publication instead of overwriting an active generation.

This is not a multi-file filesystem transaction: a failure late in publication can leave an unreferenced immutable object. It cannot expose an incomplete active generation because the descriptor is published last and loaded descriptors validate every referenced content-addressed path and file size.

Temporary object and descriptor names are never returned in content-meta and never loaded as generations.

## Content-meta flow

For local content, `handleCloudContentMetaV3()` obtains the effective version used by freshness and then:

1. tries to load `generations/<overlay>/<RUID>/<version>.json`;
2. validates the descriptor plus each immutable object's content-addressed path and size without rereading all audio bytes;
3. prepares the complete local generation when no valid descriptor exists;
4. persists the descriptor;
5. sends all content items from that generation;
6. records the RUID in `internal.v3HashedChapterUids` only after the hashed content-meta response was written successfully.

An explicitly assigned local source is authoritative. If its complete generation cannot be prepared, TeddyCloud returns a local server error instead of silently substituting TONIES content. Normal cloud fallback remains available only where no authoritative local source or NoCloud rule forbids it.

## Live TAP snapshot

`getTonieInfo()` distinguishes two TAP states:

- `CT_SOURCE_TAP_CACHED`: the complete final TAF is current and can be materialized directly.
- `CT_SOURCE_TAP_STREAM`: the request-local `contentPath` points to the growing `<final>.tmp` file.

The V3 path never reads the growing `.tmp` as a local generation. For `CT_SOURCE_TAP_STREAM`, `handler_cloud.c` holds the existing TAP target lock and materializes one already selected runtime order:

1. runtime indices, including shuffle selection, are chosen exactly once;
2. the existing TAP generator creates one complete TAF snapshot;
3. the snapshot is validated and replace-safely published to the final TAP path;
4. only the complete final TAF is reopened and passed to the V3 chapter materializer.

The returned source path is always the final TAP path, never `.tmp`. The target lock remains owned by `handler_cloud.c` for generation, publication, reopening and V3 materialization, preventing a parallel request from replacing the snapshot while it is being read.

The selected order and its chapter metadata are stored in the local generation
descriptor. A small state file below `v3-local/tap-state/<overlay>/<RUID>.json`
points to the prepared, playing and immediately previous versions. Dynamic TAP
playlists only mark a new selection pending when MQTT reports the end of the
current playback cycle. This state never confirms or removes the normal
source-change freshness marker.

This synchronous V3 snapshot does not change the existing progressive TAP behavior used outside the V3 local-content path.

## Chapter request flow

Hashed chapter requests are resolved only from the descriptor for the current overlay, RUID and effective version. The requested canonical name must exactly match one descriptor entry. A missing descriptor, stale version, unknown hash, wrong index, wrong RUID or invalid object returns `404` locally.

The selected immutable `.opus` object is sent through TeddyCloud's existing unchanged file-streaming path. No local request is reconstructed from a TAF during hashed delivery, and no failed local request falls through to the TONIES chapter endpoint.

## Legacy transition gate

Older local manifests used:

```text
teddycloud_<two-digit decimal index>_<RUID>
```

Such a request is accepted only while both conditions are true:

- the RUID is not present in `internal.v3HashedChapterUids`;
- no source-change freshness entry is pending for the RUID.

The requested legacy index is then mapped to a freshly prepared local generation. Once TeddyCloud successfully sends a hashed manifest for the RUID, the transition is persistent and legacy names are rejected.

A legacy request with `Range.start > 0` receives:

```text
416 Range Not Satisfiable
Content-Range: bytes */<current chapter size>
```

No new body bytes are sent. This forces the box to discard an old partial download instead of appending bytes from the newly selected content to a stale `.part` file.

## Immutable range semantics and `.part` protection

Hashed objects use the normal immutable-file range implementation:

- valid resume requests read from the same immutable object;
- response length and `Content-Range` are calculated from that object;
- a start offset outside the object receives `416` with `Content-Range: bytes */<object size>`.

Protection against mixed client-side `.part` data is layered:

- changed content produces a new hash name and therefore a new client cache key;
- the active generation descriptor binds that name to one overlay, RUID and version;
- stale hashed names receive `404` rather than another generation's bytes;
- resumed legacy names receive `416` before any bytes are appended;
- server-side temporary objects and TAP `.tmp` files are never advertised.

## Freshness relationship

The generation key uses the effective V3 version, including an overlay-local forced version after a source change. If the assigned source is not materialized when the mapping changes, the forced version is allocated as soon as the local content becomes readable; no manifest is emitted with the old version in between. TB2 compares the raw content versions for inequality and does not apply the TB1 custom-Audio-ID normalization. Content-meta and chapter requests do not prove that the box activated the new generation, so they do not clear a pending source-change marker.

The marker is cleared only when the local MQTT observer receives playback state for the same RUID and the reported `contentVersion` exactly equals the current effective version. Stale or unknown playback versions are ignored.

## Source and file map

### `include/v3_local_content.h`

- Defines immutable chapter and complete-generation descriptors.
- Exposes prepare, save, load, exact-name lookup and ownership APIs.
- Keeps V3 object/cache contracts separate from HTTP request state.

### `src/v3_local_content.c`

- Validates TAF chapter boundaries.
- Remuxes independent Ogg/Opus chapters without re-encoding.
- Calculates hashes from exact output bytes.
- Publishes immutable objects with a same-directory rename and never falls back to copying into a visible final path.
- Publishes generation descriptors safely after every chapter object exists.
- Strictly validates descriptor fields, content-addressed paths and object sizes on load without rehashing the complete Tonie for every chapter request.

### `include/tonie_audio_playlist.h`

- Declares synchronous TAP snapshot materialization.
- Documents that the caller owns target serialization and receives only the borrowed final path.

### `src/tonie_audio_playlist.c`

- Reuses the existing TAP generator with one runtime-index selection.
- Publishes the complete snapshot through the existing replace-safe final-TAF path.
- Cleans temporary snapshot files without exposing them as V3 sources.

### `src/handler_cloud.c`

- Selects the local source and effective version.
- Holds the TAP target lock around snapshot preparation.
- Loads or creates generation descriptors for content-meta.
- Parses hashed and legacy chapter names, applies the transition gate and keeps local misses local.
- Returns `416` for resumed legacy requests and delegates hashed objects to the existing unchanged immutable-file streamer.

### `src/contentJson.c`

- Rejects native `library-entry.json` and Tonieplay `content-meta.json` paths
  before the generic content-sidecar migration can rewrite them.
- Keeps recovery explicit: an already damaged manifest remains the cache/library
  doctor's responsibility and is never guessed from directory contents.

### `include/settings.h` and `src/settings.c`

- Persist `internal.v3HashedChapterUids`, the per-RUID transition marker used by the legacy gate.
- Keep the marker internal rather than exposing protocol migration state as a user setting.

### `tests/test_tb2_v3_local_content_contract.py`

- Protects the hash-name, full-digest object, complete-generation, remux, strict local-routing, legacy-gate, range and freshness contracts.
