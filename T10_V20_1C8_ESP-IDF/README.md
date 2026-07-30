# LilyGO TTGO T10 V2.0 — ESP-IDF test firmware (1.8" ST7735)

A self-contained, **Arduino-free** firmware for the LilyGO TTGO T10 V2.0 (ESP32) with the
1.8" ST7735 TFT. Pure ESP-IDF, hand-written display driver, driven entirely from the
terminal with `make`.

What it does once flashed:

- **Boot image** shown full-screen on the TFT, held until you press any button,
  with a **cute chiptune melody** played through the speaker on boot
- An **LVGL 9** GUI with a settings menu, driven by the 3 buttons as an encoder:
  - **BTN1** (GPIO35) = back / move up  (encoder rotate −)
  - **BTN2** (GPIO34) = forward / move down (encoder rotate +)
  - **BTN3** (GPIO39) = select / edit (encoder press)
  - Fields: **WiFi switch**, **backlight number** (spinbox, live PWM),
    **mode dropdown**, and actions (**WiFi scan**, **Sensors**, **Board info**,
    **Reboot**).

  > Note: this unit wires the buttons to GPIO **35 / 34 / 39** — the LilyGO repo's
  > Arduino header claims 36/37/39, which is wrong for this board revision.
- Uptime / free-heap heartbeat printed over USB serial

## Quick start

```bash
cd T10_V20_1C8_ESP-IDF

make setup          # one-time: downloads & installs ESP-IDF (~2 GB, few minutes)
make flash-monitor  # build, flash over USB, then open the serial monitor
```

Exit the serial monitor with **Ctrl-]**.

If flashing hangs at `Connecting......`, hold the **BOOT** button on the board while it
starts, then release.

## All make targets

| Command | What it does |
|---------|--------------|
| `make setup` | One-time ESP-IDF install (network + ~2 GB) |
| `make` / `make build` | Compile the firmware |
| `make flash` | Build + flash over USB |
| `make monitor` | Open the serial monitor |
| `make flash-monitor` | Flash then monitor (the usual command) |
| `make menuconfig` | ESP-IDF configuration UI |
| `make erase` | Erase the entire flash |
| `make clean` / `make fullclean` | Remove build artifacts |
| `make port` | Show the auto-detected serial port |

### Serial port

The Makefile auto-detects the port (`/dev/cu.SLAB_USBtoUART`, then `usbserial*`, then
`wchusbserial*`). Override it if needed:

```bash
make flash-monitor PORT=/dev/cu.usbserial-XXXX
```

Check `make port` to see what was detected. If nothing shows up, the board's USB-UART
driver may be missing — for CH34x boards install the driver from
<https://www.wch.cn/downloads/CH343SER_EXE.html> (CP210x/SLAB boards work out of the box
on recent macOS).

### ESP-IDF location

`make setup` installs ESP-IDF to `~/esp/esp-idf`. If you already have it elsewhere:

```bash
make flash-monitor IDF_PATH=/path/to/esp-idf
```

## Layout

```
T10_V20_1C8_ESP-IDF/
├── Makefile               # the make targets above
├── CMakeLists.txt         # top-level ESP-IDF project
├── sdkconfig.defaults     # esp32, 4MB flash, DIO, PSRAM off
├── tools/
│   ├── install-esp-idf.sh # what `make setup` runs
│   └── img2c.py           # convert an image -> main/boot_image.h
└── main/
    ├── app_main.c         # boot, menu loop, heartbeat
    ├── st7735.c/.h        # hand-written ST7735 SPI driver
    ├── font5x7.h          # built-in 5x7 ASCII font
    ├── boot_image.h       # 128x160 boot splash (RGB565, gitignored/generated)
    ├── sound.c/.h         # speaker melody (GPIO25, LEDC)
    ├── buttons.c/.h       # GPIO 35/34/39 (active-low)
    ├── lvgl_port.c/.h     # LVGL <-> ST7735 flush + 3-button encoder
    ├── ui_menu.c/.h       # LVGL settings screen (swap for EEZ output)
    ├── idf_component.yml  # pulls in lvgl/lvgl ^9
    ├── i2c_bus.c/.h       # shared I2C master bus
    ├── imu.c/.h           # MPU9250 accel/gyro/temp + AK8963 mag
    ├── wifi_scan.c/.h     # station-mode scan
    └── power.c/.h         # optional IP5306 keep-on (battery)
```

## Boot image

On boot the firmware draws a full-screen 128x160 image and waits for any button
press before continuing to the menu.

To use your own splash: **drop a `boot_image.png` (128x160) into this folder** and
run `make flash`. The build auto-converts it to `main/boot_image.h` (only when the
PNG changes). The PNG is gitignored; the generated header is committed as a
default, so the build still works with no PNG present.

```bash
pip install pillow                 # one-time, needed for the conversion
cp ~/my_splash.png boot_image.png  # 128x160
make flash                         # converts + flashes
```

Manual conversion (e.g. non-128x160 source, or letterbox instead of crop):

```bash
python3 tools/img2c.py my_photo.png --fit contain --rotate 90
```

## Boot melody

A short chiptune plays through the speaker (GPIO25) on boot, via the LEDC PWM
peripheral — see `main/sound.c`. Edit the `notes[]` / `durs[]` arrays in
`sound_play_boot_melody()` to change the tune.

> Want a real **WAV** file instead of tones? GPIO25 is a DAC pin, so PCM
> playback is possible (a `wav2c.py` + DAC player). It costs flash (~8 KB per
> second at 8 kHz) and needs a short source clip. Ask if you want it added.
> **MP3** is intentionally not supported — it needs a heavyweight decoder that
> isn't worth it for a boot chime.

## GUI / menu (LVGL 9)

The UI is built with **LVGL 9**, pulled in automatically as a managed component
(`main/idf_component.yml`) — the first `make build` downloads it. LVGL is
configured via Kconfig in `sdkconfig.defaults` (`CONFIG_LV_*`).

- `main/lvgl_port.c` wires LVGL to this board: the display flush goes through our
  ST7735 driver (`st7735_blit`, with RGB565 byte-swap), and the **3 buttons act
  as an LVGL encoder** input device (BTN1 = rotate back, BTN2 = rotate forward,
  BTN3 = press). All navigable widgets live in one input `group`
  (`lvgl_port_group()`).
- `main/ui_menu.c` is the hand-coded settings screen (switch / spinbox / dropdown
  / action buttons). It's the reference for how widgets are created and added to
  the encoder group.

### Editing the UI visually with EEZ Studio

[EEZ Studio](https://www.envox.eu/studio/) is a free, open-source drag-and-drop
editor that exports LVGL 9 C code (the SquareLine Studio alternative).

1. Install EEZ Studio, create a **new LVGL project**, LVGL version **9.x**,
   display **128 × 160**.
2. In project settings, set the input to an **encoder / keypad group** (so the
   generated screens are navigable without a touchscreen).
3. Design your screens (labels, switches, spinboxes, dropdowns, sliders…).
4. **Build/Export** — point the output at `main/ui/` in this project.
5. Add the generated sources to the build: either list them in
   `main/CMakeLists.txt` `SRCS`, or (simpler) add `main/ui` as its own component.
6. In `app_main`, replace `ui_menu_start()` with the generated `ui_init()`, then
   add the exported screen's widgets to `lvgl_port_group()` (EEZ can emit the
   group-add calls if you assigned widgets to a group in the editor).

The `lvgl_port` layer (display + encoder + tick + task) stays the same no matter
how the screens are authored — EEZ just replaces `ui_menu.c`.

## Tuning the display

The ST7735 has several factory "tab" variants with slightly different pixel offsets and
color order. This firmware ships with the values matching the repo's Arduino
`User_Setups/T10_V20_1C8.h` (GREENTAB2). If on first flash the image looks **shifted**,
**mirrored**, or the **colors are swapped** (red shows as blue), tweak these `#define`s at
the top of `main/st7735.c`:

```c
#define ST7735_COLSTART 2      // horizontal offset (try 0)
#define ST7735_ROWSTART 1      // vertical offset (try 0)
#define ST7735_MADCTL   0xC8   // orientation + color order; clear 0x08 -> 0xC0 for RGB
```

Then `make flash`. Common fixes:

- Colors inverted (red↔blue): change `0xC8` → `0xC0` (or vice-versa).
- Image shifted a few px: adjust `COLSTART` / `ROWSTART`.
- Whole image mirrored/upside-down: flip the `0x40` (MX) / `0x80` (MY) bits in `MADCTL`.

## Pin reference (T10 V2.0)

| Signal | GPIO | | Signal | GPIO |
|--------|------|-|--------|------|
| TFT MOSI | 23 | | TFT backlight | 27 |
| TFT SCLK | 5  | | Button 1 | 35 |
| TFT CS   | 16 | | Button 2 | 34 |
| TFT DC   | 17 | | Button 3 | 39 |
| TFT RST  | — (software) | | IP5306 I2C | SDA 21 / SCL 22 |
