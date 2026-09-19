# Firmware updates (OTA)

TeddyCloud keeps the firmware-update policy for TB1 and TB2 separate. TB1
downloads come from Boxine and use the TB1 firmware directories. TB2 downloads
come from TONIES and use the TB2 firmware directories. Firmware from one
generation is never offered to the other generation.

## Settings

| Generation | Cloud source | Cache downloaded OTA | Serve local OTA |
|---|---|---|---|
| TB1 | `cloud.enableV1Ota` | `cloud.cacheOtaV1` | `cloud.localOtaV1` |
| TB2 | `cloud.enableV3Ota` | `cloud.cacheOtaV3` | `cloud.localOtaV3` |

The TB2 cloud-source switch controls both the `/v3/check-ota` request and the
subsequent firmware download. The generation-wide cloud masters
`cloud.enabled` (TB1) and `cloud.tb2_v3_enabled` (TB2) can additionally prevent
cloud access. Local delivery of an already cached file remains independent of
those cloud masters.

New installations cache OTA downloads for both generations but do not serve
them locally by default:

- `cloud.cacheOtaV1=true`
- `cloud.localOtaV1=false`
- `cloud.cacheOtaV3=true`
- `cloud.localOtaV3=false`

## Behaviour matrix

| Cloud source | Cache | Local delivery | Behaviour |
|---|---:|---:|---|
| On | Off | Either | The firmware is streamed directly from Boxine or TONIES to the box and is not stored. |
| On | On | Off | The firmware is downloaded and stored completely, but is not delivered to the box. |
| On | On | On | The firmware is stored completely and then delivered from the local cache. |
| Off | Either | On | No new cloud download occurs; an already available matching OTA can be delivered for a concrete request. |
| Off | Either | Off | No new OTA is downloaded and no local OTA is delivered. |

Caching alone does not install an update. Disabling cloud access also means
that TeddyCloud cannot discover an unknown update; for TB2, disabling the
update request prevents a new TONIES OTA description from being obtained.
Changing a switch never deletes previously stored firmware files.

## Configuration migration

Configuration version 25 replaces the shared `cloud.cacheOta` and
`cloud.localOta` settings. During migration, each old global value is copied to
the corresponding TB1 and TB2 settings. An explicit overlay value and its
overlay marker are likewise copied to both generations. The old setting IDs
remain load-only migration inputs and are no longer exposed or saved.
