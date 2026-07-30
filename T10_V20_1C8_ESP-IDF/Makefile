# -----------------------------------------------------------------------------
# LilyGO TTGO T10 V2.0 (1.8" ST7735) - ESP-IDF build helper
#
#   make setup          one-time: install ESP-IDF (~2 GB, needs network)
#   make                build the firmware
#   make flash          build + flash over USB
#   make monitor        open the serial monitor (Ctrl-] to quit)
#   make flash-monitor  flash then monitor (the usual command)
#   make menuconfig      open the config UI
#   make erase          erase the whole flash
#   make clean          remove build artifacts
#   make port           show which serial port was auto-detected
#
# Override the port if auto-detect picks the wrong one:
#   make flash PORT=/dev/cu.usbserial-XXXX
# -----------------------------------------------------------------------------

SHELL := /bin/bash

# Where ESP-IDF lives (change if you installed it elsewhere).
IDF_PATH ?= $(HOME)/esp/esp-idf

# Auto-detect the board's serial port: CP210x first, then generic usbserial,
# then WCH (CH34x). Override on the command line with PORT=...
PORT ?= $(shell ls /dev/cu.SLAB_USBtoUART /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -n1)

# Source the IDF environment and run idf.py in a single shell invocation.
IDF = source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && idf.py

.PHONY: default all setup check venv boot-image audio sdkconfig-sync build flash monitor flash-monitor menuconfig erase clean fullclean size port test schema help

default: build

## --- toolchain -------------------------------------------------------------

setup:
	@bash tools/install-esp-idf.sh

# Fail early with a helpful message if ESP-IDF isn't installed yet.
check:
	@if [ ! -f "$(IDF_PATH)/export.sh" ]; then \
		echo "ESP-IDF not found at $(IDF_PATH)."; \
		echo "Run 'make setup' first (one-time, downloads the toolchain)."; \
		exit 1; \
	fi

## --- boot image ------------------------------------------------------------

# If a boot_image.png (128x160) is present, regenerate main/boot_image.h from it
# whenever it changes. The PNG is gitignored; the generated .h is committed as a
# default so the build works with no PNG present.
BOOT_PNG := boot_image.png
BOOT_HDR := main/boot_image.h
VENV     := .venv
VPY      := $(VENV)/bin/python

# Create a uv-managed venv with Pillow, only if it's missing. Requires `uv`.
venv:
	@if [ ! -x "$(VPY)" ]; then \
		command -v uv >/dev/null 2>&1 || { \
			echo "uv not found. Install it: https://docs.astral.sh/uv/"; exit 1; }; \
		echo "Creating uv venv ($(VENV)) + installing Pillow..."; \
		uv venv "$(VENV)"; \
		uv pip install --python "$(VPY)" pillow; \
	fi

# Ensure main/boot_image.h exists before building. It is gitignored (per-user
# content), so it is always generated here:
#   * boot_image.png present  -> convert it (via the uv venv + Pillow)
#   * otherwise               -> generate the built-in default splash (no deps)
boot-image:
	@if [ -f "$(BOOT_PNG)" ]; then \
		if [ "$(BOOT_PNG)" -nt "$(BOOT_HDR)" ] || [ ! -f "$(BOOT_HDR)" ]; then \
			$(MAKE) --no-print-directory venv; \
			echo "Converting $(BOOT_PNG) -> $(BOOT_HDR)"; \
			"$(VPY)" tools/img2c.py "$(BOOT_PNG)" -o "$(BOOT_HDR)"; \
		fi; \
	elif [ ! -f "$(BOOT_HDR)" ]; then \
		echo "No $(BOOT_PNG); generating default splash -> $(BOOT_HDR)"; \
		python3 tools/gen_default_boot.py -o "$(BOOT_HDR)"; \
	fi

## --- audio clips -----------------------------------------------------------

# Convert audio_clips/*.wav -> main/audio_clips.h (8-bit 16 kHz mono) on every
# build. The WAVs and the generated header are gitignored (per-user content).
AUDIO_DIR := audio_clips
AUDIO_HDR := main/audio_clips.h

audio:
	@mkdir -p "$(AUDIO_DIR)"
	@python3 tools/wav2c.py "$(AUDIO_DIR)" -o "$(AUDIO_HDR)"

## --- build / flash ---------------------------------------------------------

# ESP-IDF only applies sdkconfig.defaults when sdkconfig is absent. If the
# defaults changed, drop sdkconfig so the new values take effect (no manual
# `rm -f sdkconfig` dance).
sdkconfig-sync:
	@if [ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig ]; then \
		echo "sdkconfig.defaults changed -> regenerating sdkconfig"; \
		rm -f sdkconfig; \
	fi

build: check sdkconfig-sync boot-image audio
	@$(IDF) build

flash: check sdkconfig-sync boot-image audio port-check
	@$(IDF) -p "$(PORT)" flash

monitor: check port-check
	@$(IDF) -p "$(PORT)" monitor

flash-monitor: check sdkconfig-sync boot-image audio port-check
	@$(IDF) -p "$(PORT)" flash monitor

menuconfig: check
	@$(IDF) menuconfig

erase: check port-check
	@$(IDF) -p "$(PORT)" erase-flash

size: check
	@$(IDF) size

clean: check
	@$(IDF) clean

fullclean: check
	@$(IDF) fullclean

## --- tests -----------------------------------------------------------------

# Host-side unit + integration tests for the pure-C logic (MCAP writer, protobuf
# encoder). No board or ESP-IDF needed -- just a C compiler. See tools/run_tests.sh.
test:
	@bash tools/run_tests.sh

# Regenerate main/accel_schema.h from tools/accel.proto (needs protoc). The header
# is committed, so this is only needed when the .proto changes.
schema:
	@bash tools/gen_schema.sh

## --- helpers ---------------------------------------------------------------

port:
	@if [ -z "$(PORT)" ]; then \
		echo "No serial port detected. Plug the board in via USB, or pass PORT=..."; \
	else \
		echo "Using port: $(PORT)"; \
	fi

port-check:
	@if [ -z "$(PORT)" ]; then \
		echo "No serial port found. Plug in the board, or specify PORT=/dev/cu.xxxx"; \
		echo "Available ports:"; ls /dev/cu.* 2>/dev/null | sed 's/^/  /'; \
		exit 1; \
	fi

help:
	@sed -n '2,20p' Makefile | sed 's/^# \{0,1\}//'
