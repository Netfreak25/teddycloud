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
does the same. The default report is deliberately compact: one line per role
and box overlay, followed by a deduplicated list of findings. Use
`--verbose` when every certificate property, fingerprint and individual check
is needed. `--json` emits the complete machine-readable diagnosis used by the
WebUI. `--help` lists the public options and exit codes.

## Web diagnostics

The same read-only diagnosis is available under **Settings -> Diagnostics**.
The page runs the doctor once when opened and again only when **Check again** is
selected. Its summary, certificate roles, effective box-overlay identities and
findings use the same checks and messages as the command-line report. Findings
show their certificate/configuration path whenever one can be determined. The
**Full details** section groups the already loaded checks by certificate role,
inventory set or overlay. Problems are shown first, while informational and
successful checks remain separately collapsible. Expanding details does not
start a second check.

The backing endpoint is:

```text
GET /api/diagnostics/certificates
```

It invokes the installed doctor through `bash` with the fixed base path
`/teddycloud` and accepts no request parameters. Calling it through `bash` is
intentional because packaged diagnostic scripts are installed read-only and do
not require an executable file mode. Doctor exit codes 0, 1 and 2 are valid
diagnostic results and are returned with HTTP 200. Missing prerequisites,
malformed JSON or output beyond the fixed safety limit are reported as
technical API errors. The endpoint and page do not expose certificate or
private-key contents and cannot repair, upload, move or delete files.

The JSON document has `schemaVersion: 1` and contains:

- the overall result and compact counters;
- all configured certificate roles and effective overlay identities;
- deduplicated findings for the overview, including `scope` and `path`;
- every individual check for the hierarchical detail view, also including
  `scope` and `path`.

All status values are canonical lowercase values: `ok`, `warning`, `error` or
`info`. The WebUI additionally normalizes legacy spellings such as `WARN` or
`warn` so mixed-version deployments never expose translation keys to users.

Unknown additive fields may be ignored. A client must reject an unknown schema
version instead of guessing its meaning.

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
- a leaf chain that verifies against the configured CA where the complete chain
  is locally available;
- `CA:TRUE` on the CA and no CA role on a leaf;
- current validity and an expiry warning within 30 days;
- required DNS SANs for the configured TB2 HTTPS and ICI hostnames;
- TB1/Boxine and TB2/TONIES client generation from the CA identity;
- local server-role consistency independently of the private LAN CA's subject/CN;
- a box CN in either `<12-hex>` or `b'<12-hex>'` form;
- CN, overlay ID and per-box directory agreement where a per-box identity is used.

Only paths, role names, subject/CN, issuer and shortened SHA-256 certificate
fingerprints are printed. Private-key contents, certificate contents and raw
certificate settings are never printed. Raw settings are materialized only in a
mode-0700 temporary directory for validation and are deleted on exit.

Optional build artefacts such as `.srl` and `.csr` are deliberately not required.

Factory client certificates can be issued by an intermediate CA that is not
stored in TeddyCloud. If the configured root and leaf are both unambiguously
from the expected Boxine or TONIES generation, the doctor reports that missing
intermediate as information instead of declaring the identity broken. A wrong
generation, direct-signature failure or key mismatch remains an error.

Local TB1 and TB2 server CAs are not restricted to a subject-name whitelist.
For a normally parseable server set, the doctor reports generation `LOCAL` and
then evaluates CA constraints, keys, leaf signatures, validity and required TB2
SANs independently. A familiar name such as `Teddy CA` is recognized throughout
the inventory, but is not required for a valid private LAN server CA. This does
not relax the separate Boxine and TONIES upstream-client generation checks.

Some preserved TB1 server CAs contain a historical, non-minimal serial-number
encoding rejected by current OpenSSL versions. The doctor accepts this only as
a guarded `LOCAL (Legacy)` warning when the decoded PEM payload is byte-identical
to `ca.der`, OpenSSL's ASN.1 inspection finds exactly that known serial defect,
and the locally issued server leaf matches its private key. The warning states
that modern OpenSSL could not verify the CA key or full certificate chain. Any
other unreadable, structurally damaged or mismatched CA remains an error. The
doctor never normalizes, rewrites or regenerates legacy material.

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

During configuration migration TeddyCloud copies verified legacy material into
the canonical roots, rewrites global and overlay paths and finally archives the
old trees as `certs/.server.bak[.N]` and `certs/.client.bak[.N]`. The complete
backup is retained, but is never considered an active certificate source.

An unarchived `certs/server` or `certs/client` tree is read for diagnostics only.
TB1 server files map to `server_tb1`; legacy `server/ici.pem` and `ici.key` map
to `server_tb2`, while legacy client files map to `client_tb1`. Operational
settings that still reference a legacy root are errors. Hidden backups are
reported as preserved, inactive information.

Identical files at active legacy and canonical locations are reported as
duplicates. Different files for the same relative role are errors, and the
report states whether either path is selected by the effective configuration.

Repeated CA, leaf and private-key files can be normal when a global identity was
copied into a per-box directory. They remain visible in `--verbose` output but
do not create a warning by themselves. TB1/TB2 certificates placed below the
wrong canonical tree are still reported as generation mistakes. Legacy
duplicates are summarized by directory in compact mode and listed file by file
with `--verbose`.

## Status and exit codes

- `OK`: the specific check passed.
- `WARN`: improvement is needed, but no active certificate role is proven broken.
- `ERROR`: invalid material, contradictory configuration or a broken active role.
- `INFO`: role, path or effective-selection context.

The compact summary separates role/overlay status from diagnostic findings.
Verbose mode additionally shows the raw count of all individual checks. Exit
codes are unchanged:

| Code | Meaning |
|---|---|
| `0` | no warnings or errors |
| `1` | warnings, but no errors |
| `2` | at least one certificate or configuration error |
| `3` | missing prerequisite or unreadable base configuration |

Compact example:

```text
Certificate roles
-----------------
  OK    TB2 HTTPS server - active, generation=LOCAL, CN=tbs2.tonie.cloud, certs/server_tb2/teddy-cert.pem
  OK    TB2 ICI-MQTT server - active, generation=LOCAL, CN=ici.tonie.cloud, certs/server_tb2/ici.pem

Findings
--------
  WARN  Legacy client: 3 identical duplicate file(s) (details: --verbose)

Summary
  Roles/overlays: OK=7 WARN=0 ERROR=0 INFO=1
  Findings: WARN=1 ERROR=0
  Run again with --verbose for all individual checks.
  This doctor made no changes.
```

There is intentionally no repair, copy, move or delete mode. Use the report to
correct configuration or certificate placement through the normal TeddyCloud
workflow.

## Safe initialization

Automatic certificate initialization runs only for the global server roles.
An overlay can load its configured identity but can never trigger global
generation. Existing certificate, CA or key files are never overwritten.

A completely empty set may be generated. For a partial set TeddyCloud only
adds material that is unambiguous: a missing DER representation of an intact CA
or a completely absent leaf/key pair signed by an intact CA. A half-present
pair, key mismatch, signature mismatch or differing PEM/DER CA fails closed.
New files are generated and validated under temporary sibling names before
being published; publication failures remove only the newly created files.
