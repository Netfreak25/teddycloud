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

The separate **MQTT Client Upstream** category controls the optional transparent
TB2 ICI capture forwarder:

| Setting | Default | Purpose |
|---------|---------|---------|
| `mqtt_client_upstream.enabled` | `false` | Enables the TB2 MQTT cloud path. |
| `mqtt_client_upstream.passthrough_enabled` | `false` | Arms transparent forwarding and full local capture; requires `enabled=true`. |
| `mqtt_client_upstream.port` | `8883` | Tonies ICI upstream MQTT port. |
| `mqtt_client_upstream.hostname` | `ici.tonie.cloud` | Tonies ICI upstream hostname. |
| `mqtt_client_upstream.capture_dir` | `data/diagnostics/tb2-mqtt-passthrough` | Local session capture directory. |
| `mqtt_client_upstream.capture_max_mib` | `4096` | Maximum total size of completed captures. |

These settings are deliberately separate from both the generic external
`mqtt.*` client and the internal TB2-facing `mqtt_server.*` listener.
The internal `mqtt_server` remains the incoming TLS endpoint. After that TLS
handshake and before MQTT packet parsing, an armed forwarder maps the presented
box certificate to an overlay and opens a second TLS connection using the
original per-box identity from `core.client_cert.*`. The local
`mqtt_server.cert.*` identity is never reused for the outbound role.

For an armed connection, decrypted MQTT application bytes are captured and
forwarded unchanged in both directions. No local MQTT parser, handler, ACK,
publish, cache or control path runs for that connection. The box's original
CONNECT data, including any ICI credential it carries, is forwarded without
inspection or reconstruction.

The separate **ICI Upstream** navbar tag polls
`GET /api/mqtt-client-upstream/status` every five seconds. States are
`disabled`, `standby`, `armed`, `connecting`, `tunneling` and `error`; only an
active tunnel is green. The API contains no credential values, certificate
paths, payloads or box identifiers.

Each session writes `session.json` and a full Base64 `traffic.jsonl` capture.
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
| `MQTT_FRESH_TONIES_MAX` | `50` | Maximum rUIDs in one `fresh-tonies` payload. |
| `MQTT_FRESH_TONIES_DEBOUNCE_SEC` | `2` | Per-overlay coalescing window before a pending `fresh-tonies` publish may be sent. |
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

## Box Topics

All currently handled box topics use the `toniebox/<box_cn>/...` namespace.

### Incoming Topics

| Topic | Handler | Behavior |
|-------|---------|----------|
| `toniebox/<box_cn>/logs` | `handle_mqtt_publish_logs()` | Logs the payload at debug level. |
| `toniebox/<box_cn>/claim/<ruid>` | `handle_mqtt_publish_claim()` | Validates the topic rUID, records it as Last Played contact, parses JSON `bd`, stores it as opaque diagnostics and marks all-zero `bd` values without triggering content or freshness behavior. |
| `toniebox/<box_cn>/settings/request` | `handle_mqtt_publish_settings_request()` | For a mapped box connection, publishes `settings/desired` and then tries `fresh-tonies`. |
| `toniebox/<box_cn>/settings/confirm` | `handle_mqtt_publish_settings_confirm()` | Parses `toniebox_history` as an acknowledgement for previously sent `settings_history` revisions. |
| `toniebox/<box_cn>/app-reply/bedtime-state` | `handle_mqtt_publish_app_reply_bedtime_state()` | Parses the observed STL/bedtime reply, stores the latest state on the box state and logs whether it is close to the last local STL command. |
| `toniebox/<box_cn>/metrics/battery` | `handle_mqtt_publish_metrics_battery()` | Stores battery percent/raw/current/status when present and emits the matching box events. |
| `toniebox/<box_cn>/metrics/headphones` | `handle_mqtt_publish_metrics_headphones()` | Stores speaker output plus connected-headphone diagnostics and emits the matching box events. |
| `toniebox/<box_cn>/playback/state` | `handle_mqtt_publish_playback_state()` | Parses the observed TB2 playback state, stores the latest semantic playback fields and updates playback box events. |
| `toniebox/<box_cn>/volume/state` | `handle_mqtt_publish_volume_state()` | Validates and stores the observed volume level in the confirmed range from 0 through 10. |
| `toniebox/<box_cn>/setup/status` | `handle_mqtt_publish_setup_status()` | Validates JSON and stores a bounded diagnostic snapshot without assigning unconfirmed setup semantics. |
| `toniebox/<box_cn>/metrics/events` | `handle_mqtt_publish_metrics_events()` | Validates JSON and stores a bounded diagnostic snapshot. |
| `toniebox/<box_cn>/metrics/fleet` | `handle_mqtt_publish_metrics_fleet()` | Validates JSON and stores a bounded diagnostic snapshot. |
| `toniebox/<box_cn>/app-reply/pong` | `handle_mqtt_publish_app_reply_pong()` | Stores the request ID and, when it matches the last server ping, the measured round-trip time. |
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
retry counters are cleared.

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
["0123456789ABCDEF"]
```

The array is built from `internal.freshnessCache` for the overlay. UIDs are
deduplicated, converted to rUID strings via byte-swap formatting, and capped at
50 entries. The publish only happens when the active box connection has a
matching subscription for the exact topic and the connection was mapped from
the box certificate to the same overlay/common name.

Freshness invalidations are coalesced per overlay. The first pending
invalidation opens a two-second debounce window; additional invalidations during
that window increase the coalesced counter instead of publishing another payload
immediately. If `internal.freshnessCacheChanged` is already set when the server
polls an active box connection, the background flush restores the in-memory
pending state and applies the same debounce rules.

When the active TB2 playback rUID matches an rUID in the freshness cache, the
publish is held pending so the server does not ask the box to invalidate the
Tonie it is currently playing. The diagnostic log includes the rUID reason,
coalesced count, active playback rUID if any and the publish timestamp.

A successful publish clears only the in-memory coalescer. The persistent
`internal.freshnessCacheChanged` state stays set until a later content request
proves that the box revalidated the stale content.

The background flush remembers the last successfully published payload per
overlay. While the persistent freshness state is still waiting for a content
request, the same payload is not re-published every debounce window. A reconnect,
fresh subscription or explicit overlay invalidation can still send the same
payload again, because those events may represent a box that missed the previous
publish.

If a server-to-box publish fails with a hard socket/TLS write error such as
`Not connected` or `Write failed`, the affected MQTT connection is closed and
marked inactive immediately. This prevents repeated `fresh-tonies` fanout to a
dead connection while keeping the pending freshness state intact for a later
box reconnect/subscribe.

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
overlay/common name and that have subscribed the exact target topic. Topic-name
mapping alone is not enough for these commands.

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
the box is offline or the exact capability is absent. Bedtime state is shown,
but its control stays disabled until the STL schema is confirmed.

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
| `src/mqtt_server.c` | Subscribe/request/background handlers publish pending settings and coalesced freshness data directly to the active connection. `settings/confirm` clears pending fields only when `toniebox_history` matches the sent revision. |
| `src/mqtt_server.c` | App-control helpers build typed playback, volume and ping commands; experimental `stl` remains raw JSON until its schema is confirmed. All publishes require a matching subscribed TB2 connection. |
| `src/mqtt_server.c` | `claim`, `app-reply/bedtime-state`, battery/headphone metrics and `playback/state` publishes update semantic TB2 runtime state and box events. `claim/<ruid>` also records Last Played from the topic rUID. |

## Source Occurrence Map

| File | Server-relevant occurrence |
|------|----------------------------|
| `include/mqtt_server.h` | Public lifecycle and direct publish APIs for the internal server. |
| `include/toniebox_state_type.h` | Adds bounded TB2 bedtime/STL, playback, claim, battery, headphone, volume, pong and diagnostic snapshot state to the runtime box state. |
| `src/toniebox_state.c` | Stores semantic TB2 runtime updates and emits the existing playback plus detailed TB2 box events. |
| `include/settings.h` | `settings_mqtt_server_t` and internal pending-state fields for freshness/settings delivery, including TB2 desired-setting revisions. |
| `src/settings.c` | Registers `mqtt_server.*`, including `mqtt_server.log_full_payloads`, `toniebox2.*` and internal pending-state/revision settings. |
| `src/cert.c` | Generates the ICI server certificate and binds it to the `mqtt_server.cert.*` paths. |
| `src/server.c` | Starts, polls and stops the internal MQTT server. |
| `src/mqtt_server.c` | Owns the TCP/TLS listener, packet parsing, subscription tracking, topic handlers and box publishes. |
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
- Old offline package patches and test artifacts contain MQTT text, but they are
  not authoritative for the current implementation.
