# Device connectivity state machine (v2)

Status: **design** — supersedes the boot-decided, reboot-to-switch model in the
current `main/netmgr.c`. Target: BLE and WLAN coexist and are user-switchable, WiFi
provisioning happens over BLE with no reboots, and file sync works whether or not
home WiFi is available.

## Goals

1. **Add device first** (BLE pairing/bonding), then file sync is possible.
2. **Optional WiFi provisioning** over BLE so the device joins home WiFi and the app
   can reach it over the LAN without the manual SoftAP dance every time.
3. **Phone-only still works** when no WiFi is available (BLE control + on-demand
   SoftAP bulk transfer).
4. **User-switchable WLAN ↔ BLE** once provisioned, with automatic fallback to BLE if
   WiFi drops.
5. **No reboots** to enter/leave provisioning; **no `wifi_prov_mgr`**, no
   `prov_pending` flag (removes the reboot-loop / stuck-in-provisioning bug class).

## Two persistent axes (identity)

Both live in NVS and are the top-level state:

| Axis | Meaning | Source of truth |
|------|---------|-----------------|
| **paired** | a phone is BLE-**bonded** ("Add device") | NimBLE bond store (a bond exists) |
| **provisioned** | home-WiFi STA credentials stored | `esp_wifi_get_config(STA)` SSID non-empty |

Plus one persistent preference used only when provisioned:

| Pref | Values | Default |
|------|--------|---------|
| **pref_mode** | `WLAN` / `BLE` | `WLAN` |

### Identity matrix

```
                    UNPAIRED                     PAIRED
              ┌──────────────────────┬──────────────────────────────┐
UNPROVISIONED │ S0 FRESH             │ S1 FIELD                     │
              │ advertise + pairable │ BLE sync (SoftAP handoff);   │
              │                      │ can be provisioned → S2      │
              ├──────────────────────┼──────────────────────────────┤
  PROVISIONED │ S3 ORPHAN            │ S2 FULL (WLAN ↔ BLE switch)  │
              │ on WiFi, advertise   │ the rich state               │
              │ to pair → S2         │                              │
              └──────────────────────┴──────────────────────────────┘
```

Identity transitions (all **live, no reboot**):
- **pair** / **unpair** — bond created / cleared (left ↔ right)
- **provision** / **forget** — creds stored / erased (top ↕ bottom)

## The hardware constraint (non-negotiable)

- **BLE + SoftAP must NEVER be up together** — they starve LWIP and transfers stall
  (see `[[ble-softap-heap-exclusive]]` / commit 656708d). SoftAP is only ever raised
  *transiently* for a bulk transfer, with BLE torn down for its duration, then
  restored. This is today's proven flow (`blesync_stop()` → transfer → `blesync_start()`).
- **BLE + STA (station)** coexist with room to spare — **measured in Stage 0** on
  hardware: BLE-only ≈ 60 KB free, **BLE + STA connected ≈ 55 KB free / 50 KB min**.
  So BLE stays up in WLAN mode for control/presence.
- **BUT the HTTP file server does not fit on top of BLE.** Also measured: bringing up
  `filesrv` (httpd task + mDNS + the precache task + SD/FATFS) on top of BLE + STA
  drops min-free to **~44–136 bytes** (near-OOM) and steady free to ~12 KB. The
  oversized LWIP TCP buffers were **not** the cause (trimming them didn't help) — it's
  the aggregate server bring-up. **Rule: BLE and the HTTP file server never run
  together** — file *serving* (SoftAP *or* STA) tears BLE down for its duration, same
  as today's SoftAP flow. Optional later optimization (defer precache, on-demand httpd,
  smaller stacks) could relax this; not assumed by the design.

## Two transfer paths (the device is always in exactly one)

| Path | When | Mechanics | BLE during transfer |
|------|------|-----------|---------------------|
| **WLAN** | S2 WLAN mode, S3 | file server on the **STA** interface, discovered via mDNS `t10-XXXX.local:8080`; app pulls over the home LAN | **torn down** during the transfer (server can't coexist — Stage 0) |
| **BLE**  | S0/S1, S2 BLE mode | BLE control → SoftAP handoff → bulk transfer | torn down transiently, restored after |

The WLAN path is what removes "always manually sync" — the app just reaches the
device on the network; the device is a server (reuses `filesrv.c` on STA + mDNS).

## Operational FSM (live connectivity)

Layered under the identity. Pure decision logic (testable in isolation, like
`uartrx_sm`); the glue performs radio actions.

```
OP_BLE            BLE advertising/connected; pairing, control, and BLE-path sync.
                  Radio: BLE up, WiFi idle. (S0, S1, S2-BLE, S3-unpaired-advertising)
OP_VERIFYING      candidate creds just written: try to join, NOT yet provisioned.
                  Radio: STA (+BLE). -> OP_STA_UP (confirm) or back to OP_BLE (reject).
OP_STA_CONNECTING provisioned + WLAN: joining home WiFi. Radio: STA (+BLE if coexist).
OP_STA_UP         on home WiFi: file server on LAN + internet. Radio: STA (+BLE).
OP_STA_FAILED     couldn't hold STA -> auto-fallback to OP_BLE, retry per backoff.
OP_SOFTAP         transient bulk transfer: Radio AP, BLE torn down. Returns to prior.
```

**Credential verification (the "wrong password" path).** `provisioned` is set **only
after a confirmed `GOT_IP`**, never on the `WIFI_CREDS` write itself:
- `WIFI_CREDS` write → store as *candidate* → **OP_VERIFYING** (join attempt).
- `GOT_IP` within a timeout → commit creds, `provisioned=true`, `pref_mode=WLAN`,
  notify app "provisioned", → OP_STA_UP.
- fail (bad password / AP not found / timeout) → discard candidate, stay
  **unprovisioned**, notify app "prov failed: <reason>", → OP_BLE. The app can retry.
This prevents a bad password from making the device thrash into WLAN mode every boot.

### Boot decision

```
if provisioned and pref_mode == WLAN:  OP_STA_CONNECTING
else:                                  OP_BLE
```
(No `prov_pending`, no reboot branch. `paired`/`provisioned` are just read from NVS.)

### Key transitions

| From | Event | To | Notes |
|------|-------|----|-------|
| OP_BLE | phone bonds (passkey ok) | OP_BLE | now **paired**; file sync unlocked; mint per-bond token |
| OP_BLE | `WIFI_CREDS` written (paired) | OP_VERIFYING | candidate creds; NOT yet provisioned |
| OP_VERIFYING | GOT_IP | OP_STA_UP | commit creds → **provisioned**; pref_mode←WLAN |
| OP_VERIFYING | fail/timeout | OP_BLE | discard candidate; notify "prov failed"; stay unprovisioned |
| OP_BLE | `START_SOFTAP` (BLE path) | OP_SOFTAP | tear BLE down, handoff, transfer |
| OP_SOFTAP | done / `STOP` / idle-timeout | OP_BLE **or** OP_STA_CONNECTING | back to the base it came from; restore BLE |
| OP_STA_CONNECTING | GOT_IP | OP_STA_UP | start file server on STA + mDNS |
| OP_STA_CONNECTING | retries exhausted | OP_STA_FAILED → OP_BLE | auto-fallback; STA retried per backoff |
| OP_STA_UP | `SET_MODE BLE` / WiFi lost | OP_BLE | user switch or auto-fallback; pref_mode as set |
| OP_BLE | `SET_MODE WLAN` (provisioned) | OP_STA_CONNECTING | user switch back |
| any | `FORGET_WIFI` | OP_BLE | erase creds → unprovisioned (abort any live transfer first) |
| any | `UNPAIR` | OP_BLE | clear that bond → maybe unpaired; advertise for re-pair |
| any | **factory reset** (button-hold) | OP_BLE (S0) | clear ALL bonds + creds + prefs; on-screen confirm |

The **WLAN ↔ BLE switch** (S2) is `SET_MODE`, driven by: a device UI toggle, an app
command (over whichever transport is live), or auto-fallback. All live, no reboot.

## BLE GATT contract (extends the existing `blesync` service)

Service `6F430001-…`. Existing: info `…0002` (READ), control `…0003` (WRITE), status
`…0004` (NOTIFY handoff). Additions:

- **Bonding/security** — NimBLE SM with **passkey display** (`io_cap=DISPLAY_ONLY`,
  `mitm=1`, `bonding=1`); 6-digit code on the TFT. Control + creds characteristics
  require encryption. **paired == a bond exists.** On bond, mint a **per-bond WLAN
  token** and return it over the status characteristic (used by the HTTP server on both
  SoftAP and STA/LAN).
- **`…0005` WIFI_CREDS** (WRITE, encrypted) — SSID + passphrase (TLV/JSON). On write:
  store as *candidate* → OP_VERIFYING; commit + `provisioned` only on confirmed GOT_IP
  (see verification above). Replaces `wifi_prov_mgr`.
- **Control `…0003` opcodes** (extend): `START_SOFTAP`(0x11), `STOP_WIFI`(0x12),
  `SET_MODE_WLAN`(0x13), `SET_MODE_BLE`(0x14), `FORGET_WIFI`(0x15), `UNPAIR`(0x16).
- **Status `…0004`** (extend NOTIFY) — device snapshot JSON:
  `{paired, provisioned, mode, op_state, sta_ip, wlan_token,
    softap:{ssid,pass,ip,port,token}, prov_error?}`.

App side (octanis-connect, not the Espressif app): bond to "add device"; write
`WIFI_CREDS` to provision; `SET_MODE_*` to switch; read status for the current state.

## Security, pairing & recovery

- **Pairing = MITM-protected bonding with a passkey on the TFT.** The device has a
  1.8" display, so pairing uses NimBLE SM **passkey display** (`io_cap = DISPLAY_ONLY`,
  `mitm=1`, `bonding=1`): the device shows a 6-digit code, the app enters it. Prevents
  a random nearby phone from "adding" the device. Just-Works is the fallback only if a
  build has no display.
- **WLAN auth = a persistent per-bond token, not the per-BLE-session token.** At
  pairing (or first provision) the device mints a random secret bound to that phone and
  returns it over the encrypted BLE link. The HTTP file server (SoftAP *and* STA/LAN)
  accepts that token as a Bearer/`?token=`. This makes the WLAN path work even when BLE
  is down (Stage 0 heap result), because the app already holds the token. Stored in NVS,
  cleared on unpair/forget.
- **Multiple phones:** allow up to **N bonds** (NimBLE `CONFIG_BT_NIMBLE_MAX_BONDS`,
  e.g. 3). Each bond gets its own WLAN token. Re-pairing an existing phone replaces its
  bond; a full bond store evicts the least-recently-used.
- **Factory reset (physical, mandatory).** A **button-hold combo** (e.g. BTN1+BTN3 held
  ~5 s, or a dedicated hold) clears **all bonds + all creds + prefs** → S0, with an
  on-screen "Reset? hold to confirm". This is the only recovery when the device is
  bonded to a phone that is gone — BLE `UNPAIR` needs the bonded phone; WLAN needs creds.

## Policies & edge cases

- **STA retry / backoff.** After OP_STA_FAILED (WiFi gone), `pref_mode` stays WLAN; the
  device sits in OP_BLE and retries STA on an exponential backoff (e.g. 30 s → 5 min
  cap). It returns to OP_STA_UP automatically when WiFi comes back; the user can also
  force BLE (`SET_MODE_BLE`) to stop retrying.
- **Mid-operation transitions.** `FORGET_WIFI` / `UNPAIR` / `SET_MODE` while a transfer
  is in flight: **abort the transfer cleanly first**, then apply. During **OP_SOFTAP
  there is no BLE control channel** (BLE is torn down) — the only exits are transfer
  completion, the HTTP `/session/stop`, or the no-client/idle watchdog; the mode switch
  is unavailable until it returns to the base state.
- **SD is a single shared resource.** `viblog` (vibration logging), UART-RX recording,
  and file-sync all write the SD. Policy: **one SD-writing activity at a time** — the
  FSM refuses to start a second (e.g. no SoftAP file-serve while `viblog` is recording)
  and surfaces "busy" rather than corrupting the card via concurrent FAT/SPI access.
- **mDNS per-device hostname.** `t10-XXXX.local` (last 2 MAC bytes, matching the
  `Octanis-XXXX` SoftAP SSID), so multiple devices coexist on one LAN.
- **Power / advertising (acknowledged, deferred).** Indefinite BLE advertising drains a
  battery. Later: slow the adv interval when idle and/or light-sleep between adv events;
  out of scope for the first cut but noted so it isn't designed out.

## What changes in the codebase

- **Remove** `wifi_prov_mgr` usage, the `prov_pending` NVS flag, and the reboot-based
  enter/leave provisioning from `main/provisioning.c` + `main/netmgr.c`.
- **`main/blesync.c`** gains bonding + the `WIFI_CREDS` characteristic + the new
  opcodes; it is the single BLE service (control, provisioning, pairing, handoff).
- **`main/netmgr.c`** becomes the 2-axis FSM above, with a pure decision core
  (`netmgr_sm.c/.h`, host-tested) and live mode-switching (no reboots). BLE is torn
  down **only** for OP_SOFTAP.
- **`main/filesrv.c`** additionally serves on the STA interface (WLAN path) — it
  already binds `:8080` + mDNS; just keep it up in OP_STA_UP.
- Auth/token: the per-session token continues to gate HTTP; over WLAN it is delivered
  over the (bonded, encrypted) BLE status characteristic or minted per session.

## Implementation stages

Each stage builds, passes `make test`, and is independently flashable/testable. Pure
decision logic is developed **red→green TDD** (like `uartrx_sm`).

- **Stage 0 — Heap validation spike (decides the design's "always-on BLE").** Bring
  up BLE + STA + `filesrv` together on hardware; log free heap under load. If it
  fits: BLE stays up in WLAN mode. If not: WLAN mode is STA-only and BLE comes up on
  switch/fallback (a ~1–2 s live swap). Cheap throwaway branch; no product code.

- **Stage 1 — Provisioning over blesync, drop `wifi_prov_mgr`.** Add `WIFI_CREDS`
  characteristic; on write, **candidate → OP_VERIFYING → commit only on GOT_IP**, with a
  failure notify (the "wrong password" path). Remove `prov_pending`, the reboot paths,
  and the wifi-prov event handling. Biggest bug-kill.

- **Stage 2 — Bonding, pairing & recovery.** NimBLE SM **passkey-display** bonding
  (6-digit on the TFT), `mitm=1`; `paired` = bond exists; encrypt control + creds;
  per-bond WLAN token; multiple bonds (LRU-evict); `UNPAIR` opcode; **physical
  button-hold factory reset** (clear all bonds+creds+prefs → S0, on-screen confirm).
  Gates file sync on paired.

- **Stage 3 — netmgr v2 FSM.** `netmgr_sm.c/.h` pure core (TDD: every transition, boot
  decision, verification confirm/reject, STA auto-fallback + **backoff**, mid-op
  **abort** semantics, SoftAP-from-WLAN-and-back, SD **one-writer** guard). Live WLAN↔BLE
  switch; WLAN file server on STA + `t10-XXXX.local` mDNS; OP_SOFTAP tears BLE down only
  transiently.

- **Stage 4 — UI + app contract.** Home/Settings mode toggle (WLAN/BLE) + pairing/
  provisioning/mode status on the TFT; passkey display during pairing; write the
  app-side GATT contract for the octanis-connect agent.

- **Stage 5 — Cleanup.** Remove DEV shortcuts (static token/pass, `/info` token,
  `/speedtest`, `/sdread`); update `docs/DEVICE_FILE_SYNC.md`; final audit (stacks,
  heap under coex, DEV flags gone).

### Decisions
1. WLAN sync = **device-as-server on STA + mDNS** (app pulls). Not cloud push.
2. Mode switch = **UI toggle + app command + auto-fallback** (all three).
3. Pairing = **passkey shown on the TFT** (MITM-protected), not Just-Works.
4. WLAN auth = **persistent per-bond token** (works even when BLE is down).
5. Provisioning/pairing done by **the octanis-connect app over blesync** (not the
   Espressif app).
6. **BLE-in-WLAN-mode** (Stage 0 result): BLE **stays up in idle WLAN mode** (control),
   but the **HTTP file server never coexists with BLE** — file serving over STA or
   SoftAP tears BLE down for the transfer, then restores it. So the WLAN win over
   SoftAP is "phone keeps its network" (no SSID switch), not "BLE stays up during the
   transfer."
