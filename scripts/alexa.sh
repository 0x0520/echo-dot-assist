#!/bin/sh
# Stock Alexa on or off on the Echo on adb, without a reboot (scripts/device/alexa-on.sh and alexa-off.sh on the Echo).
#   scripts/alexa.sh on     stock Alexa with internet, firmware updates still blocked; hassmic off.  Until "off" or the
#                           next reboot, which always brings the satellite back
#   scripts/alexa.sh off    Alexa off, egress lock and satellite back
# Needs only adb, so it runs on Windows too.
cd "$(dirname "$0")/.."
PC_ANY=1 . scripts/lib/device.sh; device_load adb
case "$1" in on|off) ;; *) die "usage: scripts/alexa.sh on|off";; esac
[ "$(adb get-state 2>/dev/null)" = device ] || die "no Echo running Fire OS on adb"
B=/data/local/hassmic
# The copy init runs (boot.sh): the installed update unless it failed to start three times, else the factory copy;
# on an Echo with nothing installed, what scripts/deploy.sh pushed.
adb shell "d=/system/hassmic; t=\$(cat $B/ota/tries 2>/dev/null); [ -f $B/ota/current/main.sh ] && [ \"\${t:-0}\" -lt 3 ] && d=$B/ota/current
           [ -f \$d/alexa-on.sh ] || d=$B; [ -f \$d/alexa-on.sh ] || { echo 'no alexa-on.sh on the Echo'; exit 1; }
           sh \$d/alexa-$1.sh" | tr -d '\r' || exit 1
# Without hassmic there is no switch in Home Assistant and no scripts/adb-wifi.sh: adb over Wi-Fi closes when its window
# ends, unless hassmic.conf has ADB_WIFI=1.
if [ "$1" = on ]; then
    case "$(adb get-serialno 2>/dev/null)" in *:*)
        echo "note: adb over Wi-Fi stays open only for the rest of its window (or with ADB_WIFI=1). The way back after that:"
        echo "      USB, or reboot the Echo (Alexa on lasts until the next reboot).";;
    esac
fi
