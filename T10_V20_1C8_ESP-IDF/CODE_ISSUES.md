# Code issues backlog

Findings from the 2026-08-06 multi-agent audit of `main/`. The Critical/High crash
& data-loss paths (originally #1–#5) are being fixed separately, in stages. This file
tracks everything else: Medium robustness, Low/latent, and code-quality work.

Severity: 🟡 medium · 🟢 low/latent · 🧹 cleanup. Each line cites `file:line` (approx).

## 🟡 Medium — robustness / lifecycle

- **Precache task never stopped.** `filesrv_stop()` (`filesrv.c:353`) stops httpd + frees
  mDNS but never stops the `sha_cache` precache task (`filesrv.c:346`). After a session it
  keeps reading/hashing the SD at full speed (the `manifest_precache_hold` throttle is no
  longer toggled), contending with a later viblog/uartrx recording; a quick stop→start
  spawns a *second* precache task. Fix: track the task handle, delete it in `filesrv_stop`
  (or make it one-shot / re-joinable).

- **`blesync_start()` return ignored + partial-init.** Every caller in `netmgr.c`
  (`enter_sta/enter_sync/enter_verifying`, 122–154) ignores the bool. On a failed
  `nimble_port_stop` (`blesync.c:378`, sets `s_active=false` without deinit) or a failed
  `ble_gatts_add_svcs` (`blesync.c:338`), NimBLE is left half-initialized → the next
  `blesync_start()` re-inits a live stack and fails → BLE stuck down until reboot while
  netmgr advances as if BLE is up. Fix: check the return; on failure, surface it / retry /
  reflect "BLE down" in state.

- **`MSG_SOFTAP_START` dropped in `NET_VERIFYING`/`NET_BOOT`.** (`netmgr.c:232`) The File
  Sync page then shows "Starting SoftAP…" forever (`ui_menu.c:581`) since `NET_SOFTAP` is
  never reached. Fix: either queue/defer the request, or have the UI reflect "busy, try
  again".

- **`blesync_is_paired()` hit from the LVGL task every ~1 s.** The new status bar
  (`ui_menu.c:863`) calls `ble_store_util_count()` (`blesync.c:62`) concurrently with the
  NimBLE host task — store access from arbitrary task context isn't guaranteed safe. Fix:
  cache the paired flag (update it from `BLE_GAP_EVENT_ENC_CHANGE`/disconnect) and have the
  UI read the cached value.

- **`wifi_scan_cb` blocks the LVGL task.** (`ui_menu.c:205`) Runs a blocking
  `esp_wifi_scan_start(...,true)` while holding the LVGL mutex → whole UI freezes for the
  scan; also disrupts an active SoftAP. Fix: run the scan on a worker task (like the
  airport fetch), update the page via the LVGL lock.

## 🟢 Low / latent / cosmetic

- **Manager task blocks ~2 s during the sync handoff** (2×`vTaskDelay(1000)` + `blesync_stop`,
  `netmgr.c:172/202`) while `post()` drops after 500 ms (`netmgr.c:90`) → WiFi/BLE events
  arriving in that window are dropped/delayed. Compounds the WLAN-serve disconnect handling.
  Fix: longer queue-post timeout, or don't block the manager task across the delay.

- **Non-`volatile` cross-task shared state.** `s_state`/`s_active`/`s_conn_handle`
  (`netmgr.c:38`, `blesync.c:52/54`) and 64-bit `s_last_client_ms`/`s_last_activity_ms`/
  viblog counters are read on a different task than they're written (32-bit ESP32 → torn
  64-bit reads possible). Not corrupting today; add `volatile` / atomics for correctness.

- **`apmode_start_session()` regenerates the password before the early-out.** `make_pass()`
  runs before `if (s_active) return true` (`apmode.c:65-69`) → a second call while active
  would show a password that doesn't match the live AP. Latent (netmgr only calls it from
  non-SOFTAP states); fix by moving the early-out first.

- **Recorded-byte counters undercount** by the 9-byte record framing per message and skip
  the header/schema/channel records (`viblog.c:119`, `uartrx.c:216`). Only feeds the
  already-dead `sd_free_mb` field (see cleanup) → an optimistic free-space estimate. Fix
  when/if free-space is actually displayed.

- **IMU Sensors page shows wrong scale during hi-rate logging** — reads assume ±2 g while
  the FIFO path put the part in ±16 g (`imu.c:151` vs the logger's config). Cosmetic (i2c
  serializes, no corruption).

- **`airport.c on_event` overflow leaves a mid-stream hole** (drops the offending chunk but
  keeps appending later ones, `airport.c:26`) → `cJSON_Parse` fails instead of using a valid
  prefix. Latent: the ~7 KB response never approaches `CAP=12288`.

## 🧹 Cleanup — dead code (safe deletes)

- `i2c_bus_sweep_pins()` (`i2c_bus.c:54`, decl `i2c_bus.h:18`) — bring-up tool, never called.
- ST7735 software-font path, dead now that LVGL renders all text: `st7735_draw_string`
  (`st7735.c:255`), `st7735_draw_char` (`:236`), `st7735_draw_pixel` (`:228`) — a closed
  chain — and therefore **all of `font5x7.h`** (only `st7735.c` includes it). Keep
  `fill_screen`/`fill_rect`/`backlight`/`set_brightness`.
- `st_rgb()` (`st7735.h:24`), `sd_unmount()` (`sdcard.c:78`), `uartrx_last_hex()`
  (`uartrx.c:83`) — unreferenced. (`uartrx_ring` stays: host-tested.)
- Unused getters: `imu_last_addr`/`imu_last_whoami` (`imu.c:50-51`), `radio_is_playing`
  (`radio.c:24`), `sd_is_mounted` (`sdcard.c:24`), `blesync_active` (`blesync.c:59`),
  `netmgr_internet_up` (`netmgr.c:58`), `filesrv_running` (`filesrv.c:45`).
- Dead per-sample work: the magnetometer read inside `imu_read()` (`imu.c:172`) — output
  fields never read; the hi-rate logger reads mag separately via `imu_hires_read_aux`.
- `viblog_status_t.sd_free_mb` + its `s_sd_free0`/`sd_free_bytes` snapshot plumbing
  (`viblog.h:22`, `viblog.c:52/286/347`) — computed, never displayed.

## 🧹 Cleanup — duplicate code

- **STA-IP → string ×3:** `netmgr.c:48` (`get_sta_ip`), `filesrv.c:132` (`h_info`),
  `app_main.c:99`. Extract one helper.
- **`Octanis-%02X%02X` name-from-MAC ×3:** `apmode.c:48`, `filesrv.c:67`, `blesync.c:330`
  (and the `t10-%02x%02x` hostname is the same math). One `device_ssid()` helper.
- **`next_path()` byte-identical ×2:** `uartrx.c:155` vs `viblog.c:219` (only the printf
  format differs). Shared `sd_next_path(prefix,out,n)`.
- **LVGL "button + centered label + width 100% + focus-stop + click cb" ×~6:** `page_button`
  / `make_button` + open-coded in `audio_clips_cb`, `vib_cb`, and 3× in `settings_cb`
  (`ui_menu.c`). One helper returning `(btn,label)`.
- basename-from-path (`strrchr(p,'/')`) ×2 in `ui_menu.c` (`:501`, `:765`).

## 🧹 Cleanup — magic numbers (highest-risk first)

- **HTTP port `8080` + SoftAP IP `192.168.4.1` hardcoded in `blesync.c:102`** handoff JSON,
  while `filesrv.c:28` defines `PORT 8080`. If `PORT` changes, the phone is told the wrong
  port. Pass the value in (e.g. `filesrv_port()`); name the IP `SOFTAP_GW_IP`.
- **IMU FIFO burst invariant `buf[60]` = `chunk(10)` × `stride(6)`** (`imu.c:250-261`), three
  independent literals. Name them; `buf[IMU_BURST_SAMPLES*IMU_ACCEL_SAMPLE_BYTES]`.
- **`[24]`-hour arrays duplicated across `airport.c`↔`ui_menu.c`** (producer bound vs consumer
  size, different files). `#define HOURS_PER_DAY 24` in a shared header.
- sha256 `32`/`64`/`65` and session-token `16`/`32`/`33` sizes written 3 ways each
  (`manifest.c`, `filesrv.c`). Name `SHA256_BYTES`/`SHA256_HEX_LEN`, `TOKEN_BYTES`/`TOKEN_HEX_LEN`.
- Duplicated `vTaskDelay(pdMS_TO_TICKS(1000))` handoff-drain (`netmgr.c:172/202`) →
  `BLE_NOTIFY_DRAIN_MS`. Per-task stack/priority literals at every `xTaskCreate`.

## 🧹 Cleanup — misleading docs

- **`i2c_bus.h:1` says `SDA=21, SCL=22` but the code uses `19/18`** (`i2c_bus.c:10`). Wrong
  but authoritative-looking pin doc — fix the header to match (or expose the `#define`s).
- Stale comments: `app_main.c:8` header (claims a backlight/dropdown the UI doesn't have),
  `filesrv.c:276` (`h_stop` "wired in later" — it's wired), `apmode.h:8` ("for now … shown
  on screen" — it's normal operation).
