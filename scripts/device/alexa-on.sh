#!/system/bin/sh
# Stock Alexa back without a reboot: the inverse of what the satellite setup does (main.sh satellite: lockdown.sh
# services, alexa-off.sh, quiet, netwatch stopping wifisvc, its own avahi-daemon; main.sh firewall: the egress lock).
# Until the next reboot or alexa-off.sh: the property hassmic.alexa=1 makes main.sh run as MODE=stock-online (hassmic
# off, the firewall service keeps only the updaters cut off), and a reboot forgets properties.  So a reboot always
# brings the satellite back, also when adb no longer reaches the Echo (over Wi-Fi it is closed in stock-online unless
# hassmic.conf has ADB_WIFI=1).  Stock Alexa across reboots: MODE=stock-online in hassmic.conf.
# Firmware updates stay impossible throughout: the update guard (lockdown.sh ota-only) has to be in place and pass its
# check before anything of Amazon's that talks to the internet starts; when it does not, the satellite comes back.
# Model: device.conf next to this script (service names, ALEXA_PROP).
umask 022
. "${0%/*}/device.conf" || exit 1
BASE=${HASSMIC_BASE:-/data/local/hassmic}      # only tests/alexa_test.sh sets these two
SYS=${HASSMIC_SYS:-/system/hassmic}
say() { echo "alexa-on: $*"; }

# What the satellite leaves behind: hassmic (a trial run from run.sh is no init service), the tools beside it, the mDNS
# service file and the avahi-daemon main.sh runs in place of init's, the LED patterns, and the AIPC service directory
# btout.c answers the mixer on in btmanagerd's place: btmanagerd has to create it anew and cannot while it is there.
leftovers() {
    pkill hassmic; pkill mixcap; pkill mixplay
    rm -f /data/misc/avahi/services/hassmic.service
    pkill avahi-daemon
    ledctrl -c > /dev/null
    rm -rf /dev/aipc/0
}

# Amazon's services in the order of stock's boot: network and time, the daemons around Alexa, then Alexa.  Not the
# updaters (the firewall service keeps them stopped and cut off) and not oobed: PuffinApp starts setup mode itself when
# the Echo is not registered (OOBED_START, docs/re-platform.md).
stock() {
    for s in $WIFI_SERVICE avahi-daemon $CLOUD_SERVICES $UX_SERVICE $BT_SERVICE; do start $s; done
    # ledcontroller's trigger: init starts the Alexa services, and hassmic.rc the satellite service, which sees
    # hassmic.alexa and exits.  Started by name as well: a property already 1 triggers nothing.
    setprop $ALEXA_PROP 1
    for s in $ALEXA_SERVICES; do start $s; done
    sleep 1
    getprop | grep -E "init.svc.($(echo $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND $BT_SERVICE $UX_SERVICE mixer | tr ' ' '|'))\]"
}

# The firewall watchers lockdown.sh runs (by hand on a trial install: "lockdown.sh watch" would put the egress lock back
# every 5 s).  Matched by command line as main.sh does it, and read with the shell's own read for the same reason.
# shellcheck disable=SC3045
watchers_off() {
    for p in /proc/[0-9]*; do
        c=; while IFS= read -r -d '' a; do c="$c $a"; done 2>/dev/null < $p/cmdline
        case "$c" in *lockdown.sh*watch*) kill ${p#/proc/} 2>/dev/null;; esac
    done
}

# No init services (scripts/deploy.sh, run.sh): nothing to hand over to.  The guard is loaded once; nothing keeps
# update_engine stopped until the next boot, but what it would install comes from otad, which cannot get out.
if [ -z "$(getprop init.svc.hassmic_fw)" ]; then
    leftovers; watchers_off
    sh "${0%/*}/lockdown.sh" ota-only
    stock
    exit 0
fi

[ -f $BASE/hassmic.conf ] || { say "no $BASE/hassmic.conf: hassmic's services do nothing, this Echo runs stock already"; exit 0; }
# The copy init runs (boot.sh): the installed update unless it failed to start three times, else the factory copy.  It
# has to know hassmic.alexa, or the satellite service that ALEXA_PROP starts would stop Alexa again at once.
A=$SYS; t=$(cat $BASE/ota/tries 2>/dev/null)
[ -f $BASE/ota/current/main.sh ] && [ "${t:-0}" -lt 3 ] && A=$BASE/ota/current
grep -q hassmic.alexa $A/main.sh 2>/dev/null ||
    { say "the version that runs ($A) predates alexa-on.sh: update it first (scripts/ota-push.sh), nothing changed"; exit 1; }

back() {
    say "!! $1: satellite back"
    setprop hassmic.alexa 0
    stop hassmic_fw; start hassmic_fw; start hassmic
    exit 1
}
setprop hassmic.alexa 1         # from here on every start of either service is stock-online (main.sh)
stop hassmic                    # main.sh satellite and everything it started: hassmic, netwatch, its avahi-daemon
leftovers
# main.sh firewall again: lockdown.sh ota-only watch, the guard in one iptables-restore, the egress lock gone with it
stop hassmic_fw; start hassmic_fw
i=0
until w=$(sh $A/lockdown.sh ota-only check); do
    i=$((i + 1)); [ $i -lt 40 ] || back "update guard not in place after 10 s ($w)"
    sleep 0.25
done
say "update guard in place, egress lock off; starting Alexa (until reboot or alexa-off.sh)"
stock
