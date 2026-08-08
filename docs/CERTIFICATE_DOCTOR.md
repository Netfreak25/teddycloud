# Generation-aware certificate doctor

`verify-tc-certificates.sh` is a read-only diagnostic tool for the complete
TeddyCloud certificate configuration. It recognizes the separate TB1 and TB2
roles, resolves the effective global and box-overlay settings and reports
configuration mistakes without moving, replacing or regenerating files.

Run the copy installed by an official image:

```bash
bash /usr/local/bin/verify-tc-certificates.sh
```

For an unpacked or differently mounted installation, select its root explicitly:

```bash
bash /usr/local/bin/verify-tc-certificates.sh --base-path /teddycloud
```

`--no-color` disables ANSI colors. The standard `NO_COLOR` environment variable
does the same. `--help` lists the public options and exit codes.

## Roles

The doctor treats a certificate according to its actual transport role instead
of assuming that every client CA belongs to Boxine:

| Role | Expected material |
|---|---|
| TB1 HTTPS server | `core.server_cert.*` CA, leaf and private key |
| TB2 HTTPS server | `core.server_cert_tb2.*` CA, leaf and private key |
| TB2 ICI-MQTT server | `mqtt_server.cert.*`, signed by the TB2 server CA |
| TB1 Boxine upstream client | effective `core.client_cert_tb1.*` identity |
| TB2 TONIES upstream client | effective `core.client_cert_tb2.*` identity |
| TB2 ICI-MQTT upstream client | the same effective `core.client_cert_tb2.*` identity |
| Local fake clients | `*.fake.der`, reported separately from genuine upstream identities |

The shared TB2 client identity is intentional: HTTPS requests to TONIES and the
ICI-MQTT upstream use the same global `client_tb2` set unless the box has an
explicit, complete TB2 overlay override. An override of only one or two of CA,
certificate and key is an error; TeddyCloud would otherwise construct a mixed
identity.

## Cryptographic checks

OpenSSL is an explicit runtime dependency of the official images. For every
configured set the doctor checks:

- non-empty, readable PEM or DER input;
- matching certificate and private-key public keys;
- a leaf chain that verifies against the configured CA;
- `CA:TRUE` on the CA and no CA role on a leaf;
- current validity and an expiry warning within 30 days;
- required DNS SANs for the configured TB2 HTTPS and ICI hostnames;
- TB1/Boxine, TB2/TONIES or local TeddyCloud generation from the CA identity;
- a box CN in either `<12-hex>` or `b'<12-hex>'` form;
- CN, overlay ID and per-box directory agreement where a per-box identity is used.

Only paths, role names, subject/CN, issuer and shortened SHA-256 certificate
fingerprints are printed. Private-key contents, certificate contents and raw
certificate settings are never printed. Raw settings are materialized only in a
mode-0700 temporary directory for validation and are deleted on exit.

Optional build artefacts such as `.srl` and `.csr` are deliberately not required.

## Effective configuration

Defaults in the doctor mirror `src/settings.c`. Values from
`config/config.ini` override those defaults, and values from
`config/config.overlay.ini` override the corresponding global values for that
overlay. Certificate data settings take precedence over file settings, matching
the runtime loader.

For every overlay the report shows:

- its ID, canonical box ID and configured `toniebox.boxGeneration`;
- whether the global identity or a complete explicit overlay identity is used;
- the detected certificate generation and any CN/directory mismatch;
- a certificate-based recommendation when `boxGeneration` is unknown and only
  one generation has explicit certificate settings.

The doctor does not invent a generation when the evidence is ambiguous. A
missing certificate on an active path is an error. Missing or invalid material
on a disabled path is informational or a warning so it remains visible without
claiming that the inactive service is broken.

## Canonical and legacy directories

The canonical roots are:

```text
certs/server_tb1
certs/server_tb2
certs/client_tb1
certs/client_tb2
```

The legacy `certs/server` and `certs/client` trees are read for diagnostics only.
TB1 server files map to `server_tb1`; legacy `server/ici.pem` and `ici.key` map
to `server_tb2`, while legacy client files map to `client_tb1`. If a legacy file
has no canonical counterpart, the doctor recommends migration.
Identical files at both locations are reported as duplicates. Different files
for the same relative role are errors, and the report states whether either path
is selected by the effective configuration.

Repeated CA files in per-box directories are normal and are not treated as
identity duplicates. Repeated leaf certificates or private keys are reported,
as are TB1/TB2 certificates placed below the wrong canonical tree.

## Status and exit codes

- `OK`: the specific check passed.
- `WARN`: improvement is needed, but no active certificate role is proven broken.
- `ERROR`: invalid material, contradictory configuration or a broken active role.
- `INFO`: role, path or effective-selection context.

The final line summarizes `OK`, `WARN` and `ERROR` counts. Exit codes are:

| Code | Meaning |
|---|---|
| `0` | no warnings or errors |
| `1` | warnings, but no errors |
| `2` | at least one certificate or configuration error |
| `3` | missing prerequisite or unreadable base configuration |

Example shape (fingerprints and subjects vary):

```text
TB2 ICI-MQTT server
  INFO  ... leaf PEM fingerprint=0123456789ABCDEF ...
  OK    ... SAN contains ici.tonie.cloud
  OK    ... leaf certificate and private key match

Summary
  OK=42 WARN=0 ERROR=0 INFO=18
  This doctor made no changes.
```

There is intentionally no repair, copy, move or delete mode. Use the report to
correct configuration or certificate placement through the normal TeddyCloud
workflow.
