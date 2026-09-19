# Settings scopes in the WebUI

The WebUI classifies every public TeddyCloud setting into one of three scopes:

- **Global** contains settings that are independent of a Toniebox generation.
- **TB1** contains TB1 certificates, cloud endpoints, RTNL and TB1 box behaviour.
- **TB2** contains TB2 certificates, HTTPS modes, ICI MQTT and TB2 box behaviour.

The global settings page shows all three tabs. A box overlay shows `Global` and
the tab matching `toniebox.boxGeneration`; an overlay with an unknown generation
shows only `Global`. Overlay eligibility remains deliberately narrower than the
global settings list, so the scope filter cannot make a non-overlayable setting
editable.

The central declaration is
`teddycloud_web/src/components/common/form/settingsLayout.json`. It defines the
scope, section, ordering, overlay allowlist and UI dependencies. Repository
settings registered in `src/settings.c` must match exactly one declared section.
The settings-scope contract test rejects missing or overlapping assignments.
Settings supplied only at runtime fall back to `Global` and emit a browser
console warning.

`mqtt.*` and `hass.*` are general integrations and remain in `Global`.
`mqtt_server.*` and `mqtt_client_upstream.*` are part of the TB2 ICI path and
therefore appear only under `TB2`. The specialized bidirectional MQTT filter
editor is rendered once in that section rather than duplicating its raw boolean
settings.

Request-specific TB2 v3 endpoint switches are hidden while the effective
`cloud.tb2_v3_enabled` master is false, including in box overlays, but their
stored values are not changed. OTA cache and local-delivery controls remain in
the TB2 firmware-update section because local delivery is intentionally
independent of cloud access. TB1 has its own firmware-update section and policy;
the two generations do not share OTA cache or delivery settings.
