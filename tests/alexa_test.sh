#!/bin/sh
# alexa-on.sh and alexa-off.sh on the PC, against the real main.sh and lockdown.sh: Alexa on, off and on again on an
# installed Echo without a reboot.  Stand-ins: Android's init (start/stop run main.sh as hassmic_fw and hassmic, each in
# its own process group, and ALEXA_PROP=1 starts the Alexa services and the satellite service, as the rc files do),
# properties (getprop/setprop), iptables (tests/fake_iptables.py: the filter table in a file, answering in the wording of
# "iptables -S"), and the tools that only exist on the Echo.  Everything that changes goes into one log, in order: which
# services start and stop, which properties are set, and what state the firewall chain is in after each change.
# Checked: the update guard is in place before anything of Amazon's that talks to the internet starts; otad, ace_otad and
# update_engine never run with Alexa on and are cut off by the firewall; the satellite stays off across a restart of its
# service; alexa-off.sh brings back the egress lock and the satellite; an installed version that predates alexa-on.sh
# is refused.  Run with the Echo's shell where there is one (mksh), else sh.  Needs python3 and setsid.
cd "$(dirname "$0")/.."
SH=${SH:-$(command -v mksh || echo sh)}
command -v setsid > /dev/null || { echo "setsid missing (util-linux)"; exit 1; }
T=$(mktemp -d); fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
waitfor() { i=0; while [ $i -lt ${2:-40} ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }
cleanup() {
    for f in $T/pid.*; do [ -f "$f" ] && kill -KILL -"$(cat $f)" 2>/dev/null; done
    rm -rf $T
}
trap cleanup EXIT

SYS=$T/sys BASE=$T/data P=$T/props L=$T/log
mkdir -p $T/bin $SYS $BASE $P
cp scripts/system/main.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh devices/donut/device.conf $SYS/
. devices/donut/device.conf
echo 'NAME="Test"' > $BASE/hassmic.conf
export HASSMIC_DIR=$SYS HASSMIC_SYS=$SYS HASSMIC_BASE=$BASE FAKE_FW_DIR=$T

# Properties: one file each.  init: start/stop log, set init.svc.<name>, and run main.sh for hassmic's two services.
cat > $T/bin/getprop <<EOF
#!/bin/sh
if [ \$# = 0 ]; then for f in $P/*; do echo "[\${f##*/}]: [\$(cat \$f)]"; done; else cat $P/\$1 2>/dev/null; fi
EOF
cat > $T/bin/setprop <<EOF
#!/bin/sh
old=\$(cat $P/\$1 2>/dev/null); echo "\$2" > $P/\$1
case \$1 in ctl.*|service.adb.*|hassmic.adb.*) exit 0;; esac
echo "setprop \$1 \$2" >> $L
# the stock rc files and hassmic.rc: on property:$ALEXA_PROP=1
[ "\$1" = $ALEXA_PROP ] && [ "\$2" = 1 ] && [ "\$old" != 1 ] && for s in $ALEXA_SERVICES hassmic; do start \$s; done
exit 0
EOF
cat > $T/bin/start <<EOF
#!/bin/sh
echo "start \$1" >> $L
case \$1 in hassmic_fw) m=firewall;; hassmic) m=satellite;; *) echo running > $P/init.svc.\$1; exit 0;; esac
# init does not start a running service again
[ -f $T/pid.\$1 ] && kill -0 -"\$(cat $T/pid.\$1)" 2>/dev/null && exit 0
echo running > $P/init.svc.\$1
setsid $SH -c "$SH $SYS/main.sh \$m; echo stopped > $P/init.svc.\$1" > /dev/null 2>&1 < /dev/null &
echo \$! > $T/pid.\$1
EOF
cat > $T/bin/stop <<EOF
#!/bin/sh
echo "stop \$1" >> $L
echo stopped > $P/init.svc.\$1
[ -f $T/pid.\$1 ] && { kill -KILL -"\$(cat $T/pid.\$1)" 2>/dev/null; rm -f $T/pid.\$1; }
exit 0
EOF
printf '#!/bin/sh\necho "pkill $*" >> %s\n' $L > $T/bin/pkill
printf '#!/bin/sh\necho "          inet addr:192.168.1.5  Bcast:192.168.1.255"\n' > $T/bin/ifconfig
printf '#!/bin/sh\n[ "$1 $2" = "-u %s" ] && { echo 5008; exit 0; }\nexec %s "$@"\n' $UPDATE_UID "$(command -v id)" > $T/bin/id
for t in ledctrl wpa_cli avahi-daemon chown chcon insmod; do printf '#!/bin/sh\nexit 0\n' > $T/bin/$t; done
for t in iptables ip6tables iptables-restore ip6tables-restore; do
    printf '#!/bin/sh\nFAKE_TOOL=%s exec python3 %s/tests/fake_iptables.py "$@"\n' $t "$(pwd)" > $T/bin/$t
done
chmod 755 $T/bin/*
# the satellite: runas drops its own arguments, hassmic says that it runs
printf '#!/bin/sh\nshift 4; exec "$@"\n' > $SYS/runas
printf '#!/bin/sh\ncase " $* " in *" -S "*) exit 0;; esac\necho $$ > %s/hassmic.pid\nexec sleep 600\n' $T > $SYS/hassmic
chmod 755 $SYS/*
export PATH=$T/bin:$PATH

satellite_runs() { [ -f $T/hassmic.pid ] && kill -0 "$(cat $T/hassmic.pid)" 2>/dev/null; }
fw() { iptables -w -S hassmic_out 2>/dev/null; }
# log lines from line $1 on
since() { tail -n +$(($1 + 1)) $L; }
mark() { wc -l < $L | tr -d ' '; }
# line number (within since $1) of the first line matching the regex $2, or 99999
first() { n=$(since $1 | grep -n -E -m1 "$2" | cut -d: -f1); echo ${n:-99999}; }

# Boot as init does it: the stock services up, hassmic_fw at "on boot", ledcontroller's ALEXA_PROP=1 later
echo 192.168.1.1 > $P/net.dns1; echo 5555 > $P/service.adb.tcp.port; echo running > $P/init.svc.adbd
for s in $ALEXA_SERVICES $UPDATE_SERVICES $CLOUD_SERVICES $BT_SERVICE $UX_SERVICE $WIFI_SERVICE avahi-daemon mixer; do
    echo running > $P/init.svc.$s
done
: > $L
start hassmic_fw
setprop $ALEXA_PROP 1
waitfor satellite_runs 60 && waitfor "sh $SYS/lockdown.sh check > /dev/null" 40
ok $? "boot: satellite runs, egress lock in place"

# 1. A version that predates alexa-on.sh runs: refused, nothing changed
mkdir -p $BASE/ota/v1; sed '/hassmic.alexa/d' $SYS/main.sh > $BASE/ota/v1/main.sh; ln -s $BASE/ota/v1 $BASE/ota/current
m=$(mark); out=$($SH $SYS/alexa-on.sh); rc=$?
[ $rc = 1 ] && echo "$out" | grep -q "predates alexa-on.sh" && [ -z "$(since $m)" ] && satellite_runs
ok $? "installed version without hassmic.alexa: refused, nothing touched (rc $rc)"
echo 3 > $BASE/ota/tries
m=$(mark); $SH $SYS/alexa-on.sh > /dev/null; rc=$?
[ $rc = 0 ]; ok $? "... the same update after three failed starts: the factory copy runs, which knows it (rc $rc)"
$SH $SYS/alexa-off.sh > /dev/null; waitfor satellite_runs 40
rm -rf $BASE/ota/current $BASE/ota/v1 $BASE/ota/tries

# 2. on, off, on
alexa_on() {
    m=$(mark); out=$($SH $SYS/alexa-on.sh 2>&1); rc=$?
    [ $rc = 0 ]; ok $? "$1: alexa-on.sh rc $rc"
    sleep 1
    [ "$(getprop hassmic.alexa)" = 1 ] && [ "$(getprop $ALEXA_PROP)" = 1 ]; ok $? "$1: hassmic.alexa=1, $ALEXA_PROP=1"
    fwon=$(first $m "^fw iptables ota-only")
    [ $(first $m "^setprop hassmic.alexa 1") -lt $(first $m "^stop hassmic$") ]
    ok $? "$1: hassmic.alexa set before the satellite service is stopped (main.sh's own alexa-off.sh cannot undo it)"
    bad=; for s in $WIFI_SERVICE avahi-daemon $CLOUD_SERVICES $UX_SERVICE $BT_SERVICE $ALEXA_SERVICES; do
        n=$(first $m "^start $s$"); [ $n -lt 99999 ] && [ $fwon -lt $n ] || bad="$bad $s"
    done
    [ $fwon -lt 99999 ] && [ -z "$bad" ]; ok $? "$1: update guard loaded before each of Amazon's services starts${bad:+ (not:$bad)}"
    ! since $m | grep -q -E "^start ($(echo $UPDATE_SERVICES $UPDATE_ONDEMAND | tr ' ' '|'))$"; ok $? "$1: no updater started"
    [ $(since $m | sed -n "$fwon,\$p" | grep -c "^fw iptables lock") = 0 ]; ok $? "$1: egress lock not put back"
    fw | grep -q -- "--uid-owner 5008 -j DROP" && ip6tables -S hassmic_out | grep -q -- "--uid-owner 5008 -j DROP" &&
        ! fw | grep -q -x -- "-A hassmic_out -j DROP" && sh $SYS/lockdown.sh ota-only check
    ok $? "$1: hassmic_out is the update guard in both tables (ace_otad's uid dropped, nothing else), its check passes"
    ! satellite_runs && [ "$(getprop init.svc.hassmic)" = stopped ] && grep -q "stock-online (alexa-on.sh" $BASE/boot.log
    ok $? "$1: satellite off, the service ALEXA_PROP started exited as stock-online"
}
alexa_on "on"

m=$(mark); stop hassmic; start hassmic; sleep 2
! satellite_runs && ! since $m | grep -q -E "^stop ($(echo $ALEXA_SERVICES | tr ' ' '|'))$"
ok $? "on: a restart of the satellite service keeps Alexa running"

# update_engine is started on demand: the firewall service stops it again; a flush by stock's firewall.sh is undone
m=$(mark); start $UPDATE_ONDEMAND
waitfor "since $m | grep -q '^stop $UPDATE_ONDEMAND$'" 40; ok $? "on: $UPDATE_ONDEMAND started on demand is stopped again"
iptables -w -F hassmic_out
waitfor "fw | grep -q uid-owner" 40; ok $? "on: update guard back after stock's firewall.sh emptied the chain"

m=$(mark); out=$($SH $SYS/alexa-off.sh 2>&1); rc=$?
[ $rc = 0 ] && [ "$(getprop hassmic.alexa)" = 0 ] && [ "$(getprop $ALEXA_PROP)" = 0 ]; ok $? "off: rc $rc, hassmic.alexa=0, $ALEXA_PROP=0"
bad=; for s in $ALEXA_SERVICES $BT_SERVICE; do [ "$(getprop init.svc.$s)" = stopped ] || bad="$bad $s"; done
[ -z "$bad" ]; ok $? "off: Alexa and Bluetooth stopped${bad:+ (not:$bad)}"
[ $(first $m "^start hassmic_fw$") -lt $(first $m "^start hassmic$") ]; ok $? "off: firewall service before the satellite"
waitfor satellite_runs 60 && waitfor "sh $SYS/lockdown.sh check > /dev/null" 40; ok $? "off: satellite runs, egress lock in place"
waitfor "[ \"\$(getprop init.svc.$WIFI_SERVICE)\" = stopped ]" 60
bad=; for s in $CLOUD_SERVICES $UX_SERVICE $UPDATE_SERVICES $WIFI_SERVICE; do [ "$(getprop init.svc.$s)" = stopped ] || bad="$bad $s"; done
[ -z "$bad" ]; ok $? "off: cloud daemons, $UX_SERVICE, updaters, $WIFI_SERVICE stopped again${bad:+ (not:$bad)}"

alexa_on "on again"

[ $fail = 0 ] && echo "all good" || { echo FAILED; echo "--- log"; cat $L; echo "--- boot.log"; cat $BASE/boot.log; exit 1; }
