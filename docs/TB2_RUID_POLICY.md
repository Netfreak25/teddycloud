# TB2 rUID and NoCloud Policy

TB2 rUIDs are accepted as exactly 16 hexadecimal characters and represented
internally as uppercase. `tb2_ruid_canonicalize()`, `tb2_ruid_to_uid()` and
`tb2_ruid_from_uid()` are the only conversion entry points used by the TB2
HTTPS, MQTT, freshness, state and V3 cache paths.

## Classification

- `TB2_RUID_CONTENT` identifies normal Tonie content.
- `TB2_RUID_SYSTEM` identifies the reserved `00000AF0...` range.
- `TB2_RUID_INVALID` is never used as a cache or policy key.

System rUIDs are not Tonie models and do not inherit NoCloud from a content
JSON. They remain visible to the TB2 freshness path as their own class.

## Effective NoCloud decision

`tb2_nocloud_policy_from_content()` and `tb2_nocloud_policy_resolve()` bind a
decision to the supplied effective settings overlay and canonical rUID. Normal
content is blocked from upstream only when `nocloud=true` and
`cloud_override=false`. The Tonie model is deliberately not part of this
decision.

The lightweight persisted lookup uses the effective overlay's
`internal.contentdirfull`. Missing content JSON means no local policy. Missing
legacy boolean fields retain their historical `false` default; unreadable JSON
or a present field with the wrong type is an unresolved policy and privacy
callers fail closed. Values are normalized on read and new state is written in
canonical form instead of performing a broad migration.

## Cache and upstream identities

A V3 generation descriptor binds `overlay`, uppercase `ruid`,
`effectiveVersion` and its complete chapter list. Chapter names are validated
against the same descriptor identity. This prevents one overlay or content
version from selecting another generation.

TB2 upstream TLS certificates continue to be selected through
`tb2_client_identity_resolve()`: the global `client_tb2` set is the default and
an explicit complete box override wins. V3 content authentication uses
`tb2_content_identity_resolve()`, which canonicalizes both current and persisted
override rUIDs and validates the matching authentication token. These entry
points are reusable by the later V3 download/cache work.

TB1 keeps its existing content and freshness semantics; these policy helpers
are applied only at TB2-specific boundaries.
