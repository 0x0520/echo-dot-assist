#!/bin/sh
# Run the stock-state probe and compare the stock files that matter with the analysed firmware of this model.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
# a stock Echo has no adb, and TWRP has no /system mounted: without this every file came out DIFFERENT (issue #4)
[ "$(adb get-state 2>/dev/null)" = device ] ||
    die "no Echo running Fire OS on adb (state: $(adb get-state 2>&1 | tr -d '\r')); a stock Echo has no adb until it is rooted"
[ -d $FW/rootfs/system ] || die "no $FW/rootfs/system to compare with: unpack the firmware first ($DDIR/README.md)"
mkdir -p device-logs
OUT=device-logs/probe-$(date +%Y%m%d-%H%M%S).txt
adb push scripts/device/probe.sh /data/local/tmp/ >/dev/null && adb shell sh /data/local/tmp/probe.sh $PROBE_FILES > "$OUT" &&
    grep -q "######## libs" "$OUT" || die "the probe did not run on the Echo"
echo "saved $OUT"
echo "model $DEVICE ($PRODUCT); firmware $(adb shell getprop ro.build.display.id | tr -d '\r'), expected $FIRMWARE_ID"
echo "file check against $FW/rootfs (no output below = identical):"
for f in $PROBE_FILES; do
    want=$(md5sum $FW/rootfs/system/$f | cut -d' ' -f1)
    grep -q "$want" "$OUT" || echo "  DIFFERENT: /system/$f"
done
