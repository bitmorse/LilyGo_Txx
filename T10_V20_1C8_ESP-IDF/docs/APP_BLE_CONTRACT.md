# App ↔ device BLE contract (octanis-connect)

Everything the phone app needs to add a device, provision WiFi, sync files, and
switch connectivity — all over one BLE GATT service (`blesync`). Companion to
`docs/DEVICE_STATE.md` (device behavior) and `docs/DEVICE_FILE_SYNC.md` (HTTP API).

> **This document is the authoritative wire format** (opcode bytes, characteristic
> UUIDs, JSON field names). `DEVICE_STATE.md` describes device *behavior*; where the
> two differ on the bytes/JSON on the wire, this file wins. The app should still parse
> JSON **defensively** (ignore unknown fields; tolerate missing optional ones).

## GATT service

Base UUID `6F4300xx-A1B2-4C3D-9E5F-0123456789AB` (byte `xx` selects the characteristic):

| Char | `xx` | Props | Purpose |
|------|------|-------|---------|
| **Service** | `01` | — | primary service (advertised in the scan response) |
| **Info** | `02` | Read | device-state snapshot (JSON, below) |
| **Control** | `03` | Write | 1-byte opcodes (below) |
| **Status** | `04` | Notify | pushed events: handoff, prov result, state snapshot |
| **WIFI_CREDS** | `05` | Write | provision: `<ssid>\0<pass>` bytes |

Device advertises as **`Octanis-XXXX`** (last 2 MAC bytes; also the SoftAP SSID).
Subscribe to **Status** (`04`) right after connecting — it carries all async replies.

### Control opcodes (write 1 byte to `03`)

| Op | Name | Effect |
|----|------|--------|
| `0x11` | START_SYNC | begin a file-sync session; device replies on Status with a **handoff** (picks SoftAP or WLAN transport) |
| `0x12` | STOP_SYNC | end the session → device returns to idle |
| `0x13` | SET_MODE_WLAN | prefer WiFi (join home network) |
| `0x14` | SET_MODE_BLE | prefer BLE (WiFi off, sync over BLE/SoftAP) |
| `0x15` | FORGET_WIFI | erase stored credentials → device returns to sync-idle |
| `0x16` | UNPAIR | clear this device's bond ("remove device") |

### Status / Info JSON

**Info (`02`) read** and **Status (`04`) state pushes** — the device snapshot:
```json
{"state":"sta-connected","provisioned":true,"paired":true,
 "mode":"wlan","ip":"192.168.1.42","dev":"T10_88F199"}
```
`state` ∈ `sync-idle | sta-connecting | sta-connected | sta-failed | verifying |
softap | wlan-serve`. `mode` ∈ `wlan | ble`.

**Status (`04`) async events** (distinguish by keys):
- provisioning result: `{"prov":"ok"}` or `{"prov":"fail","err":"…"}`
- SoftAP handoff: `{"mode":"softap","ssid":"Octanis-XXXX","pass":"…","ip":"192.168.4.1","port":8080,"token":"…"}`
- WLAN handoff: `{"mode":"wlan","ip":"192.168.1.42","port":8080,"token":"…"}`

## Flows

### 1. Add device (pairing)
Scan for `Octanis-XXXX` → connect → **bond**. The device shows a **6-digit passkey
on its screen**; the user types it into the app (MITM passkey pairing). The bond
persists on the device. `paired` in the snapshot becomes `true`.
> Today bonding is **optional** (file sync works unpaired). A later firmware change
> will make it **required** for provisioning + sync — implement pairing now.

### 2. Provision WiFi
Write **WIFI_CREDS (`05`)** = `utf8(ssid) + 0x00 + utf8(pass)` (empty pass = open).
Then wait on **Status** for `{"prov":"ok"}` (device joined; now on the LAN) or
`{"prov":"fail","err":…}` (creds discarded — retry). The device only commits the
creds after a confirmed IP, so a wrong password can't strand it.

### 3. Sync files
Write **START_SYNC (`0x11`)**. Read the **handoff** notification:
- `mode:"softap"` → join WiFi SSID `ssid` with `pass`, then HTTP to `http://192.168.4.1:8080`.
- `mode:"wlan"` → the device is already on the current network; HTTP to `http://<ip>:8080`
  (or `http://t10-XXXX.local:8080` via mDNS). No WiFi switch.

Then use the **HTTP API** (`docs/DEVICE_FILE_SYNC.md`), authenticating with `token`
(as `Authorization: Bearer <token>` or `?token=<token>`): `GET /manifest`,
`GET /file/<id>` (Range-resumable), `DELETE /file/<id>`. When done, `POST /session/stop`
(or send **STOP_SYNC `0x12`**). During the transfer **BLE is down** (the device can't
run BLE + the HTTP server together); it re-advertises when the session ends.

### 4. Switch connectivity
Write **SET_MODE_WLAN (`0x13`)** or **SET_MODE_BLE (`0x14`)**. In BLE mode the device
turns WiFi off and syncs via SoftAP; in WLAN mode it rejoins home WiFi. The choice
persists across reboots. If WiFi drops in WLAN mode the device auto-falls-back and
keeps retrying.

### 5. Remove device / recover
**UNPAIR (`0x16`)** clears the bond. If the phone is lost, the device is recovered by
a **physical factory reset**: hold ENTER + DOWN (the top and bottom buttons) for 5 s —
wipes bonds + creds + settings.

## Notes
- One sync session at a time; the device refuses a sync while vibration-logging (SD busy).
- The `token` is currently a fixed dev value and will become a per-bond secret with the
  pairing-required change; treat it as opaque.
