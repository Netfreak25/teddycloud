# TB2 Tonieplay download protocol

This document records how a TB2 downloaded one Tonieplay collection through
the TONIES V3 cloud path. It is based on one interrupted download and the
immediately following reconnect capture. It deliberately separates observed
protocol facts from interpretations and implementation recommendations.

The transparent TB2 proxy was used only to observe the traffic. It is not part
of the content-aware V3 routing or cache described here.

All device, game and content identifiers in this document are placeholders.

## Evidence levels

The following labels are used throughout this document:

- **Observed:** present directly in the captured HTTPS or MQTT traffic.
- **Derived:** calculated from multiple observed values without assuming
  undocumented device behavior.
- **Hypothesis:** a plausible interpretation that the captures do not prove.
- **Recommendation:** proposed TeddyCloud behavior, not a claim about the TB2.

## Evidence set and limitations

The evidence consists of two HTTPS captures and their accompanying MQTT logs:

1. the initial Tonieplay download, stopped when the disc/controller was
   removed;
2. the next connection, during which the TB2 continued downloading objects.

The human-readable logger truncated the 192,395-byte `content-meta` JSON in its
display. The beginning and response metadata are available, but the complete
`content[]` array cannot be reconstructed from this text export alone.

The capture also ends before a final `dataPresent=true`, a final successful
integrity message, or a Tonieplay download-completed event is visible.
Consequently, the traffic proves which payload bytes were transferred, but it
does not prove that the TB2 finally accepted and activated the collection.

## Observed request sequence

### 1. Content metadata

The initial connection begins with:

```text
GET /v3/content-meta/<TONIEPLAY-RUID>
Authorization: <box authorization>
```

TONIES responds with HTTP `200`, `Content-Type: application/json` and a
192,395-byte JSON document. The visible top-level fields include:

```json
{
  "version": 126628231,
  "contentType": "tonieplay",
  "tonieType": "disc",
  "gameId": "<GAME-ID>",
  "tonieplayEngineVersion": "1.0.0",
  "language": "de-de",
  "tonieSalesId": "<SALES-ID>",
  "content": []
}
```

The version above belongs only to the captured example and must not be treated
as a fixed Tonieplay version.

### 2. Manifest entries

Every visible item in `content[]` has object-level download metadata. Fields
observed across the visible entries include:

- `type`
- `filename`
- `name`
- `auth`
- `fileSize`
- `tarOffset`
- optional `labels` with names and timestamps

The first visible item is:

```text
type=tonieplay-mpy
filename=main.mpy
name=tp_<64-hex-characters>
fileSize=140515
```

Many following items are `type=audio` with `.opus` filenames. Later downloaded
objects contain small printable, CSV-like default data such as counters,
choices and boolean flags. Therefore a Tonieplay collection is not merely an
audio playlist: it contains executable MPY data, Ogg/Opus audio and additional
non-audio objects.

The complete manifest was truncated in the text export. The exact `type` and
`filename` values of every small data object are therefore **not proven** by
the available capture.

### 3. Object downloads

The TB2 downloads every object through the same endpoint, regardless of the
manifest `type`:

```text
GET /v3/chapter/tp_<64-hex-characters>?auth=<object-auth>
```

Observed successful responses use:

```text
HTTP/1.1 200 OK
Content-Type: binary/octet-stream
Content-Length: <manifest fileSize>
Accept-Ranges: bytes
```

This applies to `main.mpy`, Opus audio and the small printable data objects.
The endpoint name `chapter` therefore means a generic V3 content object in
this protocol; it must not be interpreted as audio-only.

No request in either capture contains a `Range` header. The server advertises
`Accept-Ranges: bytes`, but resumable Tonieplay downloads are **not proven** by
this evidence.

One observed `auth` value has an additional suffix resembling:

```text
<token>.<RUID>.g<version>.lastc-531-<timestamp>
```

The suffix must be preserved as opaque authentication data. Its meaning is
**unknown**. The name suggests a final-content marker and the number equals the
derived unique object count, but the capture alone does not prove either
interpretation.

### 4. Interruption and reconnect

The first HTTPS capture contains:

- 180 HTTP requests in total;
- one `content-meta` request;
- one `check-ota` request;
- 178 object requests;
- 177 successful object responses;
- one final object request without a captured response.

The successful object payloads in this first segment total 23,945,781 bytes.
MQTT later reports `bytesDownloaded=23945781` for the abandoned download. This
exact match is **derived evidence** that the MQTT counter measures completed
object response payload bytes, at least for this download.

The reconnect HTTPS capture contains:

- no new `content-meta` request;
- 354 object requests;
- 354 HTTP `200` responses;
- 38,855,888 successful payload bytes;
- no `Range` request.

Across both captures there are 532 object requests for 531 unique object
names. The only duplicate is the object whose response was missing at the end
of the first capture; it is requested again and returned in full after the
reconnect.

The successful object bytes across both captures total:

```text
23,945,781 + 38,855,888 = 62,801,669 bytes
```

MQTT reports the same value as `totalSizeBytes`. This is strong **derived
evidence** that the two captures contain one successful response payload for
every object expected by this particular collection. It is still not evidence
of final TB2 activation because the final completion state was not captured.

The absence of a second `content-meta` request shows that the TB2 could continue
this particular download without fetching the manifest again. Whether the
download plan is persisted on storage, retained in another runtime component,
or reconstructed through an unobserved mechanism is a **hypothesis**; the logs
do not distinguish these possibilities.

## MQTT lifecycle observed beside HTTPS

MQTT carries lifecycle and progress information, not the object payloads.
Observed messages include:

```text
state ENDUSER/TONIEPLAY
state ENDUSER/TONIEPLAY/DOWNLOAD
playback/state: contentVersion=<version>, dataPresent=false, downloading=true
TONIEPLAY_DOWNLOAD_STARTED: totalSizeBytes=62801669
GOOD INTEGRITY ... (0/62801669)
TONIEPLAY_DOWNLOAD_STOPPED: endReason=abandoned,
                            bytesDownloaded=23945781
```

Removing the disc/controller stops both the game and its download. The
`GOOD INTEGRITY ... (0/total)` line appears before the object transfer is
complete and therefore must not be treated as proof of a completed or valid
collection.

No captured MQTT payload contains MPY, Opus or state-object bytes. Those bytes
are transferred exclusively over HTTPS in the available evidence.

## What the captures do not prove

The following statements remain explicitly unverified:

- that `tp_<64-hex>` is a SHA-256 of the object bytes; only its shape was
  observed, and no digest comparison was captured;
- that `tarOffset` is the exact on-device storage offset; the field name and
  aligned values make this plausible, but the device filesystem was not part
  of the capture;
- that `main.mpy` is executed directly as MicroPython bytecode; the `.mpy`
  filename, `tonieplay-mpy` type and engine version strongly suggest it, but
  execution was not traced;
- that every Tonieplay collection has 531 objects or the same size;
- that the TB2 always downloads objects in manifest order;
- that HTTP Range is supported by the TB2 client for Tonieplay;
- that the `.lastc-*` suffix marks the last object;
- that the second segment ended in a successfully playable local collection.

## Implemented TeddyCloud cache behavior

The content-aware V3 path implements the following behavior derived from the
capture. These are TeddyCloud contracts; they do not turn the protocol
hypotheses above into observed TB2 facts:

1. Detect Tonieplay through `contentType == "tonieplay"`.
2. Preserve the complete original `content-meta` response. Unknown top-level
   fields and unknown per-object fields must survive unchanged.
3. Treat every entry in `content[]` as a required generic object. Filtering for
   `type == "audio"` would omit `main.mpy` and game-state data.
4. Preserve each entry's `name`, opaque `auth`, `fileSize`, `type`, `filename`
   and remaining metadata. Do not parse authentication by assuming it ends at
   `.g<version>`.
5. Stage each object independently and publish a generation only after every
   manifest object is present with exactly the advertised size.
6. Keep complete staged objects across reconnects. A repeated request for an
   already complete object should reuse it rather than truncate and download it
   again.
7. Keep incomplete objects as non-active `.part` files. The observed TB2 retry
   was a complete `200` transfer, so upstream resume is not required to match
   this capture.
8. Preserve an observed HTTP Content-Type per object. If none was observed,
   serve audio as `audio/ogg` and every other object as
   `application/octet-stream`.
9. Keep Tonieplay collections out of the existing TB2 audio-library importer.
   The separate `toniebox2.cacheTonieplayToLibraryV3` switch archives the raw
   manifest and every heterogeneous object.
10. Do not use MQTT byte counters as the cache completion authority. Completion
    must come from the manifest's complete object set and exact object sizes.

Existing immutable-object and Range-capable HTTP streaming can be reused for
local responses. Supporting Range remains useful defensively, but it is not a
behavior demonstrated by these captures.

## Tonieplay library and reassignment

Complete games are published below
`library/by/contentHash/<hash>` as `format: "tonieplay-v3"`. The directory
contains `library-entry.json`, the byte-exact original `content-meta.json` and
all objects below `objects/`. Unknown and future manifest types are stored as
`.bin`; their manifest `name`, optional `type` and `filename`, object size,
SHA-256 and observed MIME type remain recorded.

The collection hash covers a Tonieplay-specific domain, the raw manifest bytes
and the ordered object sizes and hashes. The raw manifest deliberately includes
the original opaque `auth` values. Identical object bytes with different auth
values therefore produce different library entries, and exported ZIP files
must be treated as sensitive data.

A collection can be assigned only through a TB2 overlay. TeddyCloud validates
the manifest and every object hash before changing the source, enables NoCloud
and Freshness through the normal source lifecycle, clones the raw manifest and
changes only its top-level `version`. Exact object names are served locally;
unknown names return `404` and never fall through to TONIES.

The captures do **not** prove that a Tonieplay collection is portable to an
arbitrary RUID. TeddyCloud permits that reassignment by explicit product
decision, but successful execution on a different RUID remains runtime behavior
to be verified rather than a protocol fact established by these logs.
