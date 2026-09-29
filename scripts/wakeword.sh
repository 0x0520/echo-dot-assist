#!/usr/bin/env bash
# Another wake word for an installed Echo: "Echo", "Computer", "Amazon", "Ziggy", or "Alexa" in another language.
# The models are Amazon's (DAVS) and the same for every Echo, so ones fetched before (device-logs/models/, git-ignored)
# are only copied over.  Each is first loaded by the Echo's own engine (pryon_test): an older engine (radar) cannot load
# every set.  A model not there yet is fetched from Amazon, which needs the Echo registered to an Amazon account once:
# it runs stock Alexa with the updaters cut off (MODE=stock-online) until then, and everything is undone afterwards
# (registration, the Wi-Fi the Alexa app added, the mode).  Home Assistant then offers every installed model in the
# Echo's wake word select.  Stopped halfway (Ctrl-C), a new run finds the Echo in stock-online mode and goes on there.
#   scripts/wakeword.sh [echo-ip]      without an address: the Echo on adb (USB, or ANDROID_SERIAL)
cd "$(dirname "$0")/.."
. scripts/lib/device.sh
. scripts/lib/setup.sh
. scripts/lib/wakeword.sh

case $1 in -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
[ -n "$1" ] && { export ANDROID_SERIAL=$1; [[ $1 == *:* ]] || ANDROID_SERIAL=$1:5555; }

trap '[ -n "$TTY" ] && printf "\e[?25h"' EXIT
trap '[ -n "$TASK_PID" ] && kill $TASK_PID 2>/dev/null; rm -rf "$TMP"; _clr; printf "\n  %sStopped. Run scripts/wakeword.sh again to go on.%s\n" "$DIM" "$N"; exit 130' INT TERM
LOG=build/wakeword.log; mkdir -p build; : > $LOG
MODEL_NAME="Wake word"
wakeword_run
