# TB2 HTTPS 1:1 Capture Forwarder

Phase 1 provides a transparent tunnel for decrypted TB2 HTTPS application bytes. Incoming TLS terminates at TeddyCloud; a second mutually authenticated TLS connection uses the matching box overlay's original `core.client_cert.*` identity. No HTTP parser, local handler, cache, redirect, token reconstruction, or response generator participates in a tunneled connection.

## Settings

| Setting | Default |
|---|---|
| `cloud.tb2_enabled` | `false` |
| `cloud.remote_hostname_tb2` | `tbs2.tonie.cloud` |
| `cloud.remote_port_tb2` | `443` |
| `cloud.tb2_capture_dir` | `data/diagnostics/tb2-https-passthrough` |
| `cloud.tb2_capture_max_mib` | `4096` |

TB1 HTTPS continues to use `cloud.enabled`. A missing or mismatched per-box CA, client certificate, or private key prevents the TB2 tunnel instead of falling back to a different identity.

## Sensitive capture

Each handled connection creates `session.json` and `traffic.jsonl` below the configured capture directory. Every decrypted chunk is Base64-encoded, written, and flushed before forwarding. The capture is intentionally unredacted and can contain credentials, identifiers, headers, and payloads. Keep this directory private and never publish it.

Rotation counts completed sessions only. A session without `session.json` is considered active or incomplete and is never deleted automatically.

## Status

`GET /api/tb2-https-upstream/status` reports only the enable flag, `disabled|connecting|tunneling|online|error`, target host and port, byte counters, timestamps, and a redacted error code. The WebUI polls this local endpoint every five seconds and never probes the upstream itself.

`online` means that the most recent mutually authenticated tunnel ended normally; HTTPS is not represented as a permanent connection.

## Verification boundary

Automated tests must use a local mTLS upstream. They must never contact the real Tonies cloud. Final acceptance with a physical TB2 remains a controlled external test.
