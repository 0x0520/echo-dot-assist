#!/bin/sh
# Run the stock-state probe and compare the stock files that matter with the analysed firmware of this model.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
mkdir -p device-logs
OUT=device-logs/probe-$(date +%Y%m%d-%H%M%S).txt
adb push scripts/device/probe.sh /data/local/tmp/ >/dev/null && adb shell sh /data/local/tmp/probe.sh $PROBE_FILES > "$OUT"
echo "saved $OUT"
echo "model $DEVICE ($PRODUCT); firmware $(adb shell getprop ro.build.display.id | tr -d '\r'), expected $FIRMWARE_ID"
echo "file check against $FW/rootfs (no output below = identical):"
for f in $PROBE_FILES; do
    want=$(md5sum $FW/rootfs/system/$f | cut -d' ' -f1)
    grep -q "$want" "$OUT" || echo "  DIFFERENT: /system/$f"
done
