# TB2 server certificates

TeddyCloud manages two independent box-facing TB2 leaf certificates below the
existing TB2 server CA:

| Service | Hostname setting | Default | Certificate settings |
|---------|------------------|---------|----------------------|
| HTTPS | `core.server_cert_tb2.hostname` | `tbs2.tonie.cloud` | `core.server_cert_tb2.file.crt` / `.key` |
| ICI MQTT | `mqtt_server.hostname` | `ici.tonie.cloud` | `mqtt_server.cert.crt` / `.key` |

Both hostname fields are expert settings and accept only ASCII DNS FQDNs. IP
literals, empty labels, labels longer than 63 characters and labels beginning
or ending with a hyphen are rejected. They are global server settings and
cannot be overridden per box.

For a read-only check of these server roles together with TB1/TB2 upstream and
overlay identities, see [CERTIFICATE_DOCTOR.md](CERTIFICATE_DOCTOR.md).

## Reconciliation

At startup and after a hostname change, TeddyCloud checks the affected leaf:

- certificate and EC private key are readable and match;
- the leaf validates under the currently configured TB2 server CA;
- it is a non-CA certificate with `serverAuth` and is not expired or within 24
  hours of expiry;
- its SAN set exactly matches the required service names.

HTTPS uses the configured hostname plus `tbs2.tonie.cloud`. MQTT uses the
configured hostname plus `ici.tonie.cloud`, `ici.dev.tonie.cloud` and
`ici.stage.tonie.cloud`. Duplicate names are removed. The configured hostname
is also the certificate common name; clients must use SAN validation.

At startup, existing files are never replaced. A completely missing leaf pair
may be generated from an intact CA; a partial or inconsistent set fails closed
with the resolved paths in the log. This prevents a failed initialization from
mixing a newly generated CA with an existing key or leaf.

An explicit hostname change retains the established reconciliation workflow:
TeddyCloud creates and validates a replacement leaf pair signed by the existing
TB2 CA. Changing a hostname never regenerates or replaces that CA.

## Replacement, archive and reload

Generation happens in temporary sibling files. Before activation, existing
leaf files are copied to:

```text
certs/archive/tb2/<UTC timestamp>/<https|mqtt>/
```

The archive contains the previous certificate/key and `metadata.json` with the
reason, hostnames and certificate fingerprints; it never contains key bytes in
metadata or logs. The validated temporary files then replace the active pair.
If replacement or reload fails, the prior pair is restored.

New HTTPS TLS connections read the reloaded TB2 chain. The MQTT listener swaps
its in-memory certificate pair under the same rotation lock. Existing TLS
connections continue unchanged; neither service requires a restart after a
successful leaf rebuild. Runtime-only settings
`core.server_cert_tb2.rotation_status` and
`mqtt_server.cert.rotation_status` expose the latest result in the WebUI.

The reconciliation logs only the service, hostname, reason and result. It does
not log certificate contents, private keys or authentication data.

## Optional SNI selection on the shared box HTTPS port

The global expert setting `core.server.sni_cert_selection_enabled` is disabled
by default. When enabled, TeddyCloud peeks at the ClientHello on the box API
port before the TLS handshake and loads exactly one server identity:

- no SNI extension selects the TB1 HTTPS certificate;
- `tbs2.tonie.cloud` or the configured `core.server_cert_tb2.hostname`
  selects the TB2 HTTPS certificate;
- malformed, empty, multiple or unknown SNI values are rejected before the
  handshake.

The parser uses CycloneTCP's public `socketReceive(..., SOCKET_FLAG_PEEK)` API,
does not consume ClientHello bytes and accepts fragmented TLS records up to a
32 KiB limit. The Web HTTPS listener, ICI MQTT and the transparent TB2 HTTPS
monitor are otherwise unchanged. With the setting disabled, the existing
dual-certificate mode remains active.

After the handshake, a valid 12-hex box ID from the client-certificate subject
is matched to an existing overlay. Once per activation cycle, TeddyCloud then
sets and enables that overlay's `toniebox.boxGeneration` override to the
generation selected from SNI. Repeated connections and restarts do not rewrite
the value. Turning the setting off and on starts a new cycle. Unknown or
unauthenticated clients never change settings, and the HTTP User-Agent detector
does not overwrite SNI-derived generation while this mode is active.
