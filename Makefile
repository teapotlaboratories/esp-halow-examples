# esp-halow-examples — convenience wrapper around ESP-IDF's idf.py.
#
# Sources the ESP-IDF environment for each target so you never need to
# `. export.sh` by hand. Override any variable on the command line:
#
#   make build                                  # default example, default board
#   make build APP=mesh_gate
#   make flash APP=ibss PORT=/dev/ttyACM1
#   make flash-monitor APP=mesh
#   make list                                   # what can I build?
#
SHELL := /bin/bash

# ---- ESP-IDF ---------------------------------------------------------------
# ESP-IDF is vendored at vendor/esp-idf (submodule, pinned v5.4.2 — the floor the
# halow component requires). This Makefile sources its environment for each target
# so you never need to `. export.sh` by hand.
#
# Check it out with the other submodules, then install the toolchain from it:
#   git submodule update --init --recursive
#   make install-toolchain
#
# NOTE the ':=' rather than '?='. A makefile assignment overrides the ENVIRONMENT, so
# the vendored, version-pinned ESP-IDF wins even in a shell that has already sourced a
# different ESP-IDF's export.sh (which exports IDF_PATH). With '?=' that stale export
# would silently win and you would build against whichever ESP-IDF happened to be
# active — defeating the point of vendoring one, and failing quietly rather than loudly.
#
# A command-line override still takes precedence, because command-line variables beat
# makefile assignments:
#     make build IDF_PATH=/path/to/your/esp-idf
IDF_PATH  := $(CURDIR)/vendor/esp-idf

# Where the ESP-IDF TOOLCHAIN (compilers, gdb, openocd, and the Python venv) is kept.
# ESP-IDF defaults this to ~/.espressif, shared by every project on the machine. We pin
# it INSIDE this repo instead, so:
#   - this checkout is self-contained: delete the directory and nothing is left behind;
#   - the exact toolchain version this ESP-IDF asks for is used, not whatever another
#     project happened to install into the shared location;
#   - other ESP-IDF projects are untouched and keep using their own ~/.espressif.
#
# Costs roughly 1-2 GB for a single chip target (it is not shared with other projects,
# which is the entire point). It is gitignored. To go back to the machine-wide install:
#     make build IDF_TOOLS_PATH=$$HOME/.espressif
#
# ':=' for the same reason as IDF_PATH — a stale IDF_TOOLS_PATH exported by another
# project's export.sh must not silently redirect this repo's toolchain lookup.
IDF_TOOLS_PATH := $(CURDIR)/.espressif

PORT      ?= /dev/ttyACM0

# ---- Example selection -----------------------------------------------------
# Examples live under examples/<APP>/.
APP       ?= mesh
APP_DIR   := $(CURDIR)/examples/$(APP)

# ---- Board selection -------------------------------------------------------
# Board-specific sdkconfig defaults live in boards/<BOARD>/. The chip target is NOT
# set here — it comes from the board's sdkconfig.defaults (CONFIG_IDF_TARGET), so the
# board owns its target. Copy an existing board dir to add your own.
BOARD     ?= proto1-fgh100m
BOARD_DIR := $(CURDIR)/boards/$(BOARD)

# sdkconfig defaults applied to the build: the board overlay first, then the example's
# own sdkconfig.defaults layered on top so it can add example-level config (e.g.
# CONFIG_HALOW_AP_MODE). IDF auto-appends the matching .<target> variant of each file.
SDKCONFIG_DEFAULTS := $(BOARD_DIR)/sdkconfig.defaults$(if $(wildcard $(APP_DIR)/sdkconfig.defaults),;$(APP_DIR)/sdkconfig.defaults)

# All build output goes to <repo root>/build/<APP>/<BOARD>/, never inside the example
# dir. The generated sdkconfig lives there too, so each board keeps its own config and
# switching boards never reuses a stale one.
BUILD_DIR := $(CURDIR)/build/$(APP)/$(BOARD)

# ---- Preflight -------------------------------------------------------------
# Fail with an actionable message rather than a confusing CMake error.
define check-env
@[ -f "$(IDF_PATH)/export.sh" ] || { \
    echo "no ESP-IDF at IDF_PATH=$(IDF_PATH) (expected $(IDF_PATH)/export.sh)"; \
    echo "  ESP-IDF is vendored as a submodule — check it out, then install its toolchain:"; \
    echo "    git submodule update --init --recursive"; \
    echo "    make install-toolchain"; \
    exit 2; }
@[ -d "$(APP_DIR)" ] || { \
    echo "no such example: $(APP)"; \
    echo "available: $$(cd $(CURDIR)/examples && ls -d */ | tr -d / | tr '\n' ' ')"; \
    exit 2; }
@[ -d "$(BOARD_DIR)" ] || { \
    echo "no such board: $(BOARD)"; \
    echo "available: $$(cd $(CURDIR)/boards && ls -d */ | tr -d / | tr '\n' ' ')"; \
    exit 2; }
@[ -f "$(CURDIR)/components/halow/CMakeLists.txt" ] || { \
    echo "components/halow is empty — the submodules are not checked out."; \
    echo "  run: git submodule update --init --recursive"; \
    exit 2; }
@[ -d "$(CURDIR)/vendor/morse-firmware/firmware" ] || { \
    echo "vendor/morse-firmware is empty — the submodules are not checked out."; \
    echo "  run: git submodule update --init --recursive"; \
    exit 2; }
@# The component manager writes absolute paths into dependencies.lock, including the
@# path of the `firmware` component generated inside the build dir. Delete the build
@# dir (or build the same example with a different BUILD_DIR) and that lock goes stale,
@# and the next configure dies with an opaque
@#     ERROR: The "path" field in the manifest file ... does not point to a directory
@# which reads like a broken manifest rather than a stale cache. The lock is
@# regenerated on every configure and is gitignored, so if the build dir is gone,
@# drop it.
@[ -d "$(BUILD_DIR)" ] || rm -f "$(APP_DIR)/dependencies.lock"
endef

# Source the IDF environment, enter the example dir, then run idf.py with our
# out-of-source build directory and the selected board + example config defaults.
#
# The checkout supplies ESP-IDF's SOURCE; the compilers live out-of-tree under
# ~/.espressif and are registered per checkout path. If that registration is missing,
# export.sh fails with a message about some unrelated architecture's tools ("riscv32-
# esp-elf-gdb has no installed versions") that gives no hint what to do — so catch it
# here and say the actual fix.
IDF := export IDF_TOOLS_PATH="$(IDF_TOOLS_PATH)"; \
       { source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && command -v idf.py >/dev/null; } || { \
           echo "ESP-IDF at $(IDF_PATH) is not usable in this shell."; \
           echo ""; \
           echo "  The submodule provides ESP-IDF's source, not its compilers. Those live"; \
           echo "  in ~/.espressif and must be registered for THIS checkout once:"; \
           echo ""; \
           echo "      make install-toolchain"; \
           echo ""; \
           echo "  (Already have an ESP-IDF v5.4.2+ elsewhere? Skip it and use that:"; \
           echo "      make build IDF_PATH=/path/to/esp-idf )"; \
           exit 2; }; \
       cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)" \
              -D SDKCONFIG="$(BUILD_DIR)/sdkconfig" \
              -D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)"

.PHONY: help list install-toolchain build flash monitor flash-monitor size clean fullclean erase menuconfig

# One-time per checkout. Downloads the toolchain for the board's chip target if it is not
# already in ~/.espressif, and registers this checkout so export.sh stops demanding tools
# for architectures we never build. On a machine that already has the tools this is close
# to instant — it is mostly bookkeeping.
install-toolchain:
	@[ -f "$(IDF_PATH)/install.sh" ] || { \
	    echo "no ESP-IDF at $(IDF_PATH) — run: git submodule update --init --recursive"; \
	    exit 2; }
	@target=$$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$(BOARD_DIR)/sdkconfig.defaults" | head -1); \
	 target=$${target:-esp32s3}; \
	 echo "installing the ESP-IDF toolchain for $$target (board $(BOARD))"; \
	 echo "  into $(IDF_TOOLS_PATH)  (repo-local; your ~/.espressif is not touched)"; \
	 mkdir -p "$(IDF_TOOLS_PATH)"; \
	 IDF_TOOLS_PATH="$(IDF_TOOLS_PATH)" "$(IDF_PATH)/install.sh" "$$target"
	@# ESP-IDF does not bundle cmake/ninja on Linux, and idf.py refuses to run without
	@# them. They must go into THIS repo's python env, not the machine-wide one. cmake is
	@# pinned to 3.x because cmake 4 breaks ESP-IDF's build system.
	@pip=$$(echo "$(IDF_TOOLS_PATH)"/python_env/*/bin/pip); \
	 [ -x "$$pip" ] || { echo "no python env under $(IDF_TOOLS_PATH)/python_env"; exit 2; }; \
	 if "$$pip" show cmake >/dev/null 2>&1 && "$$pip" show ninja >/dev/null 2>&1; then \
	     echo "cmake + ninja already present in the repo-local python env"; \
	 else \
	     echo "installing cmake + ninja into the repo-local python env..."; \
	     "$$pip" install --quiet "cmake==3.30.5" ninja; \
	 fi
	@echo ""
	@echo "toolchain ready in $(IDF_TOOLS_PATH) — try: make build APP=$(APP)"

help:
	@echo "esp-halow-examples — ESP32-S3 + Morse Micro MM6108 (Wi-Fi HaLow)"
	@echo ""
	@echo "  make build         - build $(APP)"
	@echo "  make flash         - flash $(APP) to $(PORT)"
	@echo "  make monitor       - serial monitor on $(PORT)  (Ctrl-] to exit)"
	@echo "  make flash-monitor - flash then monitor"
	@echo "  make menuconfig    - configure $(APP)"
	@echo "  make size | clean | fullclean | erase"
	@echo "  make list          - list examples and boards"
	@echo ""
	@echo "Examples live under examples/<APP>/; boards under boards/<BOARD>/;"
	@echo "build output in build/<APP>/<BOARD>/.  Vars:"
	@echo "  APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)"
	@echo "  IDF_PATH=$(IDF_PATH)"
	@echo "  IDF_TOOLS_PATH=$(IDF_TOOLS_PATH)"
	@echo "     (repo-local toolchain — your ~/.espressif is not used or modified)"

list:
	@echo "examples:"
	@cd $(CURDIR)/examples && for d in */; do echo "  $${d%/}"; done
	@echo "boards:"
	@cd $(CURDIR)/boards && for d in */; do echo "  $${d%/}"; done

build:
	$(check-env)
	$(IDF) build

flash:
	$(check-env)
	$(IDF) -p $(PORT) flash

monitor:
	$(check-env)
	$(IDF) -p $(PORT) monitor

flash-monitor:
	$(check-env)
	$(IDF) -p $(PORT) flash monitor

size:
	$(check-env)
	$(IDF) size

menuconfig:
	$(check-env)
	$(IDF) menuconfig

clean:
	$(check-env)
	$(IDF) clean

fullclean:
	$(check-env)
	$(IDF) fullclean

erase:
	$(check-env)
	$(IDF) -p $(PORT) erase-flash
