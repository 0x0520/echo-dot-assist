#!/bin/sh
# Packs and signs an update bundle from a build of one model: what scripts/ota-push.sh pushes and CI publishes.
#   [DEVICE=<codename>] scripts/bundle.sh KEY VERSION       -> build/<codename>/hassmic.bundle and hassmic.bundle.sig
# Needs that model's binaries in build/<codename>/ first: `make all`, or a release's (scripts/lib/build.sh).
# keys/release.pub rides along: the key of the project's releases, which the Echo then accepts for the updates hassmic
# downloads itself, once they are switched on in Home Assistant (scripts/system/main.sh).
set -e
cd "$(dirname "$0")/.."
PC_ANY=1 . scripts/lib/device.sh; device_load      # works on Windows too, with a release build
[ $# = 2 ] || die "usage: [DEVICE=<codename>] scripts/bundle.sh KEY VERSION"
# The Echo's shell keeps a CR as part of each word: one in the boot scripts and nothing starts after the next boot.
# .gitattributes keeps them out of a checkout; this catches a tree checked out before it, or an editor that put them back.
for f in scripts/system/*.sh scripts/device/*.sh $DDIR/device.conf $DDIR/hassmic.rc; do
    ! grep -q "$(printf '\r')" "$f" || die "$f has CR LF line endings: git add --renormalize . && git checkout -- ."
done
# what goes onto the Echo: scripts/lib/device.sh (ship_bins, ship_scripts); the module is data, not a program
python3 scripts/otatool.py pack "$1" "$2" $OUT/hassmic.bundle \
    $(ship_bins | sed 's/\.ko$/.ko:644/') $(ship_scripts) \
    $DDIR/device.conf:644 $DDIR/hassmic.rc:644 keys/release.pub:644
