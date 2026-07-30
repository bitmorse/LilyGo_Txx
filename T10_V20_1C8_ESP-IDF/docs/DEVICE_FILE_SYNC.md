# Device ↔ Phone File Sync — canonical spec

**Status:** draft / design, **rev 2** (incorporates the app-side review). This is
the single source of truth for the firmware ↔ app contract. Firmware (this repo)
and the phone app (octanis connect, Expo/RN) both implement against it. Change it
here first, then both sides follow.

> **rev 2 changes:** BLE service now requires LE encryption/bonding (§3, §7); added
> `POST /session/stop` + auto-teardown (§6, §8.5); `GET /info` is unauthenticated
> for reachability probing (§6); sharpened the AccessorySetupKit **SSID-capture**
> rule — the full SSID must be broadcasting at pairing time (§8.1, §15); made
> **persistent BLE (no FREE_BTDM) + SW coexistence** an explicit firmware
> requirement with a RAM caveat (§8.6); manifest is content-agnostic with a `mime`
> field and normative `sha256` (§4); per-platform BLE throughput (§13).

**Goal:** a Plaud-class experience — pair once (AirPods-style card), then the app
lists the device's recorded files over BLE and pulls them fast over a **direct
Wi-Fi link the OS joins silently, no trip to Settings**. Works on iOS and Android,
at home or in the field.

> Content-agnostic: the transport below moves any files the device writes to SD.
> Today that's the vibration `.mcap` files; the same plumbing serves audio clips
> or future recordings. (Note: this board has **no onboard microphone** — a voice
> recorder product would need an added I²S MEMS mic; that's orthogonal to sync.)

---

## 1. Architecture — two transports, one control plane

- **BLE (control plane, always):** pairing, capability/health, file listing,
  delete, transfer negotiation, and handing the phone the Wi-Fi credentials.
  Also carries *small* files directly. Never the bulk path (far too slow).
- **Wi-Fi (bulk plane):** the device serves files over HTTP; the phone pulls them
  at 10–100× BLE speed. Two variants, auto-selected per session:
  - **SoftAP (hero / universal):** device brings up its own access point; the OS
    joins it **silently** via AccessorySetupKit + `joinAccessoryHotspot` (iOS) /
    `WifiNetworkSpecifier` (Android). Works with no router, in the field, before
    provisioning. This is the "magic."
  - **Same-LAN (optimization):** when the device is already provisioned onto the
    phone's Wi-Fi, skip the hotspot entirely and pull over the existing LAN. No
    network switch, phone keeps internet, BLE stays fully live. Preferred *when
    available*, but never required.

**Why SoftAP is the hero, not the fallback:** with a native module in scope,
`joinAccessoryHotspot` removes iOS's "drops an internet-less network in ~60 s"
problem and AccessorySetupKit removes the permission-prompt friction — so the
universal path is also the premium one. Same-LAN is a transparent speed/UX win
layered on top when the phone and device happen to share a network.

### Transport selection (app logic)
```
after BLE pair → read `info` characteristic → { provisioned, station_ip, … }
  provisioned?  → probe: GET http://<station_ip>:<port>/info  (UNauthenticated, ~1s timeout)
    ├─ 200 → SAME-LAN  (START_LAN for a token, then GET /manifest,/file/… — no Wi-Fi join)
    └─ else → SOFTAP   (START_SOFTAP → joinAccessoryHotspot → GET http://192.168.4.1/…)
  unprovisioned → SOFTAP directly
```
The reachability probe is an **unauthenticated `GET /info`** (§6) so it needs no
token; the transfer token comes from `START_LAN`/`START_SOFTAP` afterward.

---

## 2. The "magic" sequence (foreground)

```
[AccessorySetupKit / CDM card]  one tap → BLE paired, app holds ASAccessory
        │
BLE  read  info                 → { provisioned, station_ip, softap_ssid, … }
BLE  write LIST                 → device streams manifest (chunked notify)
        │  user picks files / "sync all"
        ├── SAME-LAN ────────────────────────────────────────────────
        │     GET http://<station_ip>:<port>/file/<id>  (Range, Bearer)
        │     BLE stays live for progress/abort (STA+BLE is stable)
        │
        └── SOFTAP ──────────────────────────────────────────────────
              BLE  write START_SOFTAP
              BLE  status WIFI_HANDOFF → { ssid, pass, ip, port, token }
              joinAccessoryHotspot(accessory, pass)      ← silent, no Settings
              GET http://192.168.4.1:<port>/file/<id>    (Range, Bearer)
              (control/progress over HTTP; keep BLE idle — coex)
              BLE/HTTP STOP_WIFI → device tears down AP, reverts to STA
        │
verify sha256 per file → DELETE (device frees space) → upload to Supabase
```

---

## 3. BLE GATT contract

**Custom 128-bit UUIDs (provisional — regenerate as random UUIDs before release
so no two products collide):**

| Role | UUID (provisional) | Properties |
|------|--------------------|-----------|
| Service | `6F430001-A1B2-4C3D-9E5F-0123456789AB` | — |
| `info` | `6F430002-A1B2-4C3D-9E5F-0123456789AB` | Read |
| `control` | `6F430003-A1B2-4C3D-9E5F-0123456789AB` | Write |
| `status` | `6F430004-A1B2-4C3D-9E5F-0123456789AB` | Notify |
| `manifest` | `6F430005-A1B2-4C3D-9E5F-0123456789AB` | Notify |
| `data` | `6F430006-A1B2-4C3D-9E5F-0123456789AB` | Notify |

App requests **MTU 517** and **high connection priority** right after connect;
honor the negotiated MTU. Use **notifications** (not indications) for throughput.

**Security (required):** the entire sync service requires an **encrypted, bonded**
LE link. `control`, `status`, `manifest`, `data` are all **encryption-required**
(NimBLE `BLE_GATT_CHR_F_*_ENC`), and `WIFI_HANDOFF` (which carries the WPA2
passphrase + bearer token) is only ever sent on an encrypted link — never
plaintext GATT. On iOS the AccessorySetupKit flow establishes the bond; on Android
CDM/bonding does. If the link is unencrypted, the device replies `ERROR auth`.

### 3.1 `control` — commands the app writes
Wire format: `[opcode:u8][args…]`, little-endian. File `id` is the integer from
the manifest.

| Op | Name | Args | Effect |
|----|------|------|--------|
| `0x01` | `LIST` | — | stream the manifest over `manifest` |
| `0x02` | `GET_BLE` | `id:u16, offset:u32` | stream file bytes over `data` (small files / no-Wi-Fi) |
| `0x03` | `DELETE` | `id:u16` | delete file (app must have verified it first) |
| `0x10` | `START_LAN` | — | reply `WIFI_HANDOFF{mode:"lan"}` on `status` |
| `0x11` | `START_SOFTAP` | — | bring up SoftAP; reply `WIFI_HANDOFF{mode:"softap"}` |
| `0x12` | `STOP_WIFI` | — | tear down SoftAP / free token, revert to STA |
| `0x20` | `ABORT` | — | cancel the in-flight op |

### 3.2 `status` — device → app events
Wire format: `[type:u8][payload…]`.

| Type | Name | Payload |
|------|------|---------|
| `0x81` | `ACK` | `echo_opcode:u8, result:u8` |
| `0x82` | `WIFI_HANDOFF` | UTF-8 JSON (see §5) |
| `0x83` | `PROGRESS` | `id:u16, bytes_done:u32` (BLE data path) |
| `0x84` | `ERROR` | `result:u8, msg_len:u8, msg:utf8` |
| `0x85` | `MANIFEST_META` | `total_len:u32, total_chunks:u16` (sent before manifest chunks so the app shows load progress + detects truncation) |

`result` codes: `0` OK · `1` not_found · `2` busy · `3` wifi_failed ·
`4` auth · `5` no_space · `6` unsupported.

### 3.3 Chunk framing (used by `manifest` and `data`)
Each notification: `[seq:u16][flags:u8][payload…]`, `flags` bit0 = **LAST**.
Reassemble by contiguous `seq` 0..N; complete when the LAST chunk arrives. Abort
and retry `LIST`/`GET_BLE` on a gap.

---

## 4. File manifest (BLE `manifest` and HTTP `GET /manifest`)
```json
{
  "v": 1,
  "free_bytes": 59000000000,
  "files": [
    { "id": 7,
      "name": "vib0007.mcap",
      "mime": "application/mcap",
      "bytes": 1463637,
      "created_at": "2026-07-30T18:03:11Z",
      "duration_ms": 6457,
      "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08" }
  ]
}
```
- **Content-agnostic:** `mime` tells the app how to treat each file (e.g.
  `application/mcap`, `audio/opus`). Today the device writes vibration `.mcap`;
  the app must handle files opaquely (list name/size/date, upload as blobs) — **no
  media player in v1**, and `duration_ms` means *capture length*, not necessarily
  audio. `duration_ms` is optional.
- **`sha256` is normative** (not optional): the ESP32 has a **hardware SHA
  accelerator**, so compute it once **when the file is finalized** and cache it
  (SD sidecar/journal) — the manifest then serves it cheaply. The app uses it as
  the Supabase dedup / idempotency key and to verify before delete. It **equals**
  the HTTP `ETag` for the same file (§6).

---

## 5. Wi-Fi handoff message (`status` `WIFI_HANDOFF`, JSON)
Same-LAN:
```json
{ "mode": "lan", "ip": "192.168.1.42", "port": 8080, "token": "<32-hex>", "tls": false }
```
SoftAP:
```json
{ "mode": "softap", "ssid": "Octanis-AB12", "pass": "<random-wpa2>",
  "ip": "192.168.4.1", "port": 8080, "token": "<32-hex>" }
```
`token` is a per-session bearer (see §7). `ssid` **must** match the static prefix
declared to AccessorySetupKit (see §8).

---

## 6. HTTP API (device `esp_http_server`)
All requests carry `Authorization: Bearer <token>` **except `GET /info`**, which is
unauthenticated so the app can do a bare **reachability probe** (see §1 note). The
STA-mode HTTP server is **always-on** while provisioned, so the probe can succeed
before any BLE handoff.

| Method / path | Auth | Behavior |
|---------------|------|----------|
| `GET /info` | none | capability/health JSON (§9); reveals no file contents |
| `GET /manifest` | token | same JSON as the BLE manifest (§4) |
| `GET /file/<id>` | token | raw bytes; **supports `Range:`** → `206 Partial Content`, `Accept-Ranges: bytes`, `Content-Length`, `ETag: "<sha256>"` (**== manifest `sha256`**) |
| `DELETE /file/<id>` | token | delete after the client has verified the file |
| `POST /session/stop` | token | tear down SoftAP, revert to STA, **invalidate the token** — the in-band teardown when BLE is idle (§8.5) |

`Range` is parsed and served manually — `esp_http_server` does not do it for you.

---

## 7. Security (local hop)
No TLS on the local link (mbedTLS is too heavy on this no-PSRAM part and cuts
throughput). Instead:
- **WPA2** encrypts the SoftAP link; the **passphrase is random per session** and
  delivered only over the **paired, encrypted BLE channel**.
- A **random bearer `token`** (handed over the **encrypted** BLE link) authorizes
  every HTTP request except `GET /info`. It has a **TTL (default 10 min, refreshed
  by activity)** and is **invalidated** on `POST /session/stop`, `STOP_WIFI`, idle
  timeout, or AP-client-disconnect. `GET /info` is unauthenticated (leaks only
  presence/health, never file contents) so reachability can be probed first.
- Same-LAN mode uses the same token so a random LAN peer can't scrape recordings.

Reserve TLS for anything that leaves the local link (phone → Supabase).

---

## 8. Firmware hard requirements (locked in by AccessorySetupKit)
These are **non-negotiable** — the iOS silent-join flow breaks without them:

1. **Static, deterministic SoftAP SSID that is actually broadcasting at
   AccessorySetupKit pairing time.** e.g. `Octanis-AB12` (suffix = last two MAC
   bytes). The static-prefix rule alone is *necessary but not sufficient*: reports
   show `joinAccessoryHotspot` fails with "invalid SSID" when ASK captured only a
   **prefix**, because the join uses the exact `ASAccessory.ssid` captured at
   pairing and that value **does not update afterward**. → **The device must be
   Wi-Fi-discoverable with its full SSID during the ASK onboarding window** (run
   SoftAP-beacon during setup; SoftAP-beacon + BLE-advertise is coex-safe — it's
   only SoftAP + *active BLE transfer* that's unstable). Proving the exact ASK
   capture behavior is the **#1 job of the §15 spike.**
2. **Per-session WPA2 passphrase delivered over the encrypted BLE link**
   (`WIFI_HANDOFF`). SSID is discoverable/static; the secret is not.
3. **Advertise the BLE service UUID** (§3) so AccessorySetupKit / CDM discover the
   device, then the app reads the passphrase and calls `joinAccessoryHotspot`.
4. **Mode switch:** `START_SOFTAP` does `esp_wifi_stop → set_mode(AP) → start`;
   `STOP_WIFI` / `POST /session/stop` restores STA from NVS creds. The device is
   **off home Wi-Fi during a SoftAP session** (no NTP/radio/cloud) — expected;
   reverts after.
5. **Coexistence & teardown.** During a SoftAP transfer the BLE link stays
   **connected but idle** — the connection is *maintained* (so a single small
   `STOP_WIFI` write still works), but no high-throughput BLE runs concurrently
   with Wi-Fi (SoftAP + active BLE is "C1 unstable" on classic ESP32, no PSRAM).
   Drive control/progress over **HTTP** in SoftAP mode. **Primary teardown is
   in-band: `POST /session/stop`.** Safety nets (device-side, mandatory): tear the
   AP down and invalidate the token on **AP-client-disconnect** and on an **idle
   timeout** (default 60 s no HTTP activity), so a walked-away phone never strands
   the device in AP mode. In **same-LAN mode BLE stays fully live** (STA+BLE is
   stable) — a reason to prefer same-LAN when co-located.
6. **Persistent BLE (this reverses a current firmware optimization).** The sync
   control channel means BLE must be **always available**, so the firmware can no
   longer `FREE_BTDM` after provisioning (it does today — that's why BLE turns off
   post-provision to reclaim RAM). NimBLE now runs permanently alongside Wi-Fi, and
   **SW coexistence** (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`) must be enabled. **RAM
   must be re-validated** — BLE + Wi-Fi + LVGL together on a no-PSRAM part is why
   FREE_BTDM existed. If RAM is too tight for always-on BLE, the fallback is
   **on-demand BLE** (advertise only while charging / on a button press), which
   costs the seamless "open app → it's there" UX. Resolve during the spike.

**Provisioning vs sync, cleanly separated by device state:**
- **Unprovisioned** → advertise the existing `wifi_provisioning` service; run the
  provisioning manager (as today). The app provisions with
  `react-native-esp-idf-provisioning` (its own Security-1 flow).
- **Provisioned** → advertise the **file-sync service** (§3) for ASK/CDM discovery
  + `react-native-ble-plx`. "Forget WiFi" returns to the provisioning state.
So the two BLE flows never run at once; only one service is discoverable per state.
This sync mode is **mutually exclusive with provisioning only in the SoftAP case**
(both want the AP). Provisioning + same-LAN transfer coexist fine. Surface a
per-session transport choice, not a global feature switch.

---

## 9. `GET /info` / `info` characteristic JSON
```json
{ "fw": "1.4.0", "provisioned": true, "station_ip": "192.168.1.42",
  "softap_ssid": "Octanis-AB12", "softap_capable": true,
  "battery_pct": 78, "free_bytes": 59000000000, "file_count": 42 }
```
Lets the app choose the transport (and whether a hotspot is even needed) *before*
prompting the user.

---

## 10. Native module API surface (for the app team's spike)

### iOS (Swift bridge, the main lift)
```ts
// AccessorySetupKit + NEHotspotConfiguration.joinAccessoryHotspot wrapper
discoverAndPair(opts: {
  bleServiceUUID: string;   // §3 service UUID
  ssidPrefix: string;       // e.g. "Octanis-"
  displayName?: string;
}): Promise<{ id: string; bluetoothIdentifier: string; name: string }>;

joinAccessoryHotspot(opts: { accessoryId: string; passphrase: string }): Promise<void>;
leaveHotspot(): Promise<void>;
listPaired(): Promise<Accessory[]>;
removeAccessory(id: string): Promise<void>;
// events: onAccessoryAdded, onAccessoryRemoved, onPickerDismissed
```
BLE comms then go through `react-native-ble-plx` using `bluetoothIdentifier`
(`retrievePeripherals`). **Entitlement:** `com.apple.developer.networking.HotspotConfiguration`.
**Info.plist:** `NSAccessorySetupKitSupports = [Bluetooth, WiFi]`,
`NSAccessorySetupBluetoothServices`, `UIBackgroundModes: bluetooth-central`
(for later background phase).

### Android (lighter — WifiNetworkSpecifier; `react-native-wifi-reborn` may suffice)
```ts
joinLocalOnlyWifi(opts: { ssid: string; passphrase: string }): Promise<{ networkHandle: string }>;
leaveWifi(handle: string): Promise<void>;   // unregisterNetworkCallback
```
Requests a network with `INTERNET` capability removed; binds sockets to the
returned `Network`. **Permissions:** `BLUETOOTH_CONNECT`,
`BLUETOOTH_SCAN(neverForLocation)`, `NEARBY_WIFI_DEVICES(neverForLocation)`,
`CHANGE_NETWORK_STATE`, `CHANGE_WIFI_STATE`. Optional Companion Device Manager for
pairing parity.

---

## 11. Apple-imposed limits the module can NOT remove (design around)
- **`joinAccessoryHotspot` forces `joinOnce = YES`** — no API to persist an
  accessory-hotspot join → **rejoin per sync session** (silent, ASK-remembered).
  Fine for tap-to-sync; means no fully-silent *background* SoftAP rejoin.
- **Background auto-sync** rides iOS's throttled background BLE + best-effort
  Wi-Fi — a *later* phase, not part of the core magic. Foreground "open app → it
  syncs" is fully achievable now.
- **`joinAccessoryHotspot` completion ≠ associated** — verify with a reachability
  probe (`GET /info`) before starting the bulk transfer.

---

## 12. Integrity · delete · resume
- Device keeps a file until the client (a) downloads it fully and (b) verifies
  `sha256`/`ETag`, then (c) issues `DELETE`. Journal `synced` state on the SD so a
  session interrupted by a BLE↔Wi-Fi switch is idempotent.
- Interrupted transfers resume via HTTP `Range`.
- Manifest-driven: app pulls `/manifest`, diffs, requests only missing/incomplete
  files. Files ultimately land in **Supabase** (Storage + a `recordings` row),
  phone acting as relay — mirroring the existing telemetry sync engine. A
  device-direct STA→cloud "sync while charging" path is a possible later phase.

---

## 13. Throughput expectations (classic ESP32-D0WD, this board)
| Path | Realistic | 1-hour file (~14 MB) |
|------|-----------|----------------------|
| BLE — **Android** (app can tune MTU 517 + high priority) | ~20–60 KB/s | ~4–12 min |
| BLE — **iOS** (OS auto-negotiates ~185 MTU; `ble-plx` can't set MTU/priority) | **~5–20 KB/s** | ~12–45 min |
| Wi-Fi HTTP-from-SD (**1-bit SPI SD is the ceiling**, ~1–4 MB/s) | ~2–6 MB/s | ~3–7 s |

Wi-Fi is ~10–100× BLE here (more on iOS). The SD-on-SPI wiring — not the radio —
is the bulk limiter; SDMMC 4-bit would be faster but this board is wired for SPI.
BLE is for the file list + the occasional small file only.

---

## 14. Phasing — two parallel tracks

SoftAP is the **hero destination** and its iOS module is **not deferred** — it
starts immediately as the risk spike. In parallel, a cheaper same-LAN track proves
the shared GATT+HTTP contract and ships a real fast transfer while the module is
de-risked. Same code on both sides; only the transport differs.

**Track A — contract + same-LAN (cheap, ships value, no custom iOS module):**
1. BLE pairing + control + file list over `react-native-ble-plx` — proves the GATT
   contract end to end.
2. Same-LAN HTTP transfer (`START_LAN` → `fetch` with `Range`) — real fast
   transfer for the common "provisioned, co-located" case.

**Track B — SoftAP hero (the magic; the critical-path risk, run concurrently):**
3. **Week-1 §15 spike** — ESP32 SoftAP + iOS `joinAccessoryHotspot`.
4. iOS `AccessorySetupKit + joinAccessoryHotspot` **native module** + firmware
   always-on-BLE / SoftAP mode → universal, promptless transfer everywhere.
5. Android parity (`WifiNetworkSpecifier` + CDM / `react-native-wifi-reborn`).

**Later:** background auto-sync (iOS best-effort) and **Wi-Fi Aware** when ESP32↔
iOS NAN interop is fixed (open Espressif bug today — do not build on it in 2026).

---

## 15. Week-1 de-risk spike (do this before committing the full build)
Stand up an ESP32 **SoftAP broadcasting its full static SSID**, pair it via
AccessorySetupKit from a throwaway iOS 18 app, join via **`joinAccessoryHotspot`**,
and pull a multi-MB file over HTTP. Answer these specific questions — they carry
essentially all the risk:
1. **SSID capture:** does ASK capture the **full** SSID (so `joinAccessoryHotspot`
   works), or only the prefix (→ "invalid SSID")? Must the SoftAP be **broadcasting
   during pairing** for capture to succeed? (§8.1)
2. **Promptless?** Is the join actually silent under the ASK grant, or does
   `apply/join` still show a system prompt? ("silent" is intent, not yet proven.)
3. **Stability:** does the internet-less link survive long enough for a full
   multi-MB pull (the ~60 s drop that `joinAccessoryHotspot` is supposed to fix)?
4. **RAM:** with **always-on NimBLE + Wi-Fi + coex** (no FREE_BTDM, §8.6), does the
   firmware fit without PSRAM — or do we fall back to on-demand BLE?
5. **Same-LAN prompt:** does a plain `GET http://<station_ip>` under the ASK grant
   avoid the iOS **Local Network** prompt, or must the app declare
   `NSLocalNetworkUsageDescription`? (decides whether same-LAN is truly prompt-free)

If 1–3 hold, the SoftAP hero path is executable; 4–5 tune the firmware/app details.

---

## 16. Open decisions
- **Primary use case = the deciding input for sequencing.** If mostly
  *at-home/provisioned* → Track A (same-LAN) leads and ships first, Track B
  (SoftAP) runs in parallel. If mostly *field/unprovisioned* → there's no LAN to
  fall back to, so Track B's iOS-module risk must be taken up front. **Firmware
  recommendation: run both tracks concurrently** (SoftAP isn't deferred; same-LAN
  ships value while the module is de-risked). *Owner: product — pick the primary
  use case.*
- **Primary launch platform:** Android is far cheaper for SoftAP
  (`react-native-wifi-reborn` covers the join) — a good way to validate the
  SoftAP end-to-end path before investing in the iOS ASK module.
- **Resolved — integrity field:** `sha256`, HW-accelerated, computed at file
  finalize and cached; also the Supabase dedup key and the HTTP `ETag` (§4, §6).
- **Final (random) production UUIDs** for the GATT service/characteristics — lock
  them into §3 before either side hardcodes them.
- **From the §15 spike:** (a) does same-LAN avoid the iOS Local Network prompt
  under the ASK grant, or must the app declare `NSLocalNetworkUsageDescription`?
  (b) does always-on BLE + Wi-Fi + coex fit in RAM without PSRAM, or do we fall
  back to on-demand BLE? Both feed back into §8.

## App-side counterpart (tracked by the app team, noted here for the contract)
- Backend `device_files` table (device_id, remote_id, name, mime, bytes, sha256,
  captured_at, storage_path, sync/RLS cols) + private Storage bucket; **resumable
  (tus) uploads** for the 14 MB+ files, not a single PUT.
- Native modules per §10; Expo config plugins + dev build (Expo 56 / RN 0.85);
  the `HotspotConfiguration` entitlement needs a paid Apple account (grantable,
  no special review).

## References
- Apple AccessorySetupKit / `joinAccessoryHotspot`; NetworkExtension Hotspot
  Configuration entitlement.
- Android `WifiNetworkSpecifier` (local-only network), Companion Device Manager.
- ESP-IDF `esp_http_server` file serving; RF coexistence (STA+BLE stable,
  SoftAP+BLE "C1 unstable"); Wi-Fi Aware/NAN (esp32↔iOS interop bug #16743).
- Plaud "Wi-Fi Fast Transfer" (BLE control + Wi-Fi bulk, revert to BLE).
