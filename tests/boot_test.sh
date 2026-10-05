#!/bin/sh
# The root side of the boot logic on the PC: scripts/system/main.sh itself (not a copy of its commands, as
# ota_push_test.sh has) in temp directories, with the Echo's otatool (build/otatool-host) and stand-ins for Android's
# getprop/start/stop/pidof/chown.  Covers what decides whether an Echo keeps a working, locked-down boot: which bundles
# ota_watch installs, where root writes in the daemon's directory, when the start counter of an update is reset, a
# config that does not load, and the Wi-Fi requests of Wi-Fi setup over Bluetooth (wifi_watch, wifi-join.sh).  Run
# with the Echo's shell where there is one (mksh), else sh.
#   tests/boot_test.sh            needs build/otatool-host (make build/otatool-host) and python3
cd "$(dirname "$0")/.."
SH=${SH:-$(command -v mksh || echo sh)}
T=$(mktemp -d); O="python3 scripts/otatool.py"; fail=0; MAIN=
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
cleanup() { [ -n "$MAIN" ] && kill $MAIN 2>/dev/null; kill $HM 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
waitfor() { i=0; while [ $i -lt ${2:-40} ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }
[ -x build/otatool-host ] || { echo "build/otatool-host missing: make build/otatool-host"; exit 1; }

# Android's tools: properties, init services, process lookup; chown to the daemon's user needs root
mkdir -p $T/bin
printf '#!/bin/sh\n[ "$1" = ro.product.device ] && echo testdev\n' > $T/bin/getprop
printf '#!/bin/sh\necho "start $*" >> %s/svc\n' $T > $T/bin/start
printf '#!/bin/sh\necho "stop $*" >> %s/svc\n' $T > $T/bin/stop
printf '#!/bin/sh\ncat %s/pidof 2>/dev/null\n' $T > $T/bin/pidof
printf '#!/bin/sh\nexit 0\n' > $T/bin/chown
chmod 755 $T/bin/*
export PATH=$T/bin:$PATH

# System partition (factory copy) and the copy main.sh runs from
SYS=$T/sys D=$T/run BASE=$T/data
mkdir -p $SYS $D $BASE/state/ota $BASE/ota
cp build/otatool-host $SYS/otatool
$O keygen $T/k.sec $SYS/update.pub; $O keygen $T/evil.sec $T/evil.pub
echo 1.0 > $SYS/VERSION
printf '#!/bin/sh\necho "boot $*" > %s/booted\n' $T > $SYS/boot.sh
printf 'PRODUCT=testdev\nDAEMON_USER=nobody\n' > $D/device.conf
printf '#!/bin/sh\necho "$*" > %s/lockdown\nexit 0\n' $T > $D/lockdown.sh
cp scripts/system/main.sh $D/main.sh
echo 'NAME="Test"' > $BASE/hassmic.conf
run() { HASSMIC_DIR=$D HASSMIC_SYS=$SYS HASSMIC_BASE=$BASE $SH $D/main.sh "$@" & MAIN=$!; }
stop_main() { kill $MAIN 2>/dev/null; wait $MAIN 2>/dev/null; MAIN=; }

# What a bundle carries: main.sh, device.conf, a hassmic that is a real program (its /proc/<pid>/exe is what root
# compares), runas that passes the self-check through, and sysinstall.sh standing in for writing the system partition.
B=$T/b; mkdir $B
printf '#!/system/bin/sh\necho main\n' > $B/main.sh
printf 'PRODUCT=testdev\nDAEMON_USER=nobody\n' > $B/device.conf
cp "$(command -v sleep)" $B/hassmic
printf '#!/bin/sh\nshift 2; [ "$2" = -T ] && exit 0; exec "$@"\n' > $B/runas
printf '#!/bin/sh\necho "factory $*" > %s/factory\n' $T > $B/sysinstall.sh
pack() { v=$1; shift; $O pack $T/k.sec $v $BASE/state/ota/bundle "$@" > /dev/null; }
request() { rm -f $BASE/state/ota/result; touch $BASE/state/ota/request; waitfor "[ -f $BASE/state/ota/result ]" 60; }

# 1. A good bundle, with the daemon's traps laid: result.tmp a link to a file root would create and hand over, and a
# test binary from deploy.sh that would go on running instead of the update.
pack 2.0 $B/main.sh $B/device.conf:644 $B/hassmic $B/runas $B/sysinstall.sh
ln -s $T/victim $BASE/state/ota/result.tmp
cp $B/hassmic $BASE/hassmic
run firewall; request
r=$(cat $BASE/state/ota/result 2>/dev/null)
[ "$r" = "OK 2.0" ]; ok $? "signed bundle installed: $r"
[ ! -e $T/victim ] && [ ! -L $BASE/state/ota/result ]; ok $? "root writes the result in its own directory, not through the daemon's link"
[ "$(cat $BASE/ota/current/VERSION)" = 2.0 ] && [ "$(cat $BASE/ota/tries)" = 0 ]; ok $? "made current, start counter 0"
[ ! -e $BASE/hassmic ]; ok $? "the test binary in /data no longer masks the update"
waitfor "[ -f $T/booted ]"; grep -q "stop hassmic" $T/svc && grep -q "boot firewall" $T/booted; ok $? "satellite restarted, firewall script run anew"
wait $MAIN 2>/dev/null; MAIN=
cur=$(readlink $BASE/ota/current)

# 2. Bundles whose scripts would not run on the Echo are not installed
run firewall
printf '#!/system/bin/sh\r\nD=/system/hassmic\r\n' > $T/main.sh
pack 3.0 $T/main.sh $B/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.0: main.sh has CR LF"; ok $? "CR LF main.sh refused: $r"
printf 'PRODUCT=testdev\r\n' > $T/device.conf
pack 3.1 $B/main.sh $T/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.1: device.conf has CR LF"; ok $? "CR LF device.conf refused: $r"
printf 'if then fi (\n' > $T/main.sh
pack 3.2 $T/main.sh $B/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.2: main.sh does not parse"; ok $? "main.sh that does not parse refused: $r"
$O pack $T/evil.sec 6.6.6 $BASE/state/ota/bundle $B/main.sh $B/device.conf:644 > /dev/null; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED the signature"; ok $? "bundle from another key refused: $r"
[ "$(readlink $BASE/ota/current)" = "$cur" ]; ok $? "current still the good one"
# the daemon made state/ota/result a directory: rm -f leaves it, and mv would drop the result inside it
rm -f $BASE/state/ota/result; mkdir -p $BASE/state/ota/result/sub; request 2> /dev/null
[ -f $BASE/state/ota/result ] && ! [ -L $BASE/state/ota/result ] && grep -q "^FAILED the signature" $BASE/state/ota/result; ok $? "a directory in place of the result is replaced, not written into"

# 3. The start counter: reset by a passed self test of the update's own hassmic, not by the factory copy's
echo 2 > $BASE/ota/tries
cp "$(command -v sleep)" $T/factory-hassmic; $T/factory-hassmic 60 & HM=$!; echo $HM > $T/pidof
touch $BASE/state/ota/healthy; waitfor "[ ! -f $BASE/state/ota/healthy ]"; sleep 0.5
[ "$(cat $BASE/ota/tries)" = 2 ] && [ ! -f $T/factory ]; ok $? "self test of the factory copy: counter kept, nothing written"
kill $HM; wait $HM 2>/dev/null
$cur/hassmic 60 & HM=$!; echo $HM > $T/pidof
touch $BASE/state/ota/healthy; waitfor "[ -f $T/factory ]"
[ "$(cat $BASE/ota/tries)" = 0 ] && grep -q "factory $cur" $T/factory; ok $? "self test of the update: counter 0, it becomes the factory copy"
kill $HM; wait $HM 2>/dev/null; HM=
stop_main

# 4. The config: CR LF converted; one that does not load still gets the firewall up
printf 'NAME="Test"\r\n' > $BASE/hassmic.conf; rm -f $T/lockdown
run firewall; waitfor "[ -f $T/lockdown ]"
! grep -q "$(printf '\r')" $BASE/hassmic.conf && grep -q "CR LF line endings, converted" $BASE/boot.log; ok $? "CR LF config converted"
stop_main
printf 'NAME="Te\n' > $BASE/hassmic.conf; rm -f $T/lockdown
# the watcher of the version before, as a push update leaves it running (matched by command line, read with mksh's read -d)
sh -c 'sleep 30' lockdown.sh watch & W=$!
run firewall; waitfor "[ -f $T/lockdown ]"; sleep 0.3
[ "$(cat $T/lockdown)" = watch ] && grep -q "hassmic.conf does not load (shell syntax): no satellite" $BASE/boot.log; ok $? "config that does not load: firewall watcher runs regardless"
case "$SH" in *mksh) ! kill -0 $W 2>/dev/null; ok $? "... and only one: the previous watcher is stopped first";; esac
kill $W 2>/dev/null; wait $W 2>/dev/null
stop_main
echo 2 > $BASE/ota/tries
run satellite; waitfor "! kill -0 $MAIN 2>/dev/null"; kill $MAIN 2>/dev/null; wait $MAIN; rc=$?; MAIN=
[ $rc = 1 ] && [ "$(cat $BASE/ota/tries)" = 0 ]; ok $? "... and no satellite (rc $rc), not counted against the update"
printf 'NAME="Test"\nexit 0\n' > $BASE/hassmic.conf; rm -f $T/lockdown
run firewall; waitfor "[ -f $T/lockdown ]"; sleep 0.3
[ "$(cat $T/lockdown)" = watch ] && [ "$(grep -c "does not load" $BASE/boot.log)" = 3 ]; ok $? "an exit in the config does not end the firewall service"
stop_main

# 5. No going back with the release key: every bundle CI ever published verifies against it.  The owner's key may.
echo 'NAME="Test"' > $BASE/hassmic.conf
$O keygen $T/rel.sec $D/release.pub
echo 2026.10.05.120000 > $SYS/VERSION
files="$B/main.sh $B/device.conf:644 $B/hassmic $B/runas"
run firewall
$O pack $T/rel.sec 2026.10.01.000000 $BASE/state/ota/bundle $files > /dev/null; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 2026.10.01.000000 is older than 2026.10.05.120000"; ok $? "older release-signed bundle refused: $r"
echo 2026.10.07.080000+abc1234-dirty > $D/VERSION
$O pack $T/rel.sec 2026.10.06.000000-beta $BASE/state/ota/bundle $files > /dev/null; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "is older than 2026.10.07.080000+abc1234-dirty"; ok $? "... nor older than the copy that runs, a build of one's own: $r"
rm $D/VERSION
pack 2026.10.01.000000 $files; request
r=$(cat $BASE/state/ota/result); [ "$r" = "OK 2026.10.01.000000" ]; ok $? "older bundle signed with the update key installed: $r"
wait $MAIN 2>/dev/null; MAIN=
run firewall
$O pack $T/rel.sec 2026.10.06.000000 $BASE/state/ota/bundle $files > /dev/null; request
r=$(cat $BASE/state/ota/result); [ "$r" = "OK 2026.10.06.000000" ]; ok $? "newer release-signed bundle installed: $r"
wait $MAIN 2>/dev/null; MAIN=
echo 2026.10.06.000000 > $SYS/VERSION
run firewall
$O pack $T/rel.sec 2026.10.06.000000-beta $BASE/state/ota/bundle $files > /dev/null; request
r=$(cat $BASE/state/ota/result); [ "$r" = "OK 2026.10.06.000000-beta" ]; ok $? "the same version again is not older: $r"
wait $MAIN 2>/dev/null; MAIN=

# 6. Wi-Fi setup over Bluetooth: what hassmic leaves in state/wifi-request is data for wpa_cli, never shell.  First
# with a stand-in wifi-join.sh that records what it got, then the real one against a stand-in wpa_cli.
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > %s/wj.args; cp "$2" %s/wj.file; [ -f %s/wj.fail ] && { echo "not joined: SCANNING after 30s"; exit 1; }; echo "joined, address 10.0.0.7"\n' $T $T $T > $D/wifi-join.sh
hexof() { printf %s "$1" | od -An -tx1 | tr -d ' \n'; }
WR=$BASE/state/wifi-result
wifi_req() { rm -f $WR $T/wj.args $T/wj.file; printf '%s\n%s\n' "$1" "$2" > $BASE/state/wifi-request.tmp; chmod 600 $BASE/state/wifi-request.tmp
             mv $BASE/state/wifi-request.tmp $BASE/state/wifi-request; waitfor "[ -f $WR ]" 40; }
run firewall
nl='
'
# what a shell would run lands in the working directory (cwd) or in $T: neither may get any of it; an SSID is 32 bytes
evil_ssid="\"\$(touch p1)\`touch p2\`\"$nl'"
evil_psk="p\"\$(touch p4)'; touch p5 \\"
pwned() { for f in p1 p2 p4 p5 p6; do [ -e $f ] || [ -e $T/$f ] && return 0; done; return 1; }
wifi_req "$(hexof "$evil_ssid")" "$evil_psk"
r=$(cat $WR 2>/dev/null)
[ "$r" = "OK joined, address 10.0.0.7" ] && [ "$(sed -n 1p $T/wj.args)" = -x ]; ok $? "valid request applied through wifi-join.sh -x: $r"
[ "$(sed -n 1p $T/wj.file)" = "$(hexof "$evil_ssid")" ] && [ "$(sed -n 2p $T/wj.file)" = "$evil_psk" ]; ok $? "... SSID (quotes, \$(), a newline) and passphrase handed over as they were"
! pwned; ok $? "... and nothing in them executed"
[ ! -e $BASE/state/wifi-request ] && [ ! -e $BASE/wifi-request.taken ] && ! grep -q "touch p4" $BASE/boot.log; ok $? "request gone, passphrase not in the log"
wifi_req "\$(touch p6)" "password1"
r=$(cat $WR 2>/dev/null); [ "$r" = "FAILED request refused: SSID not in hex" ] && [ ! -e $T/wj.args ] && ! pwned; ok $? "raw SSID refused, nothing run: $r"
wifi_req "41" "short"
r=$(cat $WR 2>/dev/null); [ "$r" = "FAILED request refused: passphrase length" ] && [ ! -e $T/wj.args ]; ok $? "passphrase of 5 characters refused: $r"
wifi_req "41" "$(printf 'pass\tword1')"
r=$(cat $WR 2>/dev/null); [ "$r" = "FAILED request refused: passphrase not printable ASCII" ]; ok $? "control character in the passphrase refused: $r"
wifi_req "41" ""
r=$(cat $WR 2>/dev/null); [ "${r%% *}" = OK ] && [ "$(sed -n 2p $T/wj.file)" = "" ]; ok $? "open network (no passphrase) passed on: $r"
rm -f $WR $T/wj.args; echo secret > $T/secret; ln -s $T/secret $BASE/state/wifi-request; waitfor "[ -f $WR ]" 40
r=$(cat $WR 2>/dev/null); [ "$r" = "FAILED request refused: not a plain file" ] && [ "$(cat $T/secret)" = secret ] && [ ! -e $T/wj.args ]; ok $? "request that is a link refused, its target untouched: $r"
touch $T/wj.fail; wifi_req "41" "password1"
r=$(cat $WR 2>/dev/null); [ "$r" = "FAILED not joined: SCANNING after 30s" ]; ok $? "failed join reported: $r"
rm -f $T/wj.fail
stop_main

# the real wifi-join.sh, against wpa_cli, iptables and ifconfig stand-ins (each wpa_cli argument on a line of its own)
printf '#!/bin/sh\necho "-N hassmic_out"\n' > $T/bin/iptables
printf '#!/bin/sh\necho "          inet addr:10.0.0.9  Bcast:10.0.0.255"\n' > $T/bin/ifconfig
printf '#!/bin/sh\nshift 4; { printf "%%s|" "$@"; echo; } >> %s/wpa.log\ncase "$1" in add_network) echo 3;; status) [ -f %s/wpa.down ] && echo wpa_state=SCANNING || echo wpa_state=COMPLETED;; *) echo OK;; esac\n' $T $T > $T/bin/wpa_cli
chmod 755 $T/bin/*
printf '%s\n%s\n' "$(hexof "$evil_ssid")" "$evil_psk" > $T/req
rm -f $T/wpa.log; out=$($SH scripts/device/wifi-join.sh -x $T/req); rc=$?
[ $rc = 0 ] && [ "$out" = "joined, address 10.0.0.9" ]; ok $? "wifi-join.sh: joined ($out)"
grep -qxF "set_network|3|ssid|$(hexof "$evil_ssid")|" $T/wpa.log && grep -qxF "set_network|3|psk|\"$evil_psk\"|" $T/wpa.log && grep -qxF "save_config|" $T/wpa.log
ok $? "... SSID in hex and the quoted passphrase reach wpa_cli as one argument each, saved"
! pwned; ok $? "... nothing executed"
touch $T/wpa.down; rm -f $T/wpa.log; out=$(WIFI_JOIN_SECS=2 $SH scripts/device/wifi-join.sh -x $T/req); rc=$?
[ $rc = 1 ] && grep -qxF "remove_network|3|" $T/wpa.log && grep -qxF "enable_network|all|" $T/wpa.log; ok $? "wifi-join.sh: not joined, profile removed, the others enabled again ($out)"
printf '%s\n%s\n' 'x' 'password1' > $T/req2
rm -f $T/wpa.log; out=$($SH scripts/device/wifi-join.sh -x $T/req2); rc=$?
[ $rc = 1 ] && [ ! -e $T/wpa.log ]; ok $? "wifi-join.sh -x: an SSID that is not hex goes nowhere ($out)"
# 7. The satellite's mDNS service file: hassmic -S runs as the daemon's user (runas), and root writes the file in its own
# directory and renames it into the daemon's, so a link the daemon left there is replaced, not written through.
# The address wait wants an interface with a real MAC (/sys/class/net); without one it would take 120 s.
WLAN=; for i in /sys/class/net/*; do grep -q '[1-9a-f]' $i/address 2>/dev/null && { WLAN=${i##*/}; break; }; done
if [ -z "$WLAN" ] || ! command -v setsid > /dev/null; then echo "skip mDNS service file: no interface with a MAC, or no setsid"
else
    printf 'PRODUCT=testdev\nDAEMON_USER=nobody\nDAEMON_GROUPS=nogroup\nWLAN=%s\n' $WLAN > $D/device.conf
    printf '#!/bin/sh\necho "$*" >> %s/runas.log\n[ "$1" = -r ] && shift 2\nshift 2; exec "$@"\n' $T > $D/runas
    printf '#!/bin/sh\ncase "$*" in *-S*) echo "  <name>echo-test</name>"; exit 0;; esac\nexec sleep 30\n' > $D/hassmic
    printf '#!/bin/sh\nexit 0\n' > $D/alexa-off.sh
    for t in ifconfig pkill avahi-daemon; do printf '#!/bin/sh\nexit 0\n' > $T/bin/$t; done
    chmod 755 $D/runas $D/hassmic $T/bin/*
    export HASSMIC_AVAHI=$T/avahi
    mkdir -p $T/avahi/services; echo keep > $T/victim2; ln -s $T/victim2 $T/avahi/services/hassmic.service
    HASSMIC_DIR=$D HASSMIC_SYS=$SYS HASSMIC_BASE=$BASE setsid $SH $D/main.sh satellite & MAIN=$!
    waitfor "[ -f $T/avahi/services/hassmic.service ] && [ ! -L $T/avahi/services/hassmic.service ]" 60
    grep -q "^nobody nogroup $D/hassmic .*-S$" $T/runas.log; ok $? "hassmic -S runs as the daemon's user: $(grep -- '-S$' $T/runas.log)"
    [ "$(cat $T/victim2)" = keep ] && grep -q "<name>echo-test</name>" $T/avahi/services/hassmic.service; ok $? "the service file replaces the daemon's link, does not write through it"
    waitfor "grep -q host-name=echo-test $T/avahi/avahi-daemon.conf 2>/dev/null"; ok $? "avahi's host name from the file root wrote"
    kill -- -$MAIN 2>/dev/null; wait $MAIN 2>/dev/null; MAIN=
fi

[ $fail = 0 ] && echo "all good" || { echo FAILED; cat $BASE/boot.log; exit 1; }
