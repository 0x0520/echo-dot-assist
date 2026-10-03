#!/bin/sh
# Update an installed Echo over Wi-Fi: build, pack, sign with secrets/update.key, push.  No adb, no TWRP.
#   [DEVICE=<codename>] scripts/ota-push.sh [host]              host defaults to the one used last time (secrets/ota.host)
# The push installs the update beside the factory copy on the system partition and restarts hassmic with it.  If it
# does not come up, the Echo goes back to the factory copy by itself; once it has passed its self test (started, wake
# word engine loaded, a second of microphone audio), the Echo writes it over the factory copy, bootstrap included
# (boot.sh, otatool, hassmic.rc; scripts/system/sysinstall.sh): the new fallback.  Echos installed before this existed
# get there the same way: the pushed update brings the code that does the writing.
# No adb here, so the model comes from DEVICE (default donut).  The Echo refuses a bundle built for another model once it
# runs a main.sh that checks (devices/README.md).
# The Echo accepts the bundle only if it verifies against the public key that scripts/install-system.sh put on its
# read-only system partition.  Lose secrets/update.key and updates need one more trip through install-system.sh.
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
HOST=${1:-$(cat secrets/ota.host 2>/dev/null || true)}
[ -n "$HOST" ] || { echo "usage: scripts/ota-push.sh <echo-ip-or-hostname>"; exit 1; }
[ -f secrets/update.key ] || { echo "secrets/update.key missing: this key pair is created by scripts/install-system.sh"; exit 1; }
PORT=${OTA_PORT:-28929}

. scripts/lib/build.sh; build_binaries    # built here or this commit's release build (PREBUILT)
VERSION=$(build_version)                    # what hassmic will report: the commit's time and id, or the release's
scripts/bundle.sh secrets/update.key "$VERSION"
echo "pushing $DEVICE build to $HOST ..."
python3 scripts/otatool.py push "$HOST" $PORT $OUT/hassmic.bundle $OUT/hassmic.bundle.sig
mkdir -p secrets; echo "$HOST" > secrets/ota.host

echo
echo "$VERSION runs on $HOST now. Once it has passed its self test (seconds) it is also what the Echo falls back to."
