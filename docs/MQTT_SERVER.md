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
| `mqtt_server.port` | `8883` | TCP port used by the internal ICI/MQTT listener. |
| `mqtt_server.cert.crt` | `certs/server_tb2/ici.pem` | PEM server certificate loaded for the TLS endpoint. |
| `mqtt_server.cert.key` | `certs/server_tb2/ici.key` | PEM server private key loaded for the TLS endpoint. |
| `mqtt_server.log_full_payloads` | `false` | Expert diagnostic switch for base64 full-payload capture of large MQTT publishes. |
| `mqtt_server.log_connect_details` | `false` | Logs the MQTT CONNECT structure and plain client ID at global log level 5; credentials and Will fields remain masked. |

Upgrades migrate only the exact former default paths `certs/server/ici.pem`
and `certs/server/ici.key` to their `certs/server_tb2/` counterparts. Custom
certificate paths are never rewritten. If both legacy files exist and neither
target file exists, TeddyCloud copies and verifies the pair before removing the
legacy files. This is a one-time migration, not a runtime TB1 fallback.

The ICI listener is fail-closed. TeddyCloud does not bind its MQTT port when
either resolved PEM path is missing, unreadable or empty. A TLS context/setup
failure on an accepted socket closes that socket before MQTT parsing; encrypted
TLS bytes are therefore never treated as plaintext MQTT packets.

The separate **MQTT Client Upstream** category controls the optional TB2 ICI
packet proxy:

| Setting | Default | Purpose |
|---------|---------|---------|
| `mqtt_client_upstream.enabled` | `false` | Enables the TB2 MQTT cloud path. |
| `mqtt_client_upstream.passthrough_enabled` | `false` | Arms packet-aware forwarding and full local capture; requires `enabled=true`. |
| `mqtt_client_upstream.local_control_enabled` | `false` | Allows local `settings/desired` and `app-control/*` publishes while the proxy is active. The value can be overridden per TB2 overlay. |
| `mqtt_client_upstream.port` | `8883` | Tonies ICI upstream MQTT port. |
| `mqtt_client_upstream.hostname` | `ici.tonie.cloud` | Tonies ICI upstream hostname. |
| `mqtt_client_upstream.capture_dir` | `data/diagnostics/tb2-mqtt-passthrough` | Local session capture directory. |
| `mqtt_client_upstream.capture_max_mib` | `4096` | Maximum total size of completed captures. |
| `mqtt_client_upstream.forward.*` | `true` | Forwards the selected topic class in both directions. `false` suppresses it locally. |

These settings are deliberately separate from both the generic external
`mqtt.*` client and the internal TB2-facing `mqtt_server.*` listener.
The internal `mqtt_server` remains the incoming TLS endpoint. After that TLS
handshake and before MQTT packet parsing, an armed forwarder maps the presented
box certificate to an overlay and opens a second TLS connection using the
original per-box identity from `core.client_cert_tb2.*`. It must never fall back
to the TB1 `core.client_cert_tb1.*` identity. The local `mqtt_server.cert.*` ICI
identity is never reused for the outbound role.

For Docker installations, publish TCP port `8883` unchanged when the internal
ICI endpoint is enabled. Split DNS must resolve the box-facing ICI hostname to
TeddyCloud, while DNS inside the TeddyCloud container must continue to resolve
the public TONIES ICI and HTTPS hostnames. Pointing the container itself back to
TeddyCloud would create a forwarding loop instead of an upstream connection.

For an armed connection, decrypted MQTT application bytes are reassembled into
complete MQTT packets in both directions. Fragmented packets and multiple
packets in one TLS read are supported up to MQTT's own Remaining Length limit;
there is no additional proxy packet-size limit. Packets that do not match a
disabled forwarding option are sent byte-for-byte unchanged. The box's original
CONNECT data, including any ICI credential it carries, is therefore still
forwarded without reconstruction.

Every `mqtt_client_upstream.forward.*` option defaults to `true`. Setting one to
`false` suppresses its matching `PUBLISH` in both directions. The available
classes are `claim`, `volume`, `bi_events`, `fresh_tonies`, individual log
sources plus `logs.other`, and the listed children plus `other` for `metrics`,
`app_reply`, `settings`, `playback`, and `app_control`. Matching is performed on
`toniebox/<id>/...` topic segments. Log `source` values are read exactly and
case-sensitively from valid JSON; missing, invalid, or unlisted sources use
`logs.other`. An `other` option applies only to the group root and children that
have no dedicated option.

All forwarding options are overlay-capable. An explicit box value wins;
otherwise the current global value is read for every packet, so a global change
also affects existing sessions immediately. The WebUI presents global options
as forwarding switches and box options as `Global | Forward | Suppress`.

After local observation and response correlation, the proxy applies an
automatic selective NoCloud policy before the manual forwarding switches. The
automatic decision therefore cannot be enabled or bypassed by a forwarding
setting. No additional setting is required. A valid 16-hex rUID is
accepted case-insensitively and canonicalized to uppercase by the shared TB2
rUID policy. It is protected for the effective box overlay when the shared
policy resolves either an automatic source lock or a manual NoCloud lock without
an effective cloud override. The lookup reads only the policy fields and source
presence for each unique rUID in the current packet; it does not load TAF
headers or playlists and it keeps no persistent cache. A missing content JSON
is treated as an unknown cloud Tonie. Existing unreadable JSON or invalid policy
fields are fail-closed for that rUID, so damaged local policy cannot leak data.
Reserved TB2 system rUIDs beginning with `00000AF0` are classified separately;
they do not inherit a Tonie content JSON NoCloud decision.

Independently of that policy, every box-to-TONIES payload is scanned as bounded
binary data for the local chapter prefix `teddycloud_`. A match suppresses the
whole publish with filter ID `local_content.teddycloud_payload`, even when the
referenced RUID has no NoCloud lock and even when a manual forwarding switch is
enabled. Locally generated server-to-box settings, controls and freshness
publishes are not upstream relay traffic and do not enter this filter.

The automatic policy applies in these directions:

- Box to cloud: a protected `claim/<ruid>` or `playback/state` is suppressed;
  `{"tonie":null}` remains visible. Entries with a protected direct `tonie`
  field are removed from `metrics/fleet`, `metrics/events` and `bi-events`
  arrays. This removes playback metrics `1000` and `1001` as whole objects,
  including chapter, duration, play-time, content-version and end-reason data.
  A log publish is suppressed when its unstructured payload contains an
  explicitly bounded, case-insensitive 16-hex token for a protected rUID. For
  a parseable log array, only entries containing protected RUIDs are removed;
  the safe remainder is forwarded.
- Cloud to box: protected rUIDs are removed from `fresh-tonies` single objects,
  string arrays and object arrays. TeddyCloud's locally generated freshness
  publishes bypass this relay policy.

Unrelated array entries and rUID-less data remain byte-identical unless at least
one item is removed. A non-empty partial result is rebuilt with the original
PUBLISH flags, topic, QoS and packet ID. An empty result is suppressed. Invalid
JSON, allocation failure or rebuild failure on these structured topics is also
suppressed without closing the MQTT session. The original QoS 1/2 publish is
then completed using the same local acknowledgement state as a manual filter.

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
connection, local settings and app controls are permitted only when the
effective global or overlay value of
`mqtt_client_upstream.local_control_enabled` is true. Pending settings remain
queued while it is false and are sent without reconnecting after it becomes
true. Local `fresh-tonies` delivery is independent of this switch.

Matching responses to local proxy commands are handled before the automatic
NoCloud/payload protection and the manual forwarding filters. Matching local
`settings/confirm` revisions are removed from the payload; an entirely local
confirm is consumed, while unknown or cloud-owned fields are rebuilt and sent
to TONIES. A `pong` is local only when its `requestId` exactly matches the last
local ping. A `bedtime-state` reply is local only while a local STL command is
pending and within the 30-second correlation window. Alarm replies and all
unmatched, invalid or stale responses remain transparent. Correlations that
were already pending continue to be consumed after the switch is disabled and
are discarded when the MQTT connection ends. Cloud-to-box publishes are logged
and captured only; TeddyCloud never executes them. For QoS 1 and QoS 2, the
proxy remembers a bounded set of local consume/rewrite decisions. A duplicate
PUBLISH therefore reuses the original decision before correlation runs again
and cannot leak to TONIES after the first packet cleared the pending action.

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
The outbound client identity defaults to the global `core.client_cert_tb2.*`
settings. A box-specific identity is selected only when at least one TB2 client
certificate file or data setting is actively overridden in that box overlay;
`stage=select_identity` reports the selected source. Newly discovered overlays
keep these TB2 settings inherited instead of activating empty per-box paths.

The separate **ICI Upstream** navbar tag polls
`GET /api/mqtt-client-upstream/status` every five seconds. States are
`disabled`, `standby`, `armed`, `connecting`, `tunneling` and `error`; only an
active tunnel is green. The API contains no credential values, certificate
paths, payloads or box identifiers.

Each session writes `session.json` and a full Base64 `traffic.jsonl` capture.
The capture is packet-based and records `packet_type`, optional `topic`,
`forwarded`, optional `filter_id`, `generated`, and `packet_complete`.
`data_base64` always contains the packet actually received from that direction.
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
`local_status_nocloud_rewrite`, so the same capture line shows that TeddyCloud
processed the original payload before applying the final relay decision. The
independent local-prefix guard uses `local_content_block` (or
`local_status_local_content_block` after passive processing) together with its
`local_content.*` filter ID.
Capture data is intentionally sensitive, local-only and excluded from Git.

There is no `mqtt_server.hostname` setting in the code. Hostname selection is
handled outside the listener by DNS/network routing and by the certificate
names generated for the ICI endpoint.

## Certificate Identity

`src/cert.c` generates the TB2 MQTT/ICI certificate from the TB2 server CA:

- Certificate subject/common name: `ici.tonie.cloud`
- SAN DNS names: `ici.tonie.cloud`, `ici.dev.tonie.cloud`,
  `ici.stage.tonie.cloud`
- Certificate setting paths: `mqtt_server.cert.crt` and
  `mqtt_server.cert.key`
- Signing material: `internal.server_tb2.ca` and
  `internal.server_tb2.ca_key`

`src/cert.c` also special-cases generated certificate subjects containing
`ici.tonie.cloud` so they keep the ICI hostname as their common name.

## Runtime Lifecycle

`src/server.c` owns the lifecycle:

- `mqtt_server_init()` is called after the HTTP/HTTPS server contexts start.
- `mqtt_server_task()` runs once per main loop iteration after a 250 ms delay.
- `mqtt_server_deinit()` runs during server shutdown.

`mqtt_server_init()` exits immediately when `mqtt_server.enabled` is false. When
enabled, it resolves the configured certificate paths relative to the
TeddyCloud base directory, loads the PEM certificate/key into memory, opens a
non-blocking TCP socket, binds to `IP_ADDR_ANY:mqtt_server.port` and listens
with a backlog of 5.

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
| `MQTT_SETTINGS_DESIRED_MAX_ATTEMPTS` | `3` | Maximum pending settings publishes before waiting for confirm. |
| `MQTT_SETTINGS_DESIRED_RETRY_INTERVAL_SEC` | `5` | Retry interval for pending settings publishes. |
| `MQTT_APP_CONTROL_REPLY_WINDOW_SEC` | `30` | Time window used to correlate an `app-control/stl` publish with a later bedtime-state reply. |

## Connection Mapping

Each accepted connection starts with the global settings overlay and the global
box state. It is promoted to a box connection when the server can map it to a
known overlay.

Mapping sources:

- TLS client certificate on MQTT `CONNECT`
- The `<box_cn>` segment in topics matching `toniebox/<box_cn>/...`

The certificate mapping accepts client certificate issuers/subjects containing
one of these strings:

- `Boxine Factory SubCA`
- `Toniebox SubCA`
- `TeddyCloud`
- `Toniebox Root CA`

The supported certificate common-name formats are the Tonies style
`b'<MAC>'` form and a raw 12-character box common name. A successful lookup via
`get_settings_cn()` sets:

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
certificate common name resolves to a known overlay. Topic-based mapping may
also promote the connection to `conn->box_connection`, but only when the TLS
client certificate issuer/subject is trusted and the `toniebox/<box_cn>/...`
topic common name resolves to a known overlay. This lets real TB2 ICI sessions
recover when the certificate subject is not one of the older exact common-name
formats, without letting an unauthenticated MQTT client become a box just by
choosing a topic name.

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
| `toniebox/<box_cn>/volume/state` | `handle_mqtt_publish_volume_state()` | Validates and stores the observed volume level in the confirmed range from 0 through 10. |
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

For proxy connections this delivery, including its retry pump, is paused unless
the effective `mqtt_client_upstream.local_control_enabled` value is true. The
pending revisions are retained and become eligible immediately when the switch
is enabled. Direct local MQTT connections do not consult this setting.

The `<box_cn>` segment is built per active connection from its observed MQTT
topic ID. It is not taken directly from the persisted overlay `commonName`.

`settings_history` is generated from internal per-overlay revision state. When a
supported `toniebox2.*` option changes, only that corresponding JSON field gets
a new monotone revision and is marked pending. The revision is based on Unix
time in milliseconds and is advanced locally if multiple changes happen in the
same second. Unsupported/static fields remain at revision `0` until their
semantics are confirmed.

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
Successful content-meta responses, MQTT delivery acknowledgements and playback
reports do not clear that marker. It remains pending and keeps
`resumeBehavior=alwaysReset`; only a successful full or ranged V3 chapter
transfer from the still-current generation completes it.
Calls without a selected overlay retain the conservative
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
content-meta delivery. It is completed only after the V3 HTTP path successfully
transfers a full or ranged chapter whose canonical rUID, active source and
effective version still match the pending generation. Old versions, other
rUIDs, `404`/`416`, interrupted transfers and unassigned chapter names leave the
marker, `internal.freshnessCacheChanged` and its cache entry untouched. A
reconnect therefore requeues every UID still present in the cache. Hard socket
or TLS write errors close the connection while preserving that pending state.

### Outgoing `app-control/*`

The internal server has experimental support for selected TB2 app-control
commands. This is implemented only in `src/mqtt_server.c`; it is not part of
the external MQTT client in `src/mqtt.c`.

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
| `mqtt_server_publish_volume_for_overlay()` | `toniebox/<box_cn>/app-control/volume` | Builds a level payload in the confirmed range from 0 through 10. |
| `mqtt_server_publish_ping_for_overlay()` | `toniebox/<box_cn>/app-control/ping` | Generates a bounded server request ID and builds the ping payload. |
| `mqtt_server_publish_app_control_stl_for_overlay()` | `toniebox/<box_cn>/app-control/stl` | Sends the caller-provided JSON payload after syntax validation and records a local correlation marker. |

App-control publishes are box-only. The server sends them only to active
connections that were mapped from a TLS client certificate to the requested
overlay/common name and whose subscriptions match the target topic using normal
MQTT wildcard semantics. Topic-name mapping alone is not enough for these
commands.

With an active proxy, the effective
`mqtt_client_upstream.local_control_enabled` value must additionally be true.
Every local proxy publish passes through the packet-aware writer so QoS-0
controls are captured as `local_app_control`. Disabling the switch prevents new
commands but does not invalidate an already pending ping or STL correlation;
its matching reply is still consumed until the 30-second correlation window
expires. Expired markers are cleared and unmatched replies continue to TONIES
unchanged.

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

The validated command endpoints are:

| Endpoint | Accepted body |
|----------|---------------|
| `POST /api/box/playback?overlay=<id>` | A confirmed action, or `setPosition` with `chapter` and `ms`. |
| `POST /api/box/volume?overlay=<id>` | `{"level":0..10}`. |
| `POST /api/box/ping?overlay=<id>` | No caller-provided MQTT payload; the response includes the generated request ID. |
| `POST /api/box/bedtime?overlay=<id>` | Returns `501` until the STL command schema is confirmed. |

Invalid input returns `400`, an unknown overlay returns `404`, and a non-TB2,
offline or not-exactly-subscribed box returns `409`.

The WebUI polls the bundled box response every two seconds while the browser
tab is visible and refreshes immediately after a command. It resolves Tonie
metadata only when the current playback rUID changes. The existing
`internal.last_ruid`/`internal.last_ruid_time` Last Played state remains visible
after stop, `tonie:null` and offline transitions, while Now Playing becomes
inactive. Playback, chapter selection and volume controls are disabled when
the box is offline, the exact capability is absent, or a running proxy does not
permit local control. All `toniebox2.*` settings that produce
`settings/desired` are disabled by the same effective setting. Bedtime state is
shown, but its control stays disabled until the STL schema is confirmed.

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

For `app-control/stl`, the server keeps only a local timestamp, sequence number
and payload hash. Replies are logged as matched when they arrive within the
configured correlation window. No sequence or correlation field is injected into
the MQTT payload.

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

Playback observations do not acknowledge or clear pending source changes.
Their canonical rUID and `contentVersion` remain available as runtime status,
but transport observation alone does not prove that the newly selected
generation was retrieved. Final source-change cleanup is owned by the successful
and revalidated V3 chapter transfer.

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
| `src/mqtt_server.c` | Subscribe/request/background handlers publish pending settings and coalesced freshness data to the active connection. Proxy settings delivery obeys the effective local-control setting; `settings/confirm` clears and consumes only matching local revisions. |
| `src/mqtt_server.c` | App-control helpers build typed playback, volume and ping commands; experimental `stl` remains raw JSON until its schema is confirmed. Proxy commands obey the effective local-control setting and replies require exact local correlation. |
| `src/mqtt_server.c` | `claim`, logs, `app-reply/bedtime-state`, battery/headphone metrics and `playback/state` publishes are observed locally before relay filtering; semantic status handlers update TB2 runtime state and box events. `claim/<ruid>` also records Last Played from the topic rUID. |

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
| `src/mqtt_nocloud_filter.c` | Performs lightweight per-packet content-policy lookups, selective claim/playback/metrics/BI-event/log/freshness filtering and the independent `teddycloud_` payload guard. |
| `src/tb2_mqtt_passthrough.c` | Applies automatic protection after local observation and before manual filters, rebuilds partial PUBLISH packets, preserves QoS/packet-ID translation and records capture/status counters. |
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
