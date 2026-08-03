# TB2 HTTPS upstream modes

TeddyCloud supports two explicit TB2 HTTPS upstream modes. They use the same
TB2 target and client-certificate selection, but they process traffic at
different layers:

| Mode | Setting | Behaviour |
|---|---|---|
| Transparent proxy | `cloud.tb2_enabled` | Terminates the incoming box TLS connection and forwards the decrypted application bytes unchanged over a second mutually authenticated TLS connection. No HTTP parser or local cloud handler participates. |
| v3 cloud path | `cloud.tb2_v3_enabled` | Handles supported TB2 v3 endpoints locally and forwards enabled requests through TeddyCloud's HTTP client. |

Both settings default to `false` and are mutually exclusive for each settings
scope. Enabling one mode disables the other mode in that same scope. A transport
error never changes the selected mode, so failed POST requests cannot be sent a
second time through an automatic fallback.

TB1 HTTPS remains independent and continues to use `cloud.enabled`.

## Settings

| Setting | Default | Purpose |
|---|---:|---|
| `cloud.tb2_enabled` | `false` | Enable the transparent TB2 HTTPS proxy. |
| `cloud.tb2_v3_enabled` | `false` | Enable the handled TB2 v3 cloud path. |
| `cloud.tb2_capture_enabled` | `true` | Capture transparent-proxy traffic without changing transport behaviour. |
| `cloud.remote_hostname_tb2` | `tbs2.tonie.cloud` | Shared TB2 upstream hostname. |
| `cloud.remote_port_tb2` | `443` | Shared TB2 upstream port. |
| `cloud.tb2_capture_dir` | `data/diagnostics/tb2-https-passthrough` | Transparent capture directory. |
| `cloud.tb2_capture_max_mib` | `4096` | Maximum completed capture storage. |

The v3 endpoint switches are subordinate to `cloud.tb2_v3_enabled`:

- `cloud.enableV3FreshnessCheck`
- `cloud.enableV3Ota`
- `cloud.enableV3SetupStatus`
- `cloud.enableV3ContentMeta`
- `cloud.enableV3Chapter`

Global settings provide the default. A TB2 box overlay may explicitly select a
different mode; an explicit overlay value wins over the global value. TB1 box
overlays do not expose TB2 settings, and TB2 overlays do not expose TB1
settings.

## Configuration migration

Configuration version 20 maps the former settings without silently changing
the active transport:

- A legacy transparent proxy remains active only when both former
  `cloud.tb2_enabled` and `cloud.tb2_passthrough_enabled` were enabled.
- Otherwise a formerly enabled handled cloud path becomes the v3 mode.
- Legacy overlay values that the old transparent forwarder never evaluated are
  not promoted into active per-box proxy modes.
- If both new modes are present as enabled, normalization keeps the transparent
  proxy and disables v3.

`cloud.tb2_passthrough_enabled` remains an internal, load-only migration input.
It is not public through the settings index and cannot be changed through the
settings API.

## Shared TB2 client identity

Both modes resolve their outbound mutual-TLS identity through the same helper.
The global `core.client_cert_tb2.*` certificate set is the default. A box uses
its own TB2 certificate only when at least one TB2 certificate setting is
explicitly overlaid. An incomplete explicit override fails closed and never
falls back to the global identity.

TB1 and TB2 identities remain separate:

| Generation | Settings | Default directory |
|---|---|---|
| TB1 | `core.client_cert_tb1.file.*` | `certs/client_tb1` |
| TB2 | `core.client_cert_tb2.file.*` | `certs/client_tb2` |

There is no TB1-to-TB2 or TB2-to-TB1 certificate fallback.

## Transparent raw capture

When `cloud.tb2_capture_enabled` is enabled, every transparent session creates
`session.json` and `traffic.jsonl` below the configured capture directory. Each
decrypted chunk is Base64-encoded, written and flushed before it is forwarded.
Disabling capture does not disable or alter the transparent transport.

The capture is intentionally complete and unredacted. It can contain
credentials, identifiers, headers and payloads. Keep the directory private and
never publish it. Rotation counts completed sessions only; a session without
`session.json` is considered active or incomplete and is never deleted
automatically.

The v3 path is request-aware and is not written into this transparent raw
capture.

## Status and WebUI

`GET /api/tb2-https-upstream/status` reports the effective mode across active
TB2 overlays as `disabled`, `v3`, `transparent` or `mixed`. Its aggregate state
is one of:

- `disabled`
- `ready`
- `request_active`
- `connecting`
- `tunneling`
- `success`
- `error`

The response also contains `mode_counts` plus independent `v3` and
`transparent` objects. The v3 object exposes active requests, the last endpoint,
HTTP status, timestamps and error code. The transparent object exposes active
sessions, byte counters, timestamps and error code.

The WebUI uses the shared `TON`/`TONIES` navbar status for whichever TB2 HTTPS
mode is active. Its tooltip shows both the selected mode and current state. The
settings view groups cloud options under `Global`, `TB1` and `TB2`; v3 endpoint
switches appear beneath the v3 master switch.

## Verification boundary

Automated transport tests must use a local mutual-TLS upstream. They must never
contact the real Tonies cloud. Final acceptance with a physical TB2 remains a
controlled external test.
