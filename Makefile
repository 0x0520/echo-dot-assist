# Cross-build for one Echo model, linking against that model's stock libraries.
#   make [DEVICE=donut]      device binaries into build/$(DEVICE)/, against firmware/$(DEVICE)/rootfs
# Everything model-specific comes from devices/$(DEVICE)/ (device.mk here, board.c in the daemon); see devices/README.md.
DEVICE  ?= donut
ifeq ($(wildcard devices/$(DEVICE)/device.mk),)
$(error unknown DEVICE "$(DEVICE)": no devices/$(DEVICE)/device.mk)
endif
include devices/$(DEVICE)/device.mk

NDK     ?= $(CURDIR)/toolchain/android-ndk-r21e
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
STOCK   := $(CURDIR)/firmware/$(DEVICE)/rootfs/system/lib
OUT     := build/$(DEVICE)
BOARD   := devices/$(DEVICE)/board.c
BUILD   := $(shell git describe --always --dirty 2>/dev/null || echo nogit)
CFLAGS  := -O2 -Wall -Wextra -fPIE -Isrc/include -DBUILD='"$(BUILD)"'
LDFLAGS := -pie -fuse-ld=lld -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs
STOCK_LIBS = $(addprefix $(STOCK)/,$(filter $(LIBS),$(1)))

# The build id is compiled in; make must notice when it changes (a new commit), not only when sources change.
# Same for the model the PC builds stand in for (they sit in build/, not build/$(DEVICE)/).
build/.build-id: FORCE
	@mkdir -p build; echo '$(BUILD)' | cmp -s - $@ || echo '$(BUILD)' > $@
build/.device: FORCE
	@mkdir -p build; echo '$(DEVICE)' | cmp -s - $@ || echo '$(DEVICE)' > $@
FORCE:

# A model without the Amazon mixer or Pryon has no use for the tools built on them: they drop out with the library.
BIN := $(OUT)/hassmic $(OUT)/runas $(OUT)/otatool \
       $(if $(filter libmixerAPI.so,$(LIBS)),$(OUT)/mixcap $(OUT)/mixplay $(OUT)/latency) \
       $(if $(filter libpryon.so,$(LIBS)),$(OUT)/pryon_test)

.DEFAULT_GOAL := all
all: $(BIN)

$(STOCK)/%.so:
	@echo "missing $@: unpack the $(DEVICE) firmware first (devices/$(DEVICE)/README.md)"; exit 1

$(OUT)/mixcap $(OUT)/mixplay: $(OUT)/%: src/tools/%.c src/include/mixer_api.h src/include/netio.h $(STOCK)/libmixerAPI.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libmixerAPI.so

$(OUT)/latency: src/tools/latency.c src/include/mixer_api.h $(STOCK)/libmixerAPI.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(STOCK)/libmixerAPI.so

# update bundles: the same source verifies + unpacks on the Echo and packs + signs on the PC
$(OUT)/otatool: src/tools/otatool.c src/third_party/monocypher.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $^ -o $@ -pie -fuse-ld=lld

build/otatool-host: src/tools/otatool.c src/third_party/monocypher.c
	@mkdir -p build
	cc -O2 -Wall $^ -o $@

$(OUT)/runas: src/tools/runas.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": raw HCI probe on the Bluetooth controller (stop btmanagerd first, see the file)
$(OUT)/hciscan: src/tools/hciscan.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": LD_PRELOAD shim to see a stock daemon's libcurl requests (see the file)
$(OUT)/libcurlspy.so: src/tools/curlspy.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -fuse-ld=lld -ldl

$(OUT)/pryon_test: src/tools/pryon_test.c src/include/pryon_api.h $(STOCK)/libpryon.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libpryon.so

HASSMIC := src/hassmic/main.c src/hassmic/wyoming.c src/hassmic/proto_wyoming.c src/hassmic/proto_esphome.c src/hassmic/buttons.c \
           src/hassmic/sendspin.c src/hassmic/arb.c src/hassmic/ble.c src/hassmic/ble_crypto.c src/hassmic/a2dp.c src/hassmic/a2dp_codecs.c src/hassmic/sbc.c src/hassmic/ota.c src/hassmic/ws.c src/hassmic/net.c src/hassmic/noise.c src/hassmic/hash.c src/hassmic/sounds.c src/hassmic/micgain.c \
           src/third_party/monocypher.c src/third_party/freeaptx.c
HASSMIC_H := $(wildcard src/hassmic/*.h src/include/*.h) build/.build-id

$(OUT)/hassmic: $(HASSMIC) $(BOARD) $(AUDIO) $(WAKE) $(HASSMIC_H) $(addprefix $(STOCK)/,$(LIBS))
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm -ldl $(addprefix $(STOCK)/,$(LIBS))

# PC build for protocol tests: file audio backend, no wake word (SIGUSR1 triggers), identity of $(DEVICE).
build/hassmic-host: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c src/hassmic/wake_none.c $(HASSMIC_H) build/.device
	@mkdir -p build
	cc -O2 -Wall -Wextra -DBUILD='"$(BUILD)"' -Isrc/include -Isrc/hassmic $(filter %.c,$^) -o $@ -lpthread -lm -ldl -lopus

# ARM build with file audio but the device's wake word engine, for running under qemu-arm (tools/qrun.sh).
$(OUT)/hassmic-qemu: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c $(WAKE) $(HASSMIC_H) $(call STOCK_LIBS,libpryon.so libopus.so)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm -ldl $(call STOCK_LIBS,libpryon.so libopus.so)

host: build/hassmic-host $(OUT)/hassmic-qemu build/otatool-host

# Building blocks of the Sendspin client, checked against reference implementations (aiohttp, python noiseprotocol).
UNIT := src/hassmic/hash.c src/hassmic/ws.c src/hassmic/net.c src/hassmic/noise.c src/third_party/monocypher.c
unit:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/hash_test.c $(UNIT) -lpthread -o build/hash_test && build/hash_test
	cc -O2 -Wall -D_GNU_SOURCE -Isrc/hassmic -Isrc/include -include stdlib.h tests/unit/ws_test.c $(UNIT) -lpthread -o build/ws_test
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/noise_test.c $(UNIT) -lpthread -o build/noise_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/ble_crypto_test.c src/hassmic/ble_crypto.c -o build/ble_crypto_test && build/ble_crypto_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/micgain_test.c src/hassmic/micgain.c -lm -o build/micgain_test && build/micgain_test
	.venv/bin/python tests/unit/ws_ref.py build/ws_test
	.venv/bin/python tests/unit/noise_ref.py build/noise_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/a2dp_codecs_test.c src/hassmic/a2dp_codecs.c src/hassmic/sbc.c src/third_party/freeaptx.c -lm -ldl -lopus -o build/a2dp_codecs_test && build/a2dp_codecs_test
	if command -v sbcenc >/dev/null; then tests/unit/sbc_ref.sh; else echo "sbc: sbcenc/sbcdec (package sbc) missing, skipped"; fi

clean:
	rm -rf build

.PHONY: all host unit clean FORCE
