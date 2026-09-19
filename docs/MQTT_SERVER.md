# Internal MQTT Server / ICI Endpoint

This document describes the internal MQTT server code path used for Toniebox
connections to the ICI endpoint. It intentionally excludes the external MQTT
client and Home Assistant broker integration implemented in `src/mqtt.c` and
documented in `MQTT_CONTROL.md`.

## Summary

The internal server is a small, embedded MQTT endpoint for box-facing traffic.
It is not a general MQTT broker. It accepts box connections, maps them to a
TeddyCloud overlay, stores the subscriptions needed for direct replies and
publishes only the box-specific topics implemented in `src/mqtt_server.c`.

The server-side ICI settings are:

| Setting | Default | Purpose |
|---------|---------|---------|
| `mqtt_server.enabled` | `false` | Starts the internal MQTT server when enabled. |
| `mqtt_server.hostname` | `ici.tonie.cloud` | DNS name for the box-facing ICI listener and its server certificate. |
| `mqtt_server.port` | `8883` | TCP port used by the internal ICI/MQTT listener. |
| `mqtt_server.cert.crt` | `certs/server_tb2/ici.pem` | PEM server certificate loaded for the TLS endpoint. |
| `mqtt_server.cert.key` | `certs/server_tb2/ici.key` | PEM server private key loaded for the TLS endpoint. |
| `mqtt_server.cert.rotation_status` | runtime only | Result of the latest certificate check or rebuild. |
| `mqtt_server.log_full_payloads` | `false` | Expert diagnostic switch for base64 full-payload capture of large MQTT publishes. |
| `mqtt_server.log_connect_details` | `false` | Logs the MQTT CONNECT structure and plain client ID at global log level 5; credentials and Will fields remain masked. |

Upgrades migrate the former `certs/server/ici.pem` and `ici.key` pair to
`certs/server_tb2/`, then rewrite the persisted settings. The complete legacy
server tree is retained as a numbered hidden `certs/.server.bak[.N]` backup;
it is not a runtime fallback. Custom certificate paths outside TeddyCloud's
legacy certificate roots are never rewritten.

The ICI listener is fail-closed. TeddyCloud does not bind its MQTT port when
either resolved PEM path is missing, unreadable or empty. A TLS context/setup
failure on an accepted socket closes that socket before MQTT parsing; encrypted
TLS bytes are therefore never treated as plaintext MQTT packets.

The separate **MQTT Client Upstream** category controls the optional,
packet-aware TB2 ICI upstream MITM:

| Setting | Default | Purpose |
|---------|---------|---------|
| `mqtt_client_upstream.enabled` | `false` | Enables packet-aware upstream forwarding, observation, filtering and capture as one global mode. |
| `mqtt_client_upstream.local_control_enabled` | `false` | Allows local `app-control/*` publishes alongside permitted TONIES commands. A command blocked at the Internet boundary remains locally usable without this exception. Permanent device settings use the separate ownership rules below. The value can be overridden per TB2 overlay. |
| `mqtt_client_upstream.port` | `8883` | Tonies ICI upstream MQTT port. |
| `mqtt_client_upstream.hostname` | `ici.tonie.cloud` | Tonies ICI upstream hostname. |
| `mqtt_client_upstream.capture_dir` | `data/diagnostics/tb2-mqtt-passthrough` | Local session capture directory. |
| `mqtt_client_upstream.capture_max_mib` | `4096` | Maximum total size of completed captures. |
| `mqtt_client_upstream.forward.*` | `true`, except `forward.other=false` | Permits the selected PUBLISH class across the Internet boundary in both directions. `false` suppresses only its relay copy, not local processing. |

The former `mqtt_client_upstream.passthrough_enabled` option is internal,
load-only migration input. Upgrades enable the unified mode only when both old
switches were enabled; every other old combination migrates to disabled. The
status API temporarily returns `passthrough_enabled` as a deprecated alias of
`enabled` for older WebUI clients.

These settings are deliberately separate from both the generic external
`mqtt.*` client and the internal TB2-facing `mqtt_server.*` listener.
The internal `mqtt_server` remains the incoming TLS endpoint. After that TLS
handshake and before MQTT packet parsing, the enabled MITM maps the presented
box certificate to an existing TB2 overlay and opens a second TLS connection using the
original per-box identity from `core.client_cert_tb2.*`. It must never fall back
to the TB1 `core.client_cert_tb1.*` identity. The local `mqtt_server.cert.*` ICI
identity is never reused for the outbound role.

For Docker installations, publish TCP port `8883` unchanged when the internal
ICI endpoint is enabled. Split DNS must resolve the box-facing ICI hostname to
TeddyCloud, while DNS inside the TeddyCloud container must continue to resolve
the public TONIES ICI and HTTPS hostnames. Pointing the container itself back to
TeddyCloud would create a forwarding loop instead of an upstream connection.

For an enabled connection, decrypted MQTT application bytes are reassembled into
complete MQTT packets in both directions. Fragmented packets and multiple
packets in one TLS read are supported up to MQTT's own Remaining Length limit;
there is no additional proxy packet-size limit. Packets that do not match a
disabled forwarding option are sent byte-for-byte unchanged. The box's original
CONNECT data, including any ICI credential it carries, is therefore still
forwarded without reconstruction.

Manual MQTT filters control only PUBLISH traffic across the TONIES Internet
boundary, in both directions; they do not switch off local status processing or
locally generated delivery to the box. The decision API explicitly distinguishes
`box_to_tonies`, `tonies_to_box`, and `local_to_box`. Packet origin comes from the
connection or the local sender, never from a payload or topic claiming to be
local. Existing permissions for locally generated settings and controls still
apply before sending; an Internet-filter exemption does not grant control access.

All `mqtt_client_upstream.forward.*` options default to `true`, except the
top-level `mqtt_client_upstream.forward.other`, which defaults to `false`.
Setting a rule to `false` suppresses only its matching Internet-boundary copy.
The available classes are `claim`, `volume`, `bi_events`, `fresh_tonies`, `setup`,
individual log sources plus `logs.other`, and the listed children plus `other` for `metrics`,
`app_reply`, `settings`, `playback`, and `app_control`. Matching is performed on
`toniebox/<id>/...` topic segments. Log `source` values are read exactly and
case-sensitively from valid JSON; missing, invalid, or unlisted sources use
`logs.other`. A group's `other` option applies only to that group root and
children with no dedicated option. `forward.setup` covers `setup` and its
descendants, including the locally observed `setup/status`; its default is
`true`. The top-level `forward.other` covers otherwise unclassified PUBLISH
topics, including topics outside `toniebox/<id>/...`. It does not override a
recognized group's rule. Existing installations without the new keys use these
defaults: setup remains allowed, previously unclassified PUBLISH traffic is
suppressed until explicitly allowed. Both new rules support normal box overrides
and appear through the existing API-driven WebUI filter section.

All forwarding options are overlay-capable. An explicit box value wins;
otherwise the current global value is read for every packet, so a global change
also affects existing sessions immediately. In the WebUI, each box filter has
an explicit override switch. With the switch off, its forwarding state is
inherited from the global setting and cannot be edited locally. With it on,
the box can select `Forward` or `Suppress`.

`Reset all filters to global values` clears the overrides for the entire MQTT
filter section, including rules hidden by search or collapsed groups. It does
not reset global settings to factory defaults or change other box settings.
Both individual changes and this section reset take effect only after `Save`;
`Discard` restores the previous values and override flags. The existing
settings-reset API removes the saved box overrides, so future global changes
continue to apply. Automatic NoCloud protection is unaffected.

The global `mqtt_client_upstream.filters_enabled` master switch (default `true`)
enables or bypasses all manual forwarding rules, including box overrides. It
cannot itself be overridden per box. Disabling it does not modify the saved
rules; re-enabling restores their effect. Existing sessions read this global
setting for every packet, so no reconnect is necessary after saving. The WebUI
shows the switch in the global MQTT filter section and an inactive notice in
both global and box filter views. Individual rules remain editable while the
master is off. This switch does not bypass automatic NoCloud/`teddycloud_`
protection, local status processing or local response correlation.

The evaluator returns the selected route, action (`forward`, `block`, or
`local`), setting ID when a rule was selected, and a reason. A master bypass is
reported separately from an allowed rule. A locally consumed reply or automatic
NoCloud rejection is recorded as a manual decision that was not evaluated; it is
not mislabeled as a manual-filter block.

After local observation and response correlation, the proxy applies an
automatic selective NoCloud policy before the manual forwarding switches. It
therefore cannot be bypassed by a forwarding option. No additional setting is
required. A valid 16-hex rUID is accepted case-insensitively and protected for
the effective box overlay when its current content JSON has `nocloud=true` and
`cloud_override=false`. The lookup reads only these two JSON fields for each
unique rUID in the current packet; it does not load TAF headers, playlists or
sources and it keeps no persistent cache. A missing content JSON is treated as
an unknown cloud Tonie. An existing but unreadable JSON or invalid policy field
is fail-closed for that rUID. Reserved TB2 system rUIDs beginning with
`00000AF0` are classified separately and never inherit a Tonie NoCloud policy.

Independently of NoCloud, every box-to-TONIES payload is scanned as bounded
binary data for the local chapter prefix `teddycloud_`. A match suppresses the
whole publish with filter ID `local_content.teddycloud_payload`, even when all
manual forwarding options are enabled. Locally generated server-to-box
settings, controls and freshness publishes are not upstream relay traffic and
do not enter this filter.

The automatic policy applies in these directions:

- Box to cloud: a protected `claim/<ruid>` or `playback/state` is suppressed;
  `{"tonie":null}` remains visible. Entries with a protected direct `tonie`
  field are removed from `metrics/fleet`, `metrics/events` and `bi-events`
  arrays. This removes playback metrics `1000` and `1001` as whole objects,
  including chapter, duration, play-time, content-version and end-reason data.
  A raw log publish is suppressed when it contains an explicitly bounded,
  case-insensitive 16-hex token for a protected rUID. In a parseable log array,
  only affected entries are removed and the safe remainder is forwarded.
- Cloud to box: protected rUIDs are removed from `fresh-tonies` single objects,
  string arrays and object arrays. TeddyCloud's locally generated freshness
  publishes bypass this relay policy.

Unrelated array entries and rUID-less data remain byte-identical unless at least
one item is removed. A non-empty partial result is rebuilt with the original
PUBLISH flags, topic, QoS and packet ID. An empty result is suppressed. Invalid
JSON, allocation failure or rebuild failure on these structured topics is also
suppressed without closing the MQTT session. The original QoS 1/2 publish is
then completed using the same local acknowledgement state as a manual filter.
When response correlation and NoCloud protection both rewrite a publish, the
final payload after both stages is used for the wire packet, capture and stored
local-response replay. The earlier observer payload must not replace a later
automatic privacy rewrite.

Suppressed QoS 0 publishes are dropped. For QoS 1 the proxy returns `PUBACK` to
the sender. For QoS 2 it keeps independent packet-ID state per direction,
returns `PUBREC`, and completes matching and duplicate `PUBREL` packets with
`PUBCOMP`. ACKs and `PUBREL` packets that do not belong to a locally suppressed
publish are forwarded unchanged.

The proxy also observes box `SUBSCRIBE` and `UNSUBSCRIBE` packets without
generating `SUBACK` or `UNSUBACK`. This gives the local server an accurate view
of the box subscriptions while the original packets and acknowledgements remain
transparent. TeddyCloud may send a matching local server-to-box publish before
the upstream `SUBACK`, as permitted by MQTT 3.1.1.

TeddyCloud observes box-to-cloud publishes before the forwarding decision. It
processes claims and the passive status paths for settings confirms, setup,
battery/events/fleet/headphones metrics, playback, volume, and existing
app-reply status handlers even when the publish is suppressed. A direct local
MQTT connection retains the normal local-control behavior. On a proxy
connection, permission is evaluated separately for each `app-control/*` topic:
a command blocked from TONIES remains locally usable, while a command permitted
from TONIES additionally requires the effective global or overlay value of
`mqtt_client_upstream.local_control_enabled` for local use. Permanent device settings
are controlled by effective Desired forwarding, not this exception switch.
Local `fresh-tonies` delivery is independent of both settings decisions.

Matching responses to local proxy commands are handled before the automatic
NoCloud/payload protection and manual forwarding filters. Matching local
`settings/confirm` revisions are removed from the payload; an entirely local
confirm is consumed, while unknown or cloud-owned fields are rebuilt and sent
to TONIES. A `pong` is local only when its `requestId` exactly matches the
unexpired local ping on that connection. A `bedtime-state` reply also needs a
matching pending STL operation, not just a nearby timestamp. Both use a
30-second correlation window; details and ambiguity handling are below. Alarm replies and all
unmatched, invalid or stale responses remain transparent. Correlations that
were already pending continue to be consumed after the switch is disabled and
are discarded when the MQTT connection ends. Cloud commands are not executed
locally; successfully delivered Cloud Desireds are additionally stored without
emitting a second command. For QoS 1 and QoS 2, the
proxy remembers a bounded set of local consume/rewrite decisions. A duplicate
PUBLISH therefore reuses the original decision before correlation runs again
and cannot leak to TONIES after the first packet cleared the pending action.

The Internet-filter boundary does not yet make the box session independent of
TONIES. Upstream failures can still close that session. Proxy-mode
`settings/request` ownership is described below. CONNECT/CONNACK,
subscriptions, keepalive and ACK packets are not
manual PUBLISH-filter categories. Required ACKs for locally suppressed packets
and existing local freshness ACK consumption are unchanged. No new application
responses are fabricated for unknown topics. Native originalcache routing,
private content, source assignment and general freshness behavior are outside
this filter-boundary change.

At global log level `5`, connection diagnostics use the filter prefix
`TB2 MQTT upstream`; packet decisions use `TB2 MQTT proxy`. They report the DNS,
TCP, TLS and packet-forwarding stage, destination address, direction, MQTT packet
type/topic/QoS, the selected forwarding setting, and textual and numeric error
codes. Certificate, key and credential contents are not written to these logs.
After a successful upstream TLS handshake, `stage=client_auth` reports whether
the ICI server sent a TLS CertificateRequest and whether CycloneTLS answered
with the configured TB2 certificate, an empty certificate list, or no client
certificate because none was requested. The log also marks resumed sessions.
The incoming box certificate is logged as `stage=box_client_auth` before
passthrough selection and is resolved through the same canonical CN-to-overlay
mapping used by the normal MQTT server path.
The subject must contain an exact 12-hex box ID, either raw or as `b'<ID>'`, and
must match an existing active TB2 overlay case-insensitively. It never creates
a new overlay. The issuer is logged but is not an admission criterion. Because
the TLS adapter tolerates unknown CAs, this is identity mapping rather than
complete client-certificate authentication: a certificate from an unknown
issuer can map to a known TB2 overlay when it carries that overlay's box ID.
The outbound client identity defaults to the global `core.client_cert_tb2.*`
settings. A box-specific identity is selected only when at least one TB2 client
certificate file or data setting is actively overridden in that box overlay;
`stage=select_identity` reports the selected source. Newly discovered overlays
keep these TB2 settings inherited instead of activating empty per-box paths.

The separate **ICI Upstream** navbar tag polls
`GET /api/mqtt-client-upstream/status` every five seconds. States are
`disabled`, `ready`, `connecting`, `connected` and `error`; only a connected
upstream is green. The API contains no credential values, certificate
paths, payloads or box identifiers.

Each session writes `session.json` and a full Base64 `traffic.jsonl` capture.

After a newly authenticated connection has completed MQTT CONNECT/CONNACK (or
the transparent initial forwarding), TeddyCloud closes older active sessions
for the same canonical box ID and overlay. Failed or incomplete reconnects do
not displace the working session. Pending freshness remains stored in the
overlay and is retried on the replacement connection.

Accepted TCP connections receive a 300 ms socket I/O timeout before TLS setup.
On Linux, a zero socket timeout would allow a later read inside `tlsRead()` to
block the main loop indefinitely even after a successful readiness check.
An I/O timeout preserves partial TLS records and lets the next loop iteration
continue; it does not itself close the connection.

The main loop closes incomplete TLS/MQTT setups with `establishment timeout`
once it observes 15 seconds since acceptance. This is a loop-checked setup
deadline, not a hard wall-clock limit for an entire TLS call: each socket I/O
has its own timeout. Once ICI forwarding is established, the existing proxy
I/O timeout of 500 ms continues to apply.

The capture is packet-based and records `packet_type`, optional `topic`,
`forwarded`, optional `filter_id`, `generated`, and `packet_complete`.
PUBLISH decisions additionally record `publish_route`, `manual_filter_decision`,
and `manual_filter_id` when a manual rule was selected. A route identifies the
actual ingress or local sender; it is not inferred from `generated`, which also
marks rebuilt received packets. The legacy `direction` field remains compatible,
so a local publish can have `direction=upstream_to_box` together with
`publish_route=local_to_box`. Locally consumed replies and automatic blocks report
why the manual filter was not evaluated. Protocol-only ACK/control captures do
not claim a manual PUBLISH-filter decision.
For a received packet, `data_base64` always contains its original bytes; for a
locally generated packet, it contains the locally produced bytes.
If packet-ID collision handling or a partially local `settings/confirm` changes
the bytes sent on the wire, the entry also contains `wire_data_base64`, the
original and effective packet IDs when applicable, and an `action`. Local
settings, app controls and their reply decisions use the actions
`local_settings_desired`, `local_app_control`, `local_response_consume` and
`local_response_rewrite`. QoS retransmits additionally use
`local_response_replay_consume`, `local_response_replay_rewrite` or
`local_response_replay_block` when a relay filter suppressed the rewritten
remainder. Locally generated ACKs and freshness publishes,
consumed freshness `PUBACK`s, and an incomplete final packet are captured as
well. Consumed local responses and ACKs do not increase suppressed-message
counters. Session and status data
contain separate forwarded/suppressed/rewritten message counters and noCloud
items-removed counters for each direction. A partial noCloud rewrite keeps the
original packet in `data_base64`, stores the actual packet in
`wire_data_base64`, sets `generated=true`, and records `action`, a
`nocloud.*` filter ID and `removed_count`.
Passively handled status packets use `local_status_forward`,
`local_status_manual_block`, `local_status_nocloud_block` or
`local_status_nocloud_rewrite`, showing that TeddyCloud processed the payload
before applying the relay decision. The local-prefix guard uses
`local_content_block` or `local_status_local_content_block` and its
`local_content.*` filter ID.
Capture data is intentionally sensitive, local-only and excluded from Git.

`mqtt_server.hostname` controls the common name and primary SAN of the local
ICI server certificate. DNS/network routing still has to direct that hostname
to TeddyCloud.

## Certificate Identity

`src/cert.c` generates and reconciles the TB2 MQTT/ICI leaf certificate from
the TB2 server CA:

- Certificate subject/common name: current `mqtt_server.hostname`
- SAN DNS names: current hostname plus `ici.tonie.cloud`, `ici.dev.tonie.cloud`,
  `ici.stage.tonie.cloud`
- Certificate setting paths: `mqtt_server.cert.crt` and
  `mqtt_server.cert.key`
- Signing material: `internal.server_tb2.ca` and
  `internal.server_tb2.ca_key`

Duplicate SAN names are removed. A hostname change rebuilds only this leaf,
never the TB2 root CA. The active PEM pair is reloaded for new MQTT TLS
connections without restarting TeddyCloud. See `TB2_SERVER_CERTIFICATES.md`.

## Runtime Lifecycle

`src/server.c` owns the lifecycle:

- `mqtt_server_init()` is called after the HTTP/HTTPS server contexts start.
- `mqtt_server_task()` runs once per main loop iteration after a 250 ms delay.
- `mqtt_server_deinit()` runs during server shutdown.

`mqtt_server_init()` exits immediately when `mqtt_server.enabled` is false. When
enabled, it resolves the configured certificate paths relative to the
TeddyCloud base directory, loads the PEM certificate/key into memory, opens a
TCP socket, binds to `IP_ADDR_ANY:mqtt_server.port` and listens with a backlog
of 5. Readiness is polled without waiting; accepted sockets use the finite
I/O timeout described above.

The implementation currently has these fixed limits:

| Constant | Value | Meaning |
|----------|-------|---------|
| `MQTT_MAX_PACKET_SIZE` | `4096` | Per-connection receive buffer size. |
| `MQTT_MAX_CONNECTIONS` | `32` | Maximum concurrent MQTT connections. |
| `MQTT_MAX_SUBSCRIPTIONS` | `32` | Maximum stored subscriptions per connection. |
| `MQTT_LOG_INLINE_PAYLOAD_SIZE` | `256` | Payload size at which optional full-payload capture replaces inline previews. |
| `MQTT_FRESH_TONIES_DEBOUNCE_SEC` | `2` | Per-overlay coalescing window before a pending `fresh-tonies` publish may be sent. |
| `MQTT_FRESH_TONIES_RETRY_INTERVAL_SEC` | `5` | Delay before retrying an unacknowledged per-rUID freshness publish. |
| `MQTT_FRESH_TONIES_MAX_ATTEMPTS` | `3` | Initial freshness publish plus two retries before closing the connection. |
| `MQTT_CONNECTION_ESTABLISH_TIMEOUT_MS` | `15000` | Maximum time from TCP accept to completed MQTT CONNECT/CONNACK or transparent initial forwarding. |
| `MQTT_SETTINGS_DESIRED_MAX_ATTEMPTS` | `3` | Maximum pending settings publishes before waiting for confirm. |
| `MQTT_SETTINGS_DESIRED_RETRY_INTERVAL_SEC` | `5` | Retry interval for pending settings publishes. |
| `MQTT_APP_CONTROL_REPLY_WINDOW_MS` | `30000` | Monotonic time window for connection-local ping and STL reply correlation. |

## Connection Mapping

Each accepted connection starts with the global settings overlay and the global
box state. It is promoted to a box connection when the server can map it to a
known overlay.

Mapping sources:

- TLS client certificate on MQTT `CONNECT`
- The `<box_cn>` segment in topics matching `toniebox/<box_cn>/...`

The supported certificate common-name formats are the Tonies style
`b'<MAC>'` form and a raw 12-character hexadecimal box common name. The subject
must map case-insensitively to an existing active TB2 overlay. Unknown issuers
are accepted for mapping and logged, but unknown IDs, malformed IDs and
non-TB2 overlays are rejected. A successful lookup sets:

- `conn->client_ctx.settings`
- `conn->client_ctx.settingsNoOverlay`
- `conn->client_ctx.state`
- `conn->box_connection = TRUE`

Twelve-character hexadecimal box identities are canonicalized to uppercase for
new overlays and for the logical connection identity. Existing overlay IDs are
matched case-insensitively and are not renamed. Certificate directories remain
lowercase, so normalizing the logical identity does not move or duplicate
certificate files.

The MQTT namespace identity is tracked separately as `conn->box_topic_id`. It
is learned from concrete `toniebox/<box_cn>/...` publishes and subscriptions and
keeps the exact spelling used on the wire. Locally generated messages use that
topic ID first and an uppercase logical box ID only as a fallback. This matters
because MQTT topic matching is case-sensitive even though certificate and
overlay identity matching is not.

Box-only actions such as settings request/confirm handling require
`conn->box_connection`. Certificate mapping sets that flag immediately when the
certificate subject resolves to a known TB2 overlay. Topic-based mapping may
also promote the connection, but only when that topic resolves to the same
overlay as the presented certificate. An unauthenticated MQTT client therefore
cannot become a box merely by choosing a topic name.

While a mapped box connection is active, the MQTT server updates the existing
WebUI state anchors `internal.online` and `internal.last_connection` on the
matching settings overlay. This makes a TB2 that is only connected through ICI
visible through the same status fields as the older box paths. The global
offline sweep keeps an overlay online while such an active MQTT box connection
exists, instead of treating the last timestamp as a one-second HTTP-only pulse.

## Supported MQTT Packets

The server implements only the packets it needs:

| Packet | Behavior |
|--------|----------|
| `CONNECT` | Maps the connection from the TLS certificate and returns a success `CONNACK`. |
| `PINGREQ` | Returns `PINGRESP`. |
| `SUBSCRIBE` | Stores the requested topic filters and returns `SUBACK` with QoS 0. |
| `PUBLISH` | Routes known box topics, logs unknown topics and sends `PUBACK` for QoS 1 publishes. |
| `DISCONNECT` | Frees TLS/socket state and clears stored subscriptions. |

Outbound publishes are sent as QoS 0 packets. The server does not retain
messages and does not fan out arbitrary topics like a broker.

## Payload Logging

Normal publish logging keeps the existing short inline payload preview. When
`mqtt_server.log_full_payloads` is enabled and a publish payload is at least
`MQTT_LOG_INLINE_PAYLOAD_SIZE` bytes, the server additionally emits a parseable
diagnostic line:

```text
MQTT FULL PUBLISH dir=rx|tx topic='...' payload_b64='...' (QoS ..., declared_len ..., observed_len ...)
```

The payload bytes are base64 encoded so reverse-engineering exports can recover
the complete RX/TX body. In that mode the old large-payload preview intentionally
uses `payload=<full-capture>` instead of a truncated payload fragment, so
analysis tools do not mistake the preview for the real payload.

When `mqtt_server.log_connect_details` is enabled and the global log level is
`5`, the server logs the CONNECT protocol, version, flags, keepalive, field
presence, byte offsets and exact field lengths. The client ID is shown as
plaintext while this expert switch is enabled. Will data, username and password
remain masked.

The TLS listener requests a client certificate with optional authentication.
At log level `5`, `MQTT TLS CertificateRequest` confirms that the request is
enabled. With CONNECT diagnostics enabled, `MQTT TLS client_certificate` shows
whether the box answered and, when present, its subject, issuer and serial.
`verification=not_enforced` is explicit because the listener currently observes
the certificate without requiring a trusted client-CA chain.

## Box Topics

All currently handled box topics use the `toniebox/<box_cn>/...` namespace.

### Incoming Topics

| Topic | Handler | Behavior |
|-------|---------|----------|
| `toniebox/<box_cn>/logs` | `handle_mqtt_publish_logs()` | Logs the payload at debug level. |
| `toniebox/<box_cn>/claim/<ruid>` | `handle_mqtt_publish_claim()` | Validates the topic rUID, records it as Last Played contact, parses JSON `bd`, stores it as opaque diagnostics and marks all-zero `bd` values without triggering content or freshness behavior. |
| `toniebox/<box_cn>/settings/request` | `handle_mqtt_publish_settings_request()` | For a mapped box connection, publishes `settings/desired` and then tries `fresh-tonies`. |
| `toniebox/<box_cn>/settings/confirm` | `handle_mqtt_publish_settings_confirm()` | Parses `toniebox_history` as an acknowledgement for previously sent `settings_history` revisions. In proxy mode only matching local revisions are consumed; any remainder is forwarded. |
| `toniebox/<box_cn>/app-reply/bedtime-state` | `handle_mqtt_publish_app_reply_bedtime_state()` | Parses the observed STL/bedtime reply and stores the latest state. In proxy mode it is consumed only for a pending local STL command within the correlation window. |
| `toniebox/<box_cn>/metrics/battery` | `handle_mqtt_publish_metrics_battery()` | Stores battery percent/raw/current/status when present and emits the matching box events. |
| `toniebox/<box_cn>/metrics/headphones` | `handle_mqtt_publish_metrics_headphones()` | Stores speaker output plus connected-headphone diagnostics and emits the matching box events. |
| `toniebox/<box_cn>/playback/state` | `handle_mqtt_publish_playback_state()` | Parses the observed TB2 playback state, stores the latest semantic playback fields and updates playback box events. |
| `toniebox/<box_cn>/volume/state` | `handle_mqtt_publish_volume_state()` | Validates and stores the observed integer volume level in the confirmed range from 1 through 12. |
| `toniebox/<box_cn>/setup/status` | `handle_mqtt_publish_setup_status()` | Validates JSON and stores a bounded diagnostic snapshot without assigning unconfirmed setup semantics. |
| `toniebox/<box_cn>/metrics/events` | `handle_mqtt_publish_metrics_events()` | Validates JSON and stores a bounded diagnostic snapshot. |
| `toniebox/<box_cn>/metrics/fleet` | `handle_mqtt_publish_metrics_fleet()` | Validates JSON and stores a bounded diagnostic snapshot. |
| `toniebox/<box_cn>/app-reply/pong` | `handle_mqtt_publish_app_reply_pong()` | Stores the request ID and, when it exactly matches the last server ping, the measured round-trip time. Only an exact local match is consumed in proxy mode. |
| `toniebox/<box_cn>/app-reply/alarm` | `handle_mqtt_publish_app_reply_alarm()` | Validates JSON and stores a bounded diagnostic snapshot without assigning unconfirmed alarm semantics. |
| Any other `PUBLISH` | `handle_mqtt_publish_generic()` | Logs the topic and a truncated payload preview. |

### Outgoing `settings/desired`

Topic:

```text
toniebox/<box_cn>/settings/desired
```

This JSON payload is built from the overlay's `toniebox2.*` settings. It is
sent after a box publishes `settings/request`, after a relevant subscription is
seen while settings are pending, or when `mqtt_server_mark_toniebox2_setting_changed()`
finds an active subscribed box connection.

Local changes and retries are suppressed while TONIES owns permanent settings.
Turning off upstream or blocking Desired permits local management independently
of the app-control exception. A blocked Request can receive the snapshot-based
replacement described below, without creating local Cloud revisions.

The `<box_cn>` segment is built per active connection from its observed MQTT
topic ID. It is not taken directly from the persisted overlay `commonName`.

`settings_history` is generated from internal per-overlay revision state. When a
supported `toniebox2.*` option changes, only that corresponding JSON field gets
a new monotone revision and is marked pending. The revision is based on Unix
time in milliseconds and is advanced locally if multiple changes happen in the
same second, and exceeds all known Cloud revisions. Without a Cloud snapshot,
unsupported/static fallback fields retain revision `0`. With a snapshot their
actual Cloud values and revisions are preserved, not replaced by those defaults.

### TONIES-owned settings and durable snapshots

Ownership uses the Step-1 forwarding evaluator for `settings/desired` plus the
global ICI-upstream switch. Box overrides and manual-filter bypass apply exactly
as on the relay. The local-control exception never unlocks these device fields.

| Effective policy | Response to a box settings request |
|---|---|
| Upstream off or Desired blocked | TC answers using local values over its available Cloud baseline. |
| Desired and Request forwarded | TONIES answers; no competing local Desired. |
| Desired forwarded, Request blocked | TC answers once from its stored Cloud snapshot, or the existing local builder if none is available. |
| Confirm blocked | TC still processes the confirmation locally; no forwarding exception. |

Only the existing ten device settings are projected: volume/headphone limits,
bedtime limits, ring brightness, scrubbing, skipping, skipping direction and age
mode. Cache and library options are unrelated. An incoming topic must match the
connection's bound box; it cannot choose another overlay.

`mqtt_settings` records a Desired only after the relay has written its final
permitted payload successfully. It stores one bounded snapshot at
`<configdirfull>/mqtt-settings/<stable-overlay-id>.json`, containing
`schemaVersion: 1`, `overlay`, and `desired` with `settings_history`. Input and
serialized snapshot limits are each 64 KiB. Unknown version-paired settings,
arrays and null values are retained. Missing fields remain unchanged; older
revisions, equal revisions with conflicting values, invalid values and unsafe
JSON integers are not imported. A repeated identical pair is idempotent.

Snapshot updates use a same-directory temporary file, checked writes and flush,
then rename without a copy/truncate fallback. A dedicated mutex serializes
snapshot work; lock order when both are needed is SETTINGS then MQTT_SETTINGS.
After publication, typed setters project supported fields as explicit box
overrides, followed by one overlay-config save. Projection invokes no local
Pending/Publish hook. Normal overlay reloads restore it only while Cloud
management is active; disabling Cloud management retains the values and makes
them editable. The ordinary config writer is not a new atomic storage system:
this does not promise whole-configuration power-loss safety. Import/save errors
are visible in logs and do not undo a successful Cloud forwarding operation.

Cloud revisions never become local Pending revisions. Accepted Cloud fields
supersede stale local wishes. Later local edits replace only their corresponding
fields on the stored baseline, with new JSON-safe revisions. Exhaustion is
reported rather than wrapping. A per-connection ten-field sent ledger is populated
only after actual local delivery. Confirms match that ledger, not merely an
overlay's unsent Pending array. An old local confirm cannot clear a newer wish;
known Cloud collisions remain transparent. After an ownership change, previous
local entries expire within 30 seconds; disconnect discards them. Unchanged
snapshot replies do not create local Pending entries. A TLS write is receipt,
not proof that the box applied the setting.

The settings API exposes `readOnly`, `readOnlyReason: "tonies_settings"` and
`cloudSettingsState: "local" | "waiting" | "received" | "confirmed"` for those
ten fields. Confirmation is runtime evidence, not restored as confirmed after
a restart. Box Set and Reset are rejected with HTTP 409 while Cloud-managed;
global defaults remain editable. Unknown explicit overlays return 404 instead
of mutating globals. Config-write failures return an error, not success.

Capture retains the original received packet and Step-1 decisions. Local
request responses use `cloud_settings_fallback` or `local_settings_fallback`;
locally handled requests use `local_settings_request` and bounded QoS replay
to avoid answering a retransmission again. Ordinary local changes keep
`local_settings_desired`. Receipt, rejection and confirmation logs contain field
names and overlay identity, not new full-payload dumps.

Current desired fields:

| JSON field | Source setting |
|------------|----------------|
| `max_volume` | `toniebox2.max_volume` |
| `bedtime_max_volume` | `toniebox2.bedtime_max_volume` |
| `max_headphone_volume` | `toniebox2.max_headphone_volume` |
| `bedtime_max_headphone_volume` | `toniebox2.bedtime_max_headphone_volume` |
| `lightring_brightness` | `toniebox2.lightring_brightness` |
| `bedtime_lightring_brightness` | `toniebox2.bedtime_lightring_brightness` |
| `scrubbing_enabled` | `toniebox2.scrubbing_enabled` |
| `skipping_enabled` | `toniebox2.slap_enabled` |
| `skipping_direction` | `left` when `toniebox2.slap_back_left` is true, otherwise `right` |
| `age_mode` | `1+` when `toniebox2.baby_mode` is true, otherwise `3+` |

The payload also includes static/default fields such as `settings_applied`,
`battery_threshold`, `timezone_transitions`, `log_level`,
`timezone`, `alarms`, `bedtime_schedules` and `fleet_obs_enabled`. Their
semantics should be verified against real ICI traffic before changing them.

When a confirm payload arrives, `toniebox_history.<field>` is compared with the
last sent `settings_history.<field>` for every pending field. Equal revisions
clear the pending bit for that field. Missing or different revisions keep the
field pending and are logged with field name, expected revision and received
revision. Once all pending fields are acknowledged, the global pending flag and
retry counters are cleared. A partial acknowledgement starts a fresh bounded
retry window for only the fields that remain pending.

On a proxy connection, an equal local revision is also removed from the JSON
before forwarding. If cloud-owned, unknown or non-matching revisions remain,
the PUBLISH is rebuilt with its original topic, flags, QoS and packet ID and
only that remainder is sent to TONIES. If no forwardable data remains, the
PUBLISH is consumed and locally completed with `PUBACK` or the corresponding
QoS-2 `PUBREC`/`PUBCOMP` flow. Invalid or stale confirms pass through unchanged.

Confirm payloads that contain only `settings_applied` or top-level setting
values are treated as diagnostics for compatibility. They do not overwrite local
`toniebox2.*` configuration and they do not clear pending state without matching
`toniebox_history` revisions.

### Outgoing `fresh-tonies`

Topic:

```text
toniebox/<box_cn>/fresh-tonies
```

Payload:

```json
{"tonie":"0123456789ABCDEF"}
```

One QoS-1 `PUBLISH` is sent per UID from `internal.freshnessCache`. UIDs are
deduplicated in their existing cache order and converted to rUID strings via
byte-swap formatting. There is no extra item limit. Delivery only starts when
the active box connection has a matching subscription for the topic and the
connection was mapped from the box certificate to the same overlay/common name.
The concrete topic uses the connection's observed MQTT topic ID. While a cache
entry is pending but no matching topic identity or subscription is known, one
debug message is emitted for that pending series instead of silently waiting.

Content JSON changes target the overlay selected by the API request. A source
change is queued even if that overlay has not yet supplied a V3 freshness
inventory containing the rUID. Follow-up metadata requests such as the WebUI's
automatic `nocloud=true` or `live` update do not clear the source-change marker.
Successful content-meta responses, full or ranged chapter requests and MQTT
delivery acknowledgements do not clear that marker. It remains pending and
keeps `resumeBehavior=alwaysReset` until the box reports the new generation in
`playback/state`. Calls without a selected overlay retain the conservative
inventory-based scan and do not broadcast an unknown rUID to every box.

Freshness invalidations are coalesced per overlay. The first pending
invalidation opens a two-second debounce window; additional invalidations during
that window are appended without opening another debounce window. Only one UID
is in flight: the next UID is sent after the previous `PUBACK`. A targeted
content-mapping invalidation can requeue an already acknowledged UID while
identical queued or in-flight invalidations remain coalesced. Active playback
does not delay freshness delivery.

The initial publish uses a newly reserved non-zero packet ID and `DUP=0`. If no
matching `PUBACK` arrives, the same packet ID and payload are retried after five
and ten seconds with `DUP=1`. Five seconds after the third attempt, the
connection is closed. Remaining cache entries are retried after reconnect. A
foreign valid `PUBACK` is only logged by the local server and remains transparent
in proxy mode.

The transparent proxy reserves local freshness IDs alongside outstanding
cloud-to-box QoS IDs. If a later upstream QoS-1 or QoS-2 publish collides, only
the upstream packet ID is remapped on the box-facing wire; its acknowledgement
flow is translated back. A `PUBACK` for local freshness is consumed by
TeddyCloud and is not forwarded upstream. Local freshness publishes bypass the
bidirectional forwarding filters because they are generated by TeddyCloud, not
relayed cloud traffic.

`PUBACK` proves MQTT delivery only. A pending source change also survives
content-meta delivery and every complete or partial chapter response. It is
completed only by a valid `playback/state` whose canonical rUID matches the
pending rUID and whose `contentVersion` exactly equals the expected effective
version. Old versions, other rUIDs and missing or unparseable fields leave the
marker, `internal.freshnessCacheChanged` and its cache entry untouched. A
reconnect therefore requeues every UID still present in the cache. Hard socket
or TLS write errors close the connection while preserving that pending state.

### Outgoing `app-control/*`

The internal server supports selected TB2 app-control commands through
`src/mqtt_server.c`, with the bounded reply matcher in `src/mqtt_app_control.c`.
This is not part of the external MQTT client in `src/mqtt.c`.

Observed box subscriptions:

```text
toniebox/<box_cn>/app-control/playback
toniebox/<box_cn>/app-control/volume
toniebox/<box_cn>/app-control/ping
toniebox/<box_cn>/app-control/stl
toniebox/<box_cn>/app-control/sleep
toniebox/<box_cn>/app-control/alarm-preview
```

Currently implemented direct publish APIs:

| Function | Topic | Payload handling |
|----------|-------|------------------|
| `mqtt_server_publish_playback_for_overlay()` | `toniebox/<box_cn>/app-control/playback` | Builds one of the confirmed `start`, `pause`, `next`, `prev` or `restart` action payloads. |
| `mqtt_server_publish_playback_position_for_overlay()` | `toniebox/<box_cn>/app-control/playback` | Builds a validated `setPosition` payload with a zero-based chapter and millisecond offset. |
| `mqtt_server_publish_volume_for_overlay()` | `toniebox/<box_cn>/app-control/volume` | Builds a level payload in the confirmed integer range from 1 through 12 without conversion or clamping. |
| `mqtt_server_publish_ping_for_overlay()` | `toniebox/<box_cn>/app-control/ping` | Generates a bounded server request ID and builds the ping payload. |
| `mqtt_server_publish_app_control_stl_for_overlay()` | `toniebox/<box_cn>/app-control/stl` | Sends the caller-provided JSON payload after syntax validation and records a local correlation marker. |
| `mqtt_server_publish_app_control_sleep_for_overlay()` | `toniebox/<box_cn>/app-control/sleep` | Sends the empty JSON object required to put a box with active bedtime mode to sleep. |

App-control publishes are box-only. The server sends them only to active
connections that were mapped from a TLS client certificate to the requested
overlay/common name and whose subscriptions match the target topic using normal
MQTT wildcard semantics. Topic-name mapping alone is not enough for these
commands.

Permissions are calculated per command from the existing Internet filter,
using the actual TONIES-to-box route rather than the reply or status topic:

| Connection and forwarding decision | Local command |
|------------------------------------|---------------|
| Direct TeddyCloud MQTT connection, without an active ICI proxy | Allowed. |
| Active ICI proxy; this command is blocked from TONIES | Allowed without the local-control exception. |
| Active ICI proxy; this command is allowed from TONIES | Requires effective `mqtt_client_upstream.local_control_enabled=true`. |

The manual filter master being off bypasses individual rules: cloud commands
are then allowed and local commands require the exception. Effective global
values and box overrides are read for every decision, so changes apply without
reconnecting. An active, correctly mapped connection and matching subscription
remain mandatory. Permanent Settings ownership is not changed by this policy.

Every permitted local publish goes directly to the box through the packet-aware
writer, never to TONIES, and QoS-0 controls remain captured as `local_app_control`.
The policy does not suppress permitted TONIES commands when the local exception
is enabled. Disabling a permission prevents new local sends but does not discard
already sent ping/STL correlations before their 30-second deadline. Connection
closure does discard them. Playback and volume reports still undergo ordinary
local status processing followed by NoCloud and manual forwarding filters; they
are not consumed as command acknowledgements.

The confirmed playback payloads are generated only by the server. Resume/play
uses `{"action":"start"}`; `{"action":"play"}` is never sent. Chapter numbers
remain zero-based in the protocol. No generic HTTP-to-MQTT command relay is
provided.

`app-control/stl` is deliberately defensive because the full payload schema is
not confirmed yet. The server validates that the payload is JSON but does not
build or hard-code STL command bodies. Callers must pass the complete JSON
string explicitly.

### Runtime and control HTTP API

`GET /api/getBoxes` keeps the existing box fields and adds an optional
`runtime` object. It contains online/last-connection state, exact app-control
subscription capabilities, semantic playback/volume/battery/headphone/bedtime
state, pong correlation data and bounded setup/event/fleet/alarm diagnostic
snapshots. Every semantic state carries `valid` and `updatedAt`; invalid MQTT
payloads do not overwrite the previous valid state.

`runtime.controls` remains the authoritative set of booleans for playback,
volume, ping, bedtime and sleep. Optional `runtime.controlReasons` explains a
denied control with `cloud_controlled`, `offline` or `not_subscribed`; allowed
controls do not need a reason. The HTTP command preflight and final MQTT sender
use the same policy, so WebUI availability is not an independent permission
calculation. Older clients can ignore the additive reasons. The WebUI retains
its previous generic explanation when a backend omits a reason or returns an
unknown one.

The validated command endpoints are:

| Endpoint | Accepted body |
|----------|---------------|
| `POST /api/box/playback?overlay=<id>` | A confirmed action, or `setPosition` with `chapter` and `ms`. |
| `POST /api/box/volume?overlay=<id>` | `{"level":1..12}` with an integer level. |
| `POST /api/box/ping?overlay=<id>` | No caller-provided MQTT payload; the response includes the generated request ID. |
| `POST /api/box/bedtime?overlay=<id>` | Starts bedtime with `{"state":"on","duration":300..86400}` or stops it with `{"state":"off"}`. |
| `POST /api/box/sleep?overlay=<id>` | Accepts `{}` and sends the separate sleep command while bedtime mode is active. |
| `POST /api/box/shutdown?overlay=<id>` | Accepts `{}`. If bedtime is inactive, publishes `{"state":"on","duration":300}` first and immediately follows it with the separate sleep command; with active bedtime it sends only sleep. |

`duration` is an integer number of seconds. A start request rejects values below
300 seconds or above 86400 seconds. A stop request does not require a duration;
an optional supplied duration must still be inside the same range, so zero never
acts as a stop shortcut.

The bedtime start payload can optionally include one one-time alarm:

```json
{
  "state": "on",
  "duration": 1800,
  "oneTimeAlarm": true,
  "alarm": {
    "tone": "<tone-id>",
    "volume": 50,
    "morningLight": true
  }
}
```

The HTTP wrapper validates the field types but deliberately does not invent a
protocol range for alarm volume or a list of tone IDs. The WebUI uses 0 through
100 as a conservative input range. `sleep` is not equivalent to
`{"state":"off"}`: it asks an already active bedtime session to fade and put the
box into its sleep state.

Invalid input returns `400`, an unknown overlay returns `404`, and a non-TB2,
offline, not-subscribed or cloud-controlled box command returns `409`. The error
message distinguishes these denials from a failed local publish without changing
the endpoint or command payload format. A shutdown checks sleep and, only when
bedtime first needs enabling, STL permission before its first publish; it does
not bypass either command's policy. If only the initial STL publish succeeds,
the error reports that partial send rather than claiming shutdown succeeded.
Successful sends do not by themselves prove that the box applied the command.

TB2 volume control uses twelve discrete levels and has no mute level. The
server preserves the reported or requested level unchanged across the HTTP API,
MQTT command payload, runtime state, `/api/getBoxes` and external TeddyCloud
MQTT state. The observed hardware percentages are informational only:

| Level | Hardware percentage |
|------:|--------------------:|
| 1 | 5 |
| 2 | 8 |
| 3 | 14 |
| 4 | 22 |
| 5 | 30 |
| 6 | 40 |
| 7 | 50 |
| 8 | 60 |
| 9 | 70 |
| 10 | 80 |
| 11 | 90 |
| 12 | 100 |

These percentages are not control values and are independent of percentage
settings such as `toniebox2.max_volume`. Absolute API values outside 1 through
12 are rejected rather than clamped. Invalid `volume/state` values are logged
and do not overwrite the last valid runtime state. The direct local MQTT path
and the packet-aware MQTT upstream use the same validation and state handler;
transparent packets and raw capture bytes are not rewritten.

The last usable TB2 volume is stored per canonical box ID below
`data/runtime/toniebox-state/`. A reported `volume/state` and a successfully
published local absolute command both update this small atomic state file. On
restart TeddyCloud restores the stored level. If no level has ever been known,
the runtime API exposes level 2 with `valid=false` and `source="fallback"`; this
fallback is never persisted. `runtime.volume.source` distinguishes `reported`,
`command`, `persisted` and `fallback`. A later reported state always replaces
an optimistic command or restored value.

The WebUI polls the bundled box response every two seconds while the browser
tab is visible and refreshes immediately after a command. It resolves Tonie
metadata only when the current playback rUID changes. The existing
`internal.last_ruid`/`internal.last_ruid_time` Last Played state remains visible
after stop, `tonie:null` and offline transitions, while Now Playing becomes
inactive. Playback and chapter selection are disabled when the box is offline,
the exact capability is absent, or this command is cloud-controlled without a
local exception. The backend-provided reason explains the disabled control.
Volume uses the same availability gates, but remains usable with the
unconfirmed level 2 fallback so an absolute command can resynchronize the box.
The ten permanent device settings are independently locked by TONIES ownership;
cache/library settings remain editable subject to their own dependencies.
Bedtime and sleep controls use their respective backend capabilities; shutdown
also checks the bedtime capability when that first step is required.

Observed reply channel:

```text
toniebox/<box_cn>/app-reply/bedtime-state
```

The reply handler parses these JSON fields when present:

| JSON path | Stored/logged as |
|-----------|------------------|
| `stl.state` | `toniebox_state_t.bedtime.state` |
| `stl.duration` | `toniebox_state_t.bedtime.duration` |
| `stl.defaultDuration` | `toniebox_state_t.bedtime.defaultDuration` |
| `stl.until` | `toniebox_state_t.bedtime.until` |

`app-reply/bedtime-state` is accepted as current box state even when it was not
preceded by a local `app-control/stl` command. Parsed values are emitted through
the existing box-event path as `BedtimeState`, `BedtimeDuration`,
`BedtimeDefaultDuration` and `BedtimeUntil`.

Reply correlation keeps one pending ping and one pending STL operation per
MQTT connection. Entries are created only after a successful local write,
expire after 30 seconds of monotonic time and are discarded on connection close.
A new ping replaces the previous ping; a matching pong consumes its entry once.
A delivered TONIES ping using the same `requestId` makes that correlation
ambiguous, so its reply is not locally consumed.

STL has no confirmed request ID. Only a simple local command containing
`state` and optionally `duration` is eligible for the conservative matcher.
The reply must contain only an `stl` object, with no duplicate or unknown fields;
its allowed fields are `state`, `duration`, `defaultDuration` and `until`.
`on` and `active` describe the same enabled state. An enabled reply must match
the requested integer duration exactly; an off reply must match an off command.
Extended commands, for example those containing an alarm, do not gain an
invented confirmation rule.

Overlapping local STL operations or successfully delivered TONIES STL/sleep
commands make STL attribution ambiguous within the same window. This also
applies when a local STL follows a recent cloud operation. Blocked or failed
cloud sends do not create such conflicts. A local sleep does not erase the
preceding local STL correlation, because the existing shutdown sequence sends
both. No new protocol field is injected. Even this constrained match remains a
heuristic: an indistinguishable unsolicited state announcement cannot be proven
to originate from the command.

All replies first update the existing local status representation. Exact pong
and constrained STL matches are then consumed before upstream filters;
ambiguous, late, extended and unknown replies follow ordinary NoCloud/manual
filtering, rather than being unconditionally forwarded. Playback/volume state
and alarm replies are not consumed by this correlation. Diagnostic reasons
distinguish `exact_pong`, `stl_heuristic` and `ambiguous`, while existing
`local_control.app_reply` capture filter IDs remain compatible. Original captured
packets and bounded QoS replay handling are retained.

A dedicated mutex protects the app-control send/correlation/close lifecycle.
It does not make all TLS operations or the general MQTT relay thread-safe and
does not add a queue, retry service or offline-autonomous MQTT session.

### Incoming `playback/state`

Topic:

```text
toniebox/<box_cn>/playback/state
```

Observed active playback payload:

```json
{"tonie":"tonie_010","contentVersion":1779885493,"chapter":0,"chapterUntilMs":1782298563771,"chapterDuration":124.893}
```

Observed stopped/cleared playback payload:

```json
{"tonie":null}
```

The handler is box-only: the topic common name must match the certificate-mapped
connection. A usable JSON payload updates `toniebox_state_t.playback_state` with
these fields when present:

| JSON path | Stored/logged as |
|-----------|------------------|
| `tonie` | `toniebox_state_t.playback_state.tonie` |
| `contentVersion` | `toniebox_state_t.playback_state.contentVersion` |
| `chapter` | `toniebox_state_t.playback_state.chapter` |
| `chapterUntilMs` | `toniebox_state_t.playback_state.chapterUntilMs` |
| `chapterDuration` | `toniebox_state_t.playback_state.chapterDuration` |

When `tonie` is a string, the runtime box state is marked as playing and the
existing `Playback=ON` box event is emitted only on the transition from stopped
to playing. When `tonie` is `null`, playback state is cleared and
`Playback=OFF` is emitted if the box was previously marked as playing.

The observed `playback/state` payload does not expose a pause field. After a
successful server-originated `pause` or `start` publish, TeddyCloud therefore
updates the transient playback status to `paused` or `playing` respectively.
This keeps the WebUI play/pause control actionable while leaving Last Played
and the current Tonie untouched.

When `tonie` is a valid 16-character hexadecimal rUID, the handler also updates
the existing WebUI anchors `internal.last_ruid` and `internal.last_ruid_time`.
Stopped playback, offline transitions, `tonie:null` and tag-remove events do
not clear or overwrite Last Played.

The same canonical rUID is used to correlate a pending source change. The
change is acknowledged only when `contentVersion` is present, parseable and
exactly equals the effective version expected for that rUID and overlay. This
successful match removes the source-change marker and its freshness entry.
Playback reports for a different rUID, an older or otherwise different version,
or without usable correlation fields update no freshness completion state.

TB2 payloads may report a playback identifier that is not a raw rUID. In that
case the playback state is still stored and emitted, but Last Played is left
unchanged until a topic with an actual rUID, such as `claim/<ruid>`, arrives.

The semantic fields are also published through the existing box-event path as
`PlaybackTonie`, `PlaybackContentVersion`, `PlaybackChapter`,
`PlaybackChapterUntilMs` and `PlaybackChapterDuration`. This reuses the current
Home Assistant/event integration in `src/mqtt.c`; no external MQTT transport
logic is changed for the internal ICI server.

### Incoming `claim/<ruid>`

Topic:

```text
toniebox/<box_cn>/claim/<ruid>
```

The claim handler is box-only and requires the topic common name to match the
certificate-mapped connection. The `<ruid>` topic segment must be exactly 16
hexadecimal characters. The payload must be JSON with a string `bd` shorter
than the internal diagnostic buffer.

The topic rUID is treated as a Tonie contact and updates the existing
`internal.last_ruid` and `internal.last_ruid_time` WebUI anchors. If the rUID is
already the current Last Played value, only the timestamp is refreshed. `bd` is
stored on `toniebox_state_t.claim` only as opaque reverse-engineering state. An
all-zero `bd` is marked explicitly in state/logs, but neither normal nor
all-zero `bd` values trigger automatic claim, content or freshness actions.

### Incoming Metrics

Battery metrics:

```text
toniebox/<box_cn>/metrics/battery
```

Headphone metrics:

```text
toniebox/<box_cn>/metrics/headphones
```

Both handlers are box-only and store only fields that are present and parseable.
Battery state accepts `percent`, signed `raw`, signed `current` and scalar
`status`. Headphone state accepts `speaker.output` and the `connected` array,
which is kept as compact JSON diagnostics plus a device count.

The values are emitted through existing box-event/Home-Assistant infrastructure
as `BatteryPercent`, `BatteryRaw`, `BatteryCurrent`, `BatteryStatus`,
`SpeakerOutput`, `HeadphonesConnected`, `HeadphonesConnectedCount` and
`HeadphonesConnectedDevices`.

## Trigger Points

| File | Trigger |
|------|---------|
| `src/handler_api.c` | Changes to supported `toniebox2.*` settings call `mqtt_server_mark_toniebox2_setting_changed()` with the concrete setting name. |
| `src/handler_cloud.c` | Freshness checks update `internal.freshnessCache`, set `internal.freshnessCacheChanged` and call the overlay publisher. |
| `src/handler_cloud.c` | Content mapping changes can proactively mark rUIDs for V3 freshness and call the overlay publisher. |
| `src/handler.c` | TAP streaming callbacks call the overlay publisher when freshness state changes outside an MQTT connection context. |
| `src/mqtt_server.c` | Certificate-mapped and trusted-topic-mapped active connections update `internal.online` and `internal.last_connection`. |
| `src/mqtt_server.c` | Subscribe/request/background handlers publish locally owned settings; a sent-connection ledger correlates local confirms. Freshness delivery remains independent. |
| `src/mqtt_settings.c` | Central ownership, bounded Cloud snapshots, typed projection, strict revisions and confirmation evidence. |
| `src/mqtt_app_control.c` | Small connection-local, monotonic 30-second ping/STL correlation with conservative collision handling. The caller records completed sends and serializes access. |
| `src/mqtt_server.c` | App-control helpers apply per-command Internet ownership and the local exception, publish only to the box, and maintain bounded connection-local reply correlation. Settings ownership and Freshness remain separate. |
| `src/mqtt_server.c` | `claim`, `app-reply/bedtime-state`, battery/headphone metrics and `playback/state` publishes update semantic TB2 runtime state and box events. `claim/<ruid>` also records Last Played from the topic rUID. |

## Source Occurrence Map

| File | Server-relevant occurrence |
|------|----------------------------|
| `include/mqtt_server.h` | Public lifecycle and direct publish APIs for the internal server. |
| `include/toniebox_state_type.h` | Adds bounded TB2 bedtime/STL, playback, claim, battery, headphone, volume, pong and diagnostic snapshot state to the runtime box state. |
| `src/toniebox_state.c` | Stores semantic TB2 runtime updates and emits the existing playback plus detailed TB2 box events. |
| `include/settings.h` | `settings_mqtt_server_t` and internal pending-state fields for freshness/settings delivery, including TB2 desired-setting revisions, `internal.v3ForcedVersionUids`/`internal.v3ForcedVersions`/`internal.v3ForcedVersionBaseAudioIds` and the `internal.v3HashedChapterUids` migration guard. |
| `src/settings.c` | Registers `mqtt_server.*`, including `mqtt_server.log_full_payloads`, `toniebox2.*` and internal pending-state/revision settings. |
| `src/cert.c` | Generates the ICI server certificate and binds it to the `mqtt_server.cert.*` paths. |
| `src/server.c` | Starts, polls and stops the internal MQTT server. |
| `src/mqtt_server.c` | Owns the TCP/TLS listener, packet parsing, subscription tracking, topic handlers and box publishes. |
| `include/mqtt_nocloud_filter.h` | Declares the per-publish noCloud allow/block/rewrite decision and its rewritten payload ownership. |
| `src/mqtt_nocloud_filter.c` | Performs lightweight per-packet content-policy lookups and selective claim, playback, metrics, BI-event, log and freshness filtering. |
| `src/tb2_mqtt_passthrough.c` | Observes PUBLISH packets locally, then applies automatic NoCloud protection before explicit manual Internet-filter decisions; rebuilds the final payload, preserves QoS/packet-ID translation and records capture/status counters. |
| `src/mqtt.c` | Exposes the new TB2 runtime box events through the existing Home Assistant discovery/event path. |
| `include/home_assistant.h` | Raises the entity budget for the additional TB2 runtime event sensors. |
| `src/handler_api.c` | Exposes runtime state through `getBoxes`, implements the validated box-control HTTP endpoints and marks TB2 settings changes as pending for ICI delivery. |
| `teddycloud_web` | Polls `getBoxes`, resolves current Tonie metadata by rUID and renders the compact TB2 status, playback, volume and chapter controls. |
| `src/handler_cloud.c` | Produces freshness invalidations that are delivered over the internal MQTT server. |
| `src/handler.c` | Routes TAP-related freshness callbacks through the overlay MQTT publisher. |
| `docs/TAP_PLAYLIST_BACKEND.md` | TAP-specific notes for `fresh-tonies`; not a general MQTT server reference. |

## Practical Notes

- The `mqtt.*` settings belong to the external MQTT client/broker path and are
  unrelated to the ICI listener.
- The code has no remote ICI server setting. To make a box reach TeddyCloud,
  route the ICI hostname to the TeddyCloud host and enable the internal listener
  on the expected port.
- The ICI certificate paths are part of the server settings, not the external
  MQTT client TLS settings.
- The server currently learns the box overlay from certificate identity and/or
  the `toniebox/<box_cn>/...` topic namespace.
- Transparent proxy packets and their capture data retain their original topic
  bytes. Canonicalization applies only to logical identities and locally
  generated MQTT messages.
- Old offline package patches and test artifacts contain MQTT text, but they are
  not authoritative for the current implementation.
