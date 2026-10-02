#!/bin/sh
# Update an installed Echo over Wi-Fi: build, pack, sign with secrets/update.key, push.  No adb, no TWRP.
#   [DEVICE=<codename>] scripts/ota-push.sh [host]              host defaults to the one used last time (secrets/ota.host)
#   [DEVICE=<codename>] scripts/ota-push.sh --approve [host]    approve the version pushed there last, later
# Two stages.  The push installs the update beside the factory copy on the system partition; if it does not stay up,
# the Echo goes back to that copy by itself.  Then you try it, and once you approve, the Echo writes it over the factory
# copy, bootstrap included (boot.sh, otatool, hassmic.rc; scripts/system/sysinstall.sh): the new fallback.  Echos
# installed before this existed get there the same way: the pushed update brings the code that does the writing.
# No adb here, so the model comes from DEVICE (default donut).  The Echo refuses a bundle built for another model once it
# runs a main.sh that checks (devices/README.md).
# The Echo accepts the bundle only if it verifies against the public key that scripts/install-system.sh put on its
# read-only system partition.  Lose secrets/update.key and updates need one more trip through install-system.sh.
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
APPROVE=; [ "$1" = --approve ] && { APPROVE=1; shift; }
HOST=${1:-$(cat secrets/ota.host 2>/dev/null || true)}
[ -n "$HOST" ] || { echo "usage: scripts/ota-push.sh [--approve] <echo-ip-or-hostname>"; exit 1; }
[ -f secrets/update.key ] || { echo "secrets/update.key missing: this key pair is created by scripts/install-system.sh"; exit 1; }
PORT=${OTA_PORT:-28929}
PUSHED=$OUT/pushed-$HOST                    # the version this PC pushed there last: what --approve approves

approve() {
    echo "making $1 the factory copy of $HOST ..."
    build/otatool-host factory "$HOST" $PORT secrets/update.key "$1"
}

if [ -n "$APPROVE" ]; then
    [ -f "$PUSHED" ] || { echo "nothing pushed to $HOST from here for $DEVICE (DEVICE=...?)"; exit 1; }
    make -s build/otatool-host
    approve "$(cat "$PUSHED")"; exit
fi

make -s all build/otatool-host DEVICE=$DEVICE
VERSION=$(make -s version)                  # the commit's time and id: what hassmic will report
scripts/bundle.sh secrets/update.key "$VERSION"
echo "pushing $DEVICE build to $HOST ..."
build/otatool-host push "$HOST" $PORT $OUT/hassmic.bundle $OUT/hassmic.bundle.sig
mkdir -p secrets; echo "$HOST" > secrets/ota.host; echo "$VERSION" > "$PUSHED"

echo
echo "$VERSION runs on $HOST now, as an update. Try it: the wake word, a command, and whatever this version changes."
echo "If it does not stay up, the Echo goes back to its factory copy by itself."
if [ -t 0 ]; then
    printf "Once it works, make it the factory copy (what the Echo falls back to)? [y/N] "
    read -r ans
    case "$ans" in [yYjJ]*) approve "$VERSION"; exit;; esac
fi
echo "Not the factory copy yet. When you are happy with it: scripts/ota-push.sh --approve $HOST"
