# Hardware — LilyGO TTGO "TS" (T10) V2.0, 1.8" TFT

This firmware targets a **LilyGO TTGO TS** board (LilyGO's own repos call it "TS";
resellers list it as **"TTGO T10 V2.0 – 1.8 inch"**, e.g. tinytronics `LILYGO-I117`).
It is an **M5Stack-inspired** design (the vendor test code uses `M5.Lcd` /
`M5.Speaker` abstractions).

The pin map below was **reverse-engineered on the actual board and then confirmed
against LilyGO's canonical [`LilyGO/TTGO-TS`](https://github.com/LilyGO/TTGO-TS)
test code** — every pin matches. It is the source of truth for this project.

> ⚠️ **Do not trust `Xinyuan-LilyGO/LilyGo_Txx` → `T10_V20.h`** — that header (which
> this project was originally bootstrapped from) lists **buttons 36/37/39** and
> **I²C 21/22**, which are **wrong for this board**. The correct values are below.

## Board identity

| | |
|---|---|
| Product | LilyGO TTGO **TS** V2.0, 1.8" (aka "T10 V2.0 1.8\"") |
| MCU | ESP32-D0WD, rev 1.0, dual-core, **4 MB flash**, **no PSRAM** |
| USB-UART | **CP2104** (Silicon Labs → `/dev/cu.SLAB_USBtoUART` on macOS) |
| Power | TP4054 Li-ion charger + SY8089 regulator — **no IP5306** |

## Verified pin map (what this firmware uses)

| Function | GPIO | Notes |
|----------|------|-------|
| **TFT** ST7735, 128×160 | | GREENTAB2 offsets, BGR |
| TFT MOSI | 23 | |
| TFT SCLK | 5 | (strapping pin) |
| TFT CS | 16 | |
| TFT DC | 17 | |
| TFT RST | — | software reset (no pin) |
| TFT backlight | 27 | LEDC PWM |
| **Buttons** (active-low, input-only) | | external pull-ups; encoder map below |
| Button 1 = encoder ENTER | 35 | input-only, no internal pull |
| Button 2 = encoder UP | 34 | input-only, no internal pull |
| Button 3 = encoder DOWN | 39 | input-only, no internal pull |
| **I²C** (MPU9250 IMU) | | |
| I²C SDA | 19 | MPU9250 @ 0x68, AK8963 mag @ 0x0C |
| I²C SCL | 18 | |
| **Audio** | | |
| Speaker out | 25 | DAC1 → **NS4148 class-D amp** → LC filter → speaker |
| **microSD** (SPI3, vibration logging) | | own SPI bus, separate from the TFT |
| SD SCK / MOSI / MISO / CS | 14 / 15 / 2 / 13 | FAT mount at `/sdcard` |

## IMU / vibration logging notes
The MPU9250 is a **consumer 9-axis IMU**, not a lab accelerometer. For the
vibration logger (`viblog.c`) it runs at its ceiling: **4 kHz** output data rate
(accel DLPF bypassed, ~1 kHz usable bandwidth), **±16 g** (2048 LSB/g), gyro/mag
powered down, streamed through the on-chip **512-byte FIFO**. The FIFO is drained
over a dedicated **1 MHz** (fast-mode-plus) I²C device handle — needed for the
24 KB/s the accel produces; it falls back to 400 kHz (≈3 kHz reliable) if the
board's pull-ups can't sustain 1 MHz. There is **no MPU interrupt pin wired to the
ESP32** on this board, so sample timing uses the nominal 4 kHz ODR (±~3 %), not a
hardware data-ready line. Good for general machine/structure vibration and
dominant-frequency tracking; not for high-frequency bearing-defect analysis.

## Audio detail
GPIO25 (8-bit DAC) is **AC-coupled into an NS4148 class-D amplifier** (`U17` on the
schematic) whose output goes through an LC reconstruction filter to the speaker.
So the speaker is genuinely amplified — that's why an 8-bit DAC can drive it. The
amp gain is fixed (no volume pin wired), so loudness is capped by the 8-bit DAC.
The boot melody uses LEDC (square wave) on the same pin; clip/radio playback uses
the DAC (`gpio_reset_pin(25)` hands the pin from LEDC to the DAC).

## Free / usable GPIOs
With I²C on 18/19 (not 21/22), these are **free** on this board:

- **General purpose (output-capable):** GPIO **21, 22, 26, 32, 33, 4** — e.g. a spare
  UART could use TX/RX on **21/22** (both non-strapping, confirmed empty by the I²C
  sweep).
- **Input-only (RX-capable, no output/pull):** GPIO 36, 37, 38.
- **Avoid** (strapping): 0, 2, 5, 12, 15. **Unusable** (flash): 6–11. **Debug UART0:** 1, 3.

## Schematics
- **`docs/TTGO-TS_T10_V1.0_schematic.pdf`** — downloaded from
  [`LilyGO/TTGO-TS`](https://github.com/LilyGO/TTGO-TS); the matching audio/IMU/button
  design for this board.
- `../schematic/T10_v2.0.pdf` (in the parent repo) is internally labelled **V1.8** —
  its audio section (GPIO25 → NS4148) matches, but its I²C is drawn on 21/22 (an older
  revision), which is why it didn't match this unit.
- Related: [`LilyGO/TTGO-TS-V1.2`](https://github.com/LilyGO/TTGO-TS-V1.2).
