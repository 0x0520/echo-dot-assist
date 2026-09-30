# Echo 2nd gen 2017 (radar), Fire OS 6572: armv7, Android 7.1 bionic (API 24 NDK target), Amazon mixer + Pryon.
# libmixerAPI.so has the same 334 exports as donut's (a rebuild, byte-different, ABI-identical).  libpryon.so is the
# slimmer engine generation (9.9 MB vs donut's 19.8 MB, as on crumpet); every symbol wake_pryon.c needs is there.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_mixer.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libmixerAPI.so libpryon.so libopus.so libz.so
