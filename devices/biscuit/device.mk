# Echo Dot 2nd gen 2016 (biscuit), Fire OS 6574.1: armv7, Android 7.1 bionic (API 24 NDK target), Amazon mixer + Pryon.
# libmixerAPI.so, libpryon.so, libAmazonKWD.so and libopus.so are byte-identical to donut's NS65741 build; only the
# SoC side differs (libasp, the mixer daemon build).  Same OS generation, same 32-bit userspace.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_mixer.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libmixerAPI.so libpryon.so libopus.so
