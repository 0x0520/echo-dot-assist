# Echo Dot 3rd gen 2018 (donut), Fire OS 6574.1: armv7, Android 7.1 bionic (API 24 NDK target), Amazon mixer + Pryon.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_mixer.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libmixerAPI.so libpryon.so libopus.so libz.so
