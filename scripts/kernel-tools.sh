#!/bin/sh
# Downloads what Wi-Fi motion's kernel module is built with (src/kmod/): kernel.org's sources of the Echo's kernel and
# the compiler Amazon built it with, into toolchain/.  Run by `make kernel-tools` (the pins are in device.mk or the
# Makefile's defaults), which the guided setup and CI call; without these `make` leaves the module out (issue #5).
#   scripts/kernel-tools.sh KSRC KCROSS KERNEL_URL KERNEL_SHA256 KCC_URL KCC_DIGEST
# Does nothing when both are there already.  With KERNEL_TOOLS_CHECK=1 in the environment it only says whether they are
# (exit status), for the guided setup.
set -e
[ $# = 6 ] || { echo "usage: make kernel-tools [DEVICE=<codename>]" >&2; exit 1; }
KSRC=$1 KCROSS=$2 KURL=$3 KSUM=$4 CURL=$5 CSUM=$6
KCC=${KCROSS%/bin/*}                                # toolchain/<compiler>, from its .../bin/<prefix>
[ -f "$KSRC/Makefile" ] && [ -x "${KCROSS}gcc" ] && exit 0
[ -z "$KERNEL_TOOLS_CHECK" ] || exit 1
# KSRC or KCROSS set to something of the system's own: not ours to download into (or to clear, below)
case $KSRC:$KCC in */toolchain/*:*/toolchain/*) ;; *) echo "$KSRC, $KCC: not in toolchain/, not downloaded" >&2; exit 1;; esac
TMP=${KSRC%/*}/.kernel-tools.$$
# unpacked beside where they go and moved there whole: an interrupted run leaves nothing that looks complete.  Two runs
# at once (act's jobs share toolchain/): the first one done is kept, the other's copy dropped, never one in use.
trap 'rm -rf "$TMP" "$KSRC.part.$$" "$KCC.part.$$"' EXIT
place() { if [ -e "$3" ]; then rm -rf "$1"; else rm -rf "$2"; mv "$1" "$2"; fi; }

# googlesource cuts or throttles a download now and then (2026-10-02: two of three CI jobs, tar on a broken stream): into
# a file, retried, and checked before anything is unpacked
fetch() {
    for i in 1 2 3 4 5; do
        code=$(curl -sSL --retry 3 --retry-all-errors -o "$TMP" -w '%{http_code}' "$1") && [ "$code" = 200 ] && $2 && return 0
        echo "$1: try $i failed (HTTP ${code:-none}, $(stat -c %s "$TMP" 2>/dev/null || echo 0) bytes)" >&2; sleep $((i * 15))
    done
    echo "$1: no complete download" >&2; return 1
}
kernel_ok() { echo "$KSUM  $TMP" | sha256sum -c --quiet -; }
gzip_ok() { gzip -t "$TMP"; }

if [ ! -f "$KSRC/Makefile" ]; then
    echo "kernel sources: $KURL"
    fetch "$KURL" kernel_ok
    mkdir -p "$KSRC.part.$$"
    tar xJf "$TMP" -C "$KSRC.part.$$" --strip-components=1
    place "$KSRC.part.$$" "$KSRC" "$KSRC/Makefile"
fi
if [ ! -x "${KCROSS}gcc" ]; then
    echo "compiler: $CURL"
    fetch "$CURL" gzip_ok
    mkdir -p "$KCC.part.$$"
    tar xzf "$TMP" -C "$KCC.part.$$"
    # googlesource makes the archive anew each time (the gzip differs), so the pin is on what it holds: every file
    digest=$(cd "$KCC.part.$$" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
    [ "$digest" = "$CSUM" ] || { echo "$KCC: files differ from the pinned ones ($digest)" >&2; exit 1; }
    # aarch64-linux-android-4.9's gcc and g++ are Python 2 wrappers (/usr/bin/python) that only exec real-<name> (and
    # goma): go direct
    for w in "$KCC.part.$$"/bin/real-*; do if [ -e "$w" ]; then ln -sf "${w##*/}" "$KCC.part.$$/bin/${w##*/real-}"; fi; done
    place "$KCC.part.$$" "$KCC" "${KCROSS}gcc"
fi
