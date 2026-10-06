# Optional TB2 firmware slot cache

`cloud.cacheOtaV3BothSlots` is a global-only TB2 switch, disabled by default.
The WebUI displays it directly below the existing TB2 OTA request setting.
Box overrides are rejected by the settings API. Existing shared `cloud.cacheOta`
and `cloud.localOta` settings retain their names, defaults and behavior; no
configuration migration or TB1 settings changes are required.

The additional download requires the requesting box's effective
`cloud.tb2_v3_enabled`, `cloud.enableV3Ota` and `cloud.cacheOta` settings.
`cloud.localOta` controls delivery only. Cache-only operation may store both
images while returning the existing cache-only response to the box.

When enabled, TeddyCloud observes complete HTTP-200 `/v3/check-ota` replies
while forwarding them unchanged. At most 16 KiB is buffered and only the latest
offer per box is retained in RAM. Both `FirmwareSlot0` and `FirmwareSlot1`
must provide an unambiguous URL, hexadecimal `sha2_256` and `version_string`.

Only a subsequent firmware request from the box starts caching. Its resource
path selects one manifest entry; after that image is successfully cached or
verified locally, one worker obtains the other entry if it is missing.
`APTRMapping`, invalid offers and checks without a firmware request do not
schedule additional downloads. Restarting clears the offers until a new check.

Both files are verified by their actual SHA-256. Resource names are opaque and
are never used to derive another URL or substitute for the manifest hash.
The box's query parameters and the counterpart URL's query parameters are
preserved separately. The worker uses the configured TONIES upstream and copies
the resolved TB2 certificate, key and User-Agent. Other hosts and redirects are
not accepted for these downloads.

Files use `ota/tb2/<type>/<resource>.bin` regardless of response filenames.
Temporary files are published only after complete HTTP receipt, flush and
matching SHA-256. Foreground and background requests coordinate writes by
target file, including requests using the existing handler without a known
offer. There is at most one waiting job per box, bounded by `MAX_OVERLAYS`.
Network, filesystem or hash failures are logged; retry occurs on a later box
request, without a worker retry loop. Disabling the switch drops waiting jobs
and offers, while an active transfer may finish. Shutdown joins the worker.

The TB2 module is connected only to the existing check/download entry points.
TB1 OTA handling, MQTT policies and the transparent HTTPS monitor are unchanged.
Caching does not automatically install firmware.

## Verification

From the repository root, using Python 3 and GCC on Linux or WSL:

```sh
python3 tests/test_tb2_ota_cache_runtime.py
python3 tests/test_tb2_ota_settings_runtime.py
python3 tests/test_settings_scope_layout_contract.py
node --test teddycloud_web/tests/otaSettings.test.cjs
```

The cache harness runs the production module with controlled HTTP responses,
real files, SHA-256 and threads. It covers both selections, cache hits, races,
invalid offers, transfer failures, opt-out and shutdown. Settings tests execute
the production registration, setter, reset and save functions with a controlled
option map. Compiler, TypeScript and formatting checks supplement these tests.
A successful real OTA capture with both slot URLs and physical box acceptance
remain outstanding: this is an automatically tested, hardware-unverified candidate.
