# Echo Dot 3rd gen 2018 (donut), Fire OS 6574.1: armv7, Android 7.1 bionic (API 24 NDK target), Amazon mixer + Pryon.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_mixer.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libmixerAPI.so libpryon.so libopus.so libz.so

# Wi-Fi motion: the driver's own RX_STAT is the last frame from anyone on the channel; a kernel module of our own reads
# every frame from the access point (src/kmod/).  donut's kernel is 4.4.22 arm64 and carries no config of its own.
KMOD    := hassmic_rcpi4m
KVER    := 4.4.22
KARCH   := arm64
KCROSS  := $(CURDIR)/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-
KFRAG   := devices/donut/kconfig
KCFLAGS := -fno-pic                 # Android's GCC builds PIC by default; modules use -mcmodel=large, which refuses it
