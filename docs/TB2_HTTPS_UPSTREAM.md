# TB2 HTTPS 1:1 Capture Forwarder

Phase 1 provides a transparent tunnel for decrypted TB2 HTTPS application bytes. Incoming TLS terminates at TeddyCloud; a second mutually authenticated TLS connection uses the TB2 identity selected for the matching box overlay. No HTTP parser, local handler, cache, redirect, token reconstruction, or response generator participates in a tunneled connection.

## Settings

| Setting | Default |
|---|---|
| `cloud.tb2_enabled` | `false` |
| `cloud.tb2_passthrough_enabled` | `false` |
| `cloud.remote_hostname_tb2` | `tbs2.tonie.cloud` |
| `cloud.remote_port_tb2` | `443` |
| `cloud.tb2_capture_dir` | `data/diagnostics/tb2-https-passthrough` |
| `cloud.tb2_capture_max_mib` | `4096` |

`cloud.tb2_enabled` enables the TB2 HTTPS upstream path. The transparent capture
forwarder starts only when `cloud.tb2_passthrough_enabled` is also enabled.
Disabling the parent switch automatically disables the passthrough switch. TB1
HTTPS continues to use `cloud.enabled`. A missing or mismatched per-box CA,
client certificate, or private key prevents the TB2 tunnel instead of falling
back to a different identity.

TB1 and TB2 use separate global client identities and separate settings:

| Generation | Settings | Default directory |
|------------|----------|-------------------|
| TB1 | `core.client_cert_tb1.file.*` | `certs/client_tb1` |
| TB2 | `core.client_cert_tb2.file.*` | `certs/client_tb2` |

Configuration version 19 migrates the legacy `core.client_cert.*` TB1 values to
`core.client_cert_tb1.*` once and no longer writes the legacy IDs. Box overlays
keep their physical `ca.der`, `client.der` and `private.der` below the
box-specific `core.certdir`.

The classic Boxine cloud callback is destination-specific: it always uses the
overlay's TB1 identity and falls back only to the global TB1 identity. It does
not select a certificate from `toniebox.boxGeneration`. The TB2 tunnel remains
strictly bound to the overlay's TB2 identity and may fall back only to the
global TB2 identity. There is no TB1-to-TB2 or TB2-to-TB1 fallback.

## Sensitive capture

Each handled connection creates `session.json` and `traffic.jsonl` below the configured capture directory. Every decrypted chunk is Base64-encoded, written, and flushed before forwarding. The capture is intentionally unredacted and can contain credentials, identifiers, headers, and payloads. Keep this directory private and never publish it.

Rotation counts completed sessions only. A session without `session.json` is considered active or incomplete and is never deleted automatically.

## Status

`GET /api/tb2-https-upstream/status` reports both enable flags,
`disabled|standby|armed|connecting|tunneling|online|error`, target host and port,
byte counters, timestamps, and a redacted error code. The WebUI polls this local
endpoint every five seconds and never probes the upstream itself.

`online` means that the most recent mutually authenticated tunnel ended normally; HTTPS is not represented as a permanent connection.

## Verification boundary

Automated tests must use a local mTLS upstream. They must never contact the real Tonies cloud. Final acceptance with a physical TB2 remains a controlled external test.
