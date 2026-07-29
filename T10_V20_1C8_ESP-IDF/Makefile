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

.PHONY: default all setup check build flash monitor flash-monitor menuconfig erase clean fullclean size port help

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

## --- build / flash ---------------------------------------------------------

build: check
	@$(IDF) build

flash: check port-check
	@$(IDF) -p "$(PORT)" flash

monitor: check port-check
	@$(IDF) -p "$(PORT)" monitor

flash-monitor: check port-check
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
