#!/bin/sh
# Run the stock-state probe and compare the stock files that matter with the analysed firmware of this model.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
# a stock Echo has no adb, and TWRP has no /system mounted: without this every file came out DIFFERENT (issue #4)
[ "$(adb get-state 2>/dev/null)" = device ] ||
    die "no Echo running Fire OS on adb (state: $(adb get-state 2>&1 | tr -d '\r')); a stock Echo has no adb until it is rooted"
# What the files should be: devices/<codename>/probe.md5, from the pinned firmware (FIRMWARE_SHA256), so no unpacked
# firmware is needed here.  Re-pinned: (cd firmware/<codename>/rootfs/system && md5sum $PROBE_FILES) > devices/<codename>/probe.md5
[ -f $DDIR/probe.md5 ] || die "no $DDIR/probe.md5 to compare with"
mkdir -p device-logs
OUT=device-logs/probe-$(date +%Y%m%d-%H%M%S).txt
adb push scripts/device/probe.sh /data/local/tmp/ >/dev/null && adb shell sh /data/local/tmp/probe.sh $PROBE_FILES > "$OUT" &&
    grep -q "######## libs" "$OUT" || die "the probe did not run on the Echo"
echo "saved $OUT"
echo "model $DEVICE ($PRODUCT); firmware $(adb shell getprop ro.build.display.id | tr -d '\r'), expected $FIRMWARE_ID"
echo "file check against firmware $FIRMWARE_ID ($DDIR/probe.md5; no output below = identical):"
for f in $PROBE_FILES; do
    want=$(awk -v f="$f" '$2 == f { print $1 }' $DDIR/probe.md5)
    [ -n "$want" ] && grep -q "$want" "$OUT" || echo "  DIFFERENT: /system/$f"
done
