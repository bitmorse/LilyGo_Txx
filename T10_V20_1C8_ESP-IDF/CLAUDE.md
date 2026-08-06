# Engineering rules for this firmware

Pure-ESP-IDF firmware for the LilyGO TTGO TS / T10 V2.0. This file is the
contract for **how** code is written here. Read it before changing code.

## Build & test

```bash
make build          # compile firmware
make test           # host unit + integration tests (no board needed)
make flash-monitor  # flash + serial monitor
make schema         # regenerate main/accel_schema.h from tools/accel.proto (needs protoc)
```

`make test` must pass before every commit. It builds the pure-C logic with
`-Wall -Wextra -Werror` and runs the unit tests plus an integration test that
validates a real MCAP file against the official `mcap` + protobuf libraries.

## Workflow: red → green → refactor (TDD)

Write the test **first**. Every change to non-trivial logic follows this loop:

1. **Red** — write a failing test that pins down the behavior you want, and run
   it to watch it fail (`cc ... test/... && ./a.out`, or `make test`). A test
   that has never been red proves nothing.
2. **Green** — write the *minimum* code to make it pass. Run the suite.
3. **Refactor** — clean up with the tests as your safety net; keep them green.

Tests live in `test/` and are plain host C (see `test/test_framework.h`). Any
logic that can be pulled out of the FreeRTOS/ESP-IDF world **should** be, so it
can be tested on the host — e.g. `main/accel_encode.c` (protobuf) and
`main/mcap.c` (container format) are deliberately free of ESP dependencies and
have full unit coverage. Hardware-bound code (drivers, tasks) is verified on the
board and kept thin so the untestable surface stays small.

New/changed pure logic without a test is not done.

## NASA/JPL "Power of 10" rules

We follow the [Power of 10](https://spinroot.com/gerard/pdf/P10.pdf) safety
rules, adapted to an ESP-IDF app. They are ordered; when one is deviated from,
say so at the call site and here.

1. **Simple control flow.** No recursion. No `goto` **except** a single forward
   `goto fail;` for cleanup in a function with multiple failure points (this is
   the one accepted exception — it beats nested `if`s or duplicated teardown).
   Example: `viblog_start()`.
2. **Bounded loops.** Every loop has a statically provable upper bound, or is a
   top-level FreeRTOS task loop (`while (s_run)`) that is explicitly a
   non-terminating service loop. No unbounded `for(;;)` buried in logic.
3. **No heap after init.** Prefer static/stack allocation. Allocations that do
   happen are made **once at start** of an activity (never in the steady-state
   hot path) and are paired with a free on every exit path — e.g. `viblog`
   allocates its batch buffers in `viblog_start()` and frees them on the
   writer-task exit and on the `fail:` path. The 4 kHz sampling/writing loop
   allocates nothing.
4. **Short functions.** One thing per function, ≈60 lines max. If it grows,
   split it.
5. **Assertions.** At least ~2 assertions per non-trivial function, checking
   preconditions and invariants (`assert(w && w->f)`, buffer-fits checks, index
   bounds). Assertions guard **programming errors**; they never replace runtime
   error handling of external failures (a missing SD card returns an error and
   sets a status string — it does not `assert`).
6. **Smallest scope.** Declare variables at first use, in the tightest scope.
   File-static state is `static` and documented.
7. **Check returns & params.** Check the return of every function that can fail
   and validate parameters. Bounds-checked encoders return 0 / false rather than
   overrun (`accel_encode_batch` returns 0 if the buffer is too small).
8. **Limited preprocessor.** `#include` and simple `#define` constants/macros
   only. No token-pasting tricks, no conditional-compilation mazes.
9. **Restrict pointers.** At most one level of dereference in a statement; no
   pointer arithmetic beyond a clear cursor over a buffer.
10. **Zero warnings.** Host tests build with `-Wall -Wextra -Werror`. Firmware
    builds clean; treat new warnings as errors.

### Accepted, documented deviations (platform-inherent)

These are unavoidable in an ESP-IDF + LVGL app and are allowed **only** where noted:

- **Function pointers (rule 9/1):** ESP-IDF event handlers and LVGL widget
  callbacks are function-pointer based. Allowed for framework glue only; app
  logic stays direct-call.
- **Heap via the SDK (rule 3):** WiFi/BLE/LVGL/FATFS allocate internally; we
  don't control that. *Our* code keeps to rule 3 as described above.
- **Unchecked config writes (rule 7):** register-setup writes in `imu.c` follow
  the file's existing pattern of not checking each `i2c` write return; a dead
  bus is caught by the subsequent read. New I/O in the *data path* is checked.

## Current audit (vibration logger)

Modules added for vibration logging — `imu.c` (hi-rate FIFO), `sdcard.c`,
`mcap.c`, `accel_encode.c`, `viblog.c` — were audited against the above:

- `mcap.c`, `accel_encode.c`: host-tested (92 checks), bounds-checked, asserted,
  `-Werror` clean. `accel_encode_batch` computes the exact length and refuses to
  write past the caller's capacity.
- `viblog.c`: allocations only in `viblog_start`; hot loop allocation-free;
  counters are single-writer (no locks needed); drops are counted, never silent.
  Uses the one accepted `goto fail`.
- Known hardware risks (I²C 1 MHz margin, no MPU INT pin, consumer-grade IMU
  bandwidth) are documented in `docs/HARDWARE.md`, not hidden.

## Connectivity vocabulary & rules (§)

Canonical definitions so we stop confusing terms. Cite the code, e.g. "§3.2", in
commits/comments/PRs when the behaviour is load-bearing. When the words below and the
code disagree, the code is a bug — fix the code, not the meaning.

### §1 The RAM constraint (why any of this exists)
- **§1.1** ESP32-D0WD, **no PSRAM**, ~320 KB RAM, tight heap.
- **§1.2** *(governing rule)* **BLE and the HTTP file server cannot run at the same
  time.** NimBLE (~40–48 KB) + the file server (httpd + mDNS + sha256 precache + FATFS/SD)
  leaves ~0 free → LWIP can't get TX buffers → transfers stall. Measured min-free 44–136 B.
- **§1.3** BLE **and Wi-Fi alone DO coexist** (~50 KB free with STA connected; SoftAP up
  is tighter but fine for a brief handoff). The incompatibility in §1.2 is BLE **+ the
  file server**, *not* BLE + Wi-Fi. Say it precisely: we offload BLE **while serving
  files**, not "while using Wi-Fi".

### §2 Transports (nouns — use these exact words)
- **§2.1 BLE** — the **default, resting transport** and control channel. A file sync can
  only be *started* over BLE (§3.3).
- **§2.2 Hotspot** — the device's own SoftAP. **Ephemeral: it exists only for the duration
  of one file-sync transfer**, then closes. Never a resting state. ("Hotspot mode" = the
  device *would* use its hotspot for the next sync.)
- **§2.3 External Wi-Fi** — the device joins a home/office network to serve files over the
  LAN. **Also ephemeral per the agreed model**: joined only for the transfer (see §5 note).
- **§2.4 File server** — the httpd instance (`filesrv.c`). The RAM-heavy thing from §1.2.
  "Serving / transfer in progress" = the file server is running.

### §3 BLE availability (the rule I keep getting wrong)
- **§3.1** BLE is up in **every resting state** (`sync-idle`, and while merely connected to
  Wi-Fi). It is **NOT "always available".**
- **§3.2** BLE is **torn down (`blesync_stop`) only for the duration of an active file
  transfer** — i.e. in the serving states `softap` / `wlan-serve` — and **restored
  (`blesync_start`) when the transfer ends**. This is the *only* time BLE is unavailable.
- **§3.3** **BLE is required to start a sync.** The phone writes the Control characteristic
  over BLE; netmgr then opens the hotspot / joins external Wi-Fi, hands off, and *then*
  drops BLE per §3.2.

### §4 Default & mode policy
- **§4.1** Default is **BLE**. The device **never auto-joins external Wi-Fi**, even with
  stored creds (`settings_wlan_mode()` defaults false).
- **§4.2** **External Wi-Fi is opt-in**, per the "Ext WiFi only" setting; greyed until
  creds exist.
- **§4.3** Adding Wi-Fi **verifies then returns to BLE** — storing creds is not a request
  to switch transports.
- **§4.4** Forgetting Wi-Fi resets the preference to the BLE default (§4.1).

### §5 States (`netmgr.c` — the source of truth for behaviour)
`boot → sync-idle → (softap ⇄ back) ` and, when external Wi-Fi is used,
`sync-idle → verifying/sta → wlan-serve → back`.
- **§5.1 sync-idle** — resting; BLE up; no file server. The default (§4.1).
- **§5.2 softap** — a hotspot transfer is in progress; BLE **down** (§3.2).
- **§5.3 wlan-serve** — an external-Wi-Fi transfer is in progress; BLE **down** (§3.2).
- **§5.4 verifying** — briefly joined to test creds, then back to §5.1 (§4.3).
- **§5.5 sta-connecting / sta-connected** — **transient only**: entered on demand (a sync
  join, or a feature Wi-Fi hold), never a resting state. Returns to §5.1 when the use ends.
> Implementation note: external Wi-Fi sync is **ephemeral** (join → serve → leave). The
> device never rests on Wi-Fi. Internet features get Wi-Fi on demand via a ref-counted
> **hold**: `netmgr_wifi_hold()` / `netmgr_wifi_release()` (ZRH Traffic holds for the
> fetch, Radio for the stream); the last release drops Wi-Fi back to BLE (§2.3). BLE
> stays up throughout the hold (§1.3). A sync is refused while the radio is playing
> (both are heap-heavy, §1.1).

### §6 Words to avoid
- ❌ "BLE is always on/available" → ✅ "BLE is up except during an active transfer (§3.2)".
- ❌ "we can't run BLE and Wi-Fi together" → ✅ "we can't run BLE and the **file server**
  together (§1.2)".
- ❌ "Hotspot mode" as a resting state → ✅ hotspot is ephemeral, sync-only (§2.2).
