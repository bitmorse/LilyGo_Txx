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
