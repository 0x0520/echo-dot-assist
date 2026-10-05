# Sourced by the PC scripts, from the repository root: which Echo model, and its facts (devices/$DEVICE/device.conf).
#   DEVICE=<codename>   pick the model by hand; the default is donut, or with device_load adb the model of the Echo on adb
# After device_load: DEVICE, DDIR (devices/$DEVICE), OUT (build/$DEVICE, as the Makefile), FW (firmware/$DEVICE), plus
# everything device.conf sets.  With two Echos on adb, ANDROID_SERIAL picks one, as for adb itself.

die() { echo "$*" >&2; exit 1; }

# Git Bash, MSYS2 and Cygwin on Windows run these scripts only part of the way: the Makefile wants Linux compilers,
# and stat -c, nc -q, .venv/bin and the setup's package managers are not there.  So stop before anything is done.  Works
# there: pushing a release build (ota-push.sh and bundle.sh, which set PC_ANY=1; Python's otatool, no compiler) and
# adb-wifi.sh, which does not load this.
on_windows() { case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) return 0;; esac; return 1; }
WSL_HINT="run it in WSL2 (Ubuntu: wsl --install), from a clone made there; for adb over USB attach the Echo with usbipd-win (README.md, Requirements)"
[ -n "$PC_ANY" ] || ! on_windows || die "${0##*/} needs Linux, this is $(uname -s): $WSL_HINT"

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

# ship_bins: the Echo's binaries in $OUT that go onto it, one per line: what bundle.sh packs, install-system.sh and
# deploy.sh push, and CI hands from its builds to the release job.  The tools on the mixer and Pryon only where the model
# has those (Makefile BIN), Wi-Fi motion's module only where it was built.  pryon_test goes along because
# scripts/artifacts.sh loads every wake word set with the Echo's own engine before installing it.  aed_test and
# whisper_test stay on the PC: research tools (docs/re-aed.md, docs/re-whisper.md) that nothing on the Echo runs, pushed
# by hand to /data/local/tmp when needed.
ship_bins() {
    echo $OUT/hassmic; echo $OUT/runas; echo $OUT/otatool
    for _f in $OUT/latency $OUT/mixcap $OUT/mixplay $OUT/pryon_test $OUT/*.ko; do [ -f "$_f" ] && echo "$_f"; done
    return 0
}
# ship_scripts: what runs as root on the Echo next to them (scripts/system: boot integration; scripts/device: firewall,
# Alexa off and on, joining Wi-Fi).  device.conf, the keys and hassmic.rc are added by each caller, which treats them differently.
ship_scripts() {
    echo scripts/system/main.sh scripts/system/boot.sh scripts/system/sysinstall.sh \
         scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/wifi-join.sh
}

# The binaries link against one firmware's libraries; on another one they may crash or misbehave in the audio path.
# device_check_firmware [warn]: stop (or only warn) when the Echo on adb runs a different firmware.
device_check_firmware() {
    have=$(adb shell getprop ro.build.display.id | tr -d '\r')
    [ "$have" = "$FIRMWARE_ID" ] && return 0
    [ "$1" = warn ] && { echo "warning: this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID" >&2; return 0; }
    die "this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID: reflash that one ($DDIR/README.md)"
}
