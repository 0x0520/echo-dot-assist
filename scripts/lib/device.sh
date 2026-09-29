# Sourced by the PC scripts, from the repository root: which Echo model, and its facts (devices/$DEVICE/device.conf).
#   DEVICE=<codename>   pick the model by hand; the default is donut, or with device_load adb the model of the Echo on adb
# After device_load: DEVICE, DDIR (devices/$DEVICE), OUT (build/$DEVICE, as the Makefile), FW (firmware/$DEVICE), plus
# everything device.conf sets.  With two Echos on adb, ANDROID_SERIAL picks one, as for adb itself.

die() { echo "$*" >&2; exit 1; }

# codename whose device.conf names this product (ro.product.device)
device_for_product() {
    for c in devices/*/device.conf; do
        [ "$(. "$c"; echo "$PRODUCT")" = "$1" ] && { c=${c%/device.conf}; echo "${c##*/}"; return 0; }
    done
    return 1
}

# device_load [adb]: with "adb", ask the connected Echo which model it is, and refuse when DEVICE says otherwise.
# In recovery the properties are TWRP's own, so there DEVICE (or the default) is taken as given.
device_load() {
    if [ "$1" = adb ] && [ "$(adb get-state 2>/dev/null)" = device ]; then
        prod=$(adb shell getprop ro.product.device | tr -d '\r')
        [ -n "$prod" ] || die "no Echo on adb (with several: export ANDROID_SERIAL=<serial from adb devices>)"
        found=$(device_for_product "$prod") ||
            die "the Echo on adb reports product '$prod', which no devices/*/device.conf names: not supported (devices/README.md)"
        [ -z "$DEVICE" ] || [ "$DEVICE" = "$found" ] || die "DEVICE=$DEVICE, but the Echo on adb is a $found ($prod)"
        DEVICE=$found
    fi
    DEVICE=${DEVICE:-donut}
    DDIR=devices/$DEVICE
    [ -f $DDIR/device.conf ] || die "unknown DEVICE '$DEVICE': no $DDIR/device.conf"
    . $DDIR/device.conf
    OUT=build/$DEVICE
    FW=firmware/$DEVICE
    export DEVICE
}

# The binaries link against one firmware's libraries; on another one they may crash or misbehave in the audio path.
# device_check_firmware [warn]: stop (or only warn) when the Echo on adb runs a different firmware.
device_check_firmware() {
    have=$(adb shell getprop ro.build.display.id | tr -d '\r')
    [ "$have" = "$FIRMWARE_ID" ] && return 0
    [ "$1" = warn ] && { echo "warning: this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID" >&2; return 0; }
    die "this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID: reflash that one ($DDIR/README.md)"
}
