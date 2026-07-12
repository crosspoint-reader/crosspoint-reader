# Dual-OS build workflow: CrossPoint (repo root) and Biscuit (biscuit/, vendored
# via git subtree) are two independent PlatformIO projects producing two app
# bins. The -ota0/-ota1 filename suffixes are labels only: both slots in
# partitions.csv are the same size, app images are slot-agnostic, and the
# on-device SD flasher always writes whichever slot is currently passive.
#
# A "does not fit" / region-overflow link error from either build means that
# app outgrew the shared 0x640000 slot. Reconcile the canonical partitions.csv
# (or accept the two firmwares can no longer co-reside) — never enlarge one
# slot asymmetrically, that breaks swapping.
#
# Usage, from anywhere:  make -C tools all|crosspoint|biscuit|clean

ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
DIST    := $(ROOT)/dist
CP_ENV  ?= gh_release
CP_VER  := $(shell python3 -c "import configparser; c = configparser.ConfigParser(); c.read('$(ROOT)/platformio.ini'); print(c['crosspoint']['version'])" 2>/dev/null || echo unknown)
GIT_REV := $(shell git -C $(ROOT) describe --tags --always --dirty 2>/dev/null || echo unknown)
# Biscuit ship env (its platformio.ini mirrors CrossPoint's env names).
# Override with B_ENV=default for a dev build.
B_ENV   ?= gh_release
B_VER   ?= $(GIT_REV)

.PHONY: all crosspoint biscuit clean

all: crosspoint biscuit

crosspoint:
	cd $(ROOT) && pio run -e $(CP_ENV)
	mkdir -p $(DIST)
	cp $(ROOT)/.pio/build/$(CP_ENV)/firmware.bin $(DIST)/crosspoint-$(CP_VER)-ota0.bin
	@echo "CrossPoint app image -> $(DIST)/crosspoint-$(CP_VER)-ota0.bin"

biscuit:
	@test -f $(ROOT)/biscuit/platformio.ini || \
		{ echo "biscuit/ is not vendored yet — see tools/sync-biscuit.sh"; exit 1; }
	cd $(ROOT)/biscuit && pio run $(if $(B_ENV),-e $(B_ENV),)
	mkdir -p $(DIST)
	@set -- $(ROOT)/biscuit/.pio/build/$(if $(B_ENV),$(B_ENV),*)/firmware.bin; \
	if [ $$# -ne 1 ] || [ ! -f "$$1" ]; then \
		echo "expected exactly one biscuit firmware.bin, found: $$*"; \
		echo "set B_ENV=<env> to pick the biscuit build environment"; \
		exit 1; \
	fi; \
	cp "$$1" $(DIST)/biscuit-$(B_VER)-ota1.bin; \
	echo "Biscuit app image   -> $(DIST)/biscuit-$(B_VER)-ota1.bin"

clean:
	-cd $(ROOT) && pio run -t clean
	-@test -f $(ROOT)/biscuit/platformio.ini && cd $(ROOT)/biscuit && pio run -t clean
	rm -rf $(DIST)
