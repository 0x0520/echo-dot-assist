#!/system/bin/sh
# The updatable part of the boot logic; started by /system/hassmic/boot.sh (root, su domain), which picks the factory copy
# or a signed update and exports HASSMIC_DIR (where this script and its neighbours are) and HASSMIC_SYS.
#   main.sh firewall    egress lock + re-assert loop, and the root side of push updates, of Wi-Fi setup over
#                       Bluetooth and of the task manager's kill; started at "on boot"
#   main.sh satellite   Alexa off, then keep hassmic running, and check that the firewall service keeps its rules right
# Model: device.conf next to this script (devices/<codename>/device.conf: service names, the daemon's user).
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   NAME="Echo Dot"             optional, default DEFAULT_NAME from device.conf
#   PROTO=esphome               optional: esphome (default, port 26053) or wyoming (port 16700)
#   ARGS=""                     optional extra hassmic arguments
#   MODE=stock-online           optional: stock Alexa with internet, e.g. to let it fetch a wake-word model.  hassmic stays
#                               off, nothing is stopped or blocked except firmware updates (lockdown.sh ota-only).  The
#                               property hassmic.alexa=1 (alexa-on.sh) means the same until the next reboot
#   ADB_WIFI=1                  optional: leave adb over Wi-Fi open (root shell for the whole network, no password; read
#                               by lockdown.sh).  Without it: closed, opened for 30 min by a switch in Home Assistant
#   ADB_WIFI_FROM=192.168.1.20  optional: what ADB_WIFI=1 and that switch admit, one IPv4 address or subnet instead of the
#                               whole network (lockdown.sh; scripts/adb-wifi.sh always admits only the PC it ran on)
umask 022                                   # init gives us 077; what we create must be readable by the daemon's user
D=${HASSMIC_DIR:-/system/hassmic}
SYS=${HASSMIC_SYS:-/system/hassmic}
BASE=${HASSMIC_BASE:-/data/local/hassmic}      # only tests/boot_test.sh sets it
OTA=$BASE/ota
CONF=$BASE/hassmic.conf
LOG=$BASE/boot.log
[ -f $CONF ] || exit 0
# A push update runs "firewall" again (exec, same PID) while the previous firewall watcher is still looping: stop it first,
# or every update adds one and old and new rules take turns.  Matched by command line, which also catches the ones that
# earlier versions left behind.  Read with the shell itself: a process that exits between the open and the read leaves
# the Echo 2's tr (Fire OS 6572) spinning on the read error for ever, and this script never got to the firewall watcher
# and the installer below (seen 2026-09-30 right after a boot: no egress lock, push updates unanswered).
if [ "$1" = firewall ]; then
    # read -d is mksh's (and bash's), not POSIX: the Echo's shell has it
    # shellcheck disable=SC3045
    for p in /proc/[0-9]*; do
        c=; while IFS= read -r -d '' a; do c="$c $a"; done 2>/dev/null < $p/cmdline
        case "$c" in *lockdown.sh*watch*) kill ${p#/proc/} 2>/dev/null;; esac
    done
fi

# Without the model's facts or a config that loads the satellite cannot start, but the firewall needs neither: it must go
# up regardless.  A config that does not load is not the update's fault: that start does not count against it (boot.sh).
no_satellite() {
    echo "== $1: no satellite" >> $LOG
    [ "$MAIN_MODE" = firewall ] && exec sh $D/lockdown.sh watch >> $LOG 2>&1
    [ "$2" = conf ] && echo 0 > $OTA/tries
    exit 1
}
MAIN_MODE=$1
[ -f $D/device.conf ] || no_satellite "$D/device.conf missing"
. $D/device.conf
# Root runs what this file says, and ADB_WIFI in it opens a root shell: nobody else may write it.  "adb push" leaves it
# writable for everyone (seen on two of three Echos), and hassmic faces the network.
chown root:root $CONF; chmod 644 $CONF
# The config is edited by hand, and pushed from Windows it comes with CR LF: mksh keeps the CR in every value.  An `exit`
# in it would end this script while sourcing it, firewall service included, so it is tried in a subshell first; a stray
# quote mksh reports and then goes on past (tests/boot_test.sh), with whatever half of the file it got, so -n for that.
if grep -q "$(printf '\r')" $CONF; then
    tr -d '\r' < $CONF > $CONF.tmp && chmod 644 $CONF.tmp && mv $CONF.tmp $CONF && echo "== $CONF had CR LF line endings, converted" >> $LOG
fi
sh -n $CONF 2> /dev/null && [ "$(. $CONF > /dev/null 2>&1; echo ok)" = ok ] || no_satellite "$CONF does not load (shell syntax)" conf
. $CONF
NAME=${NAME:-$DEFAULT_NAME}
# scripts/device/alexa-on.sh: stock Alexa until alexa-off.sh or the next reboot, which forgets the property
ALEXA_ON=; [ "$(getprop hassmic.alexa)" = 1 ] && { MODE=stock-online; ALEXA_ON=1; }

# uxeventd plays the "ready for setup" voice prompts and the orange setup spinner on an unregistered device.  hassmic drives
# LEDs (ledctrl) and earcons itself, so it goes.  Stopped before "class_start main" it never starts (SVC_DISABLED).
quiet() { for s in $UX_SERVICE $SETUP_SERVICES; do stop $s; done; }

# Amazon's wifisvc runs HTTP connectivity tests against AWS hosts.  Behind the egress lock they always fail, and it then
# tears the Wi-Fi link down and rebuilds it (seen: ~100 s after boot, link gone for 193 s, and again later).  It is only needed
# to bring the link up: wpa_supplicant (saved profile, reconnects by itself) and dhcpcd keep it up.  So stop it once there is
# an address, and let it run again only if the address stays away for a minute.
# boot.log is appended to by several long-lived processes (hassmic, the firewall watcher, this loop), all through ">>",
# i.e. O_APPEND.  So it is rotated by copy + truncate: their next write simply lands at the new end.  One old copy is kept.
LOG_MAX=1048576
rotate_log() {
    [ "$(wc -c < $LOG 2>/dev/null || echo 0)" -gt $LOG_MAX ] || return 0
    cp $LOG $LOG.1 && : > $LOG && echo "== log rotated, previous part in $LOG.1"
}

# The firewall rules belong to the firewall service (hassmic_fw: "lockdown.sh watch" loads them and checks them every
# 5 s).  That service can fail without anyone noticing: on the Echo 2 it hung at its start and there was no lock for the
# whole boot (2026-09-30, see the scan below).  So this service looks too, every 10 s, with the same check ("lockdown.sh
# check": every rule of the chain, the chain first in OUTPUT, INPUT policy DROP, each of stock's rules the satellite
# needs, IPv6 off where it cannot be filtered): wrong twice in a row means the watcher is not doing its job.  Then say so, stop that service (two runs building the chain at once interleave their
# rules), load the rules from here, and start the service again.
fwmiss=0
fwcheck() {
    if fwwrong=$(sh $D/lockdown.sh check); then fwmiss=0; return; fi
    fwmiss=$((fwmiss + 1))
    [ $fwmiss -ge 2 ] || return
    echo "== firewall rules wrong on two looks 10 s apart ($fwwrong; uptime $(cut -d. -f1 /proc/uptime)s): the firewall service is not doing its job; rules loaded from here, service restarted"
    stop hassmic_fw
    sh $D/lockdown.sh > /dev/null
    start hassmic_fw
    fwmiss=0
}

# Wi-Fi motion (device.conf KMOD): our kernel module hooks the Wi-Fi driver's receive path for the level of every frame
# from the access point (src/kmod/).  Loaded only once Wi-Fi motion is switched on (field 13 of hassmic's settings
# file; hassmic waits for /proc/<module> meanwhile), so while it is off - the default - no kernel code is touched; and
# only with the link up, so that a driver that is a module itself (donut's) is there to be hooked.  It cannot be
# unloaded: it stays until the next reboot.  A failed load is not tried again until then.
kmod() {
    [ -n "$KMOD" ] && [ -f $D/$KMOD ] || return 0
    grep -q "^${KMOD%.ko} " /proc/modules && return 0
    set -- $(cat $BASE/state/settings 2>/dev/null)
    [ "${13}" = 1 ] || return 0
    if out=$(insmod $D/$KMOD $KMOD_ARGS 2>&1); then echo "kmod: $KMOD loaded (Wi-Fi motion switched on)"
    else echo "kmod: $KMOD not loaded, not tried again until reboot: $out"; KMOD=; fi
}

netwatch() {
    miss=0
    while :; do
        rotate_log
        fwcheck
        # Holding the action button 5 s is hassmic's Wi-Fi setup over Bluetooth now (improv.c); acebuttond still sees
        # the hold and may start stock's setup mode, whose Wi-Fi Direct group and soft AP would get in the way.
        for s in $SETUP_SERVICES; do
            [ "$(getprop init.svc.$s)" = running ] && { stop $s; echo "netwatch: $s stopped (stock setup mode)"; }
        done
        if ifconfig $WLAN 2>/dev/null | grep -q "inet addr"; then
            miss=0
            kmod
            [ "$(getprop init.svc.$WIFI_SERVICE)" = running ] && { sleep 5; stop $WIFI_SERVICE; echo "netwatch: link up, $WIFI_SERVICE stopped"; }
        else
            miss=$((miss + 1))
            [ $miss -ge 6 ] && [ "$(getprop init.svc.$WIFI_SERVICE)" != running ] && { start $WIFI_SERVICE; echo "netwatch: no address for 60 s, $WIFI_SERVICE started"; miss=0; }
        fi
        sleep 10
    done
}

# Root side of a push update.  hassmic (DAEMON_USER) received a bundle, checked its signature and left it in state/ota/
# with a "request" file.  Here the bundle is verified again by the tool and key from the read-only system partition - that
# check is the one that counts - unpacked into a fresh root-owned directory and made current, if it was built for this
# model (its device.conf names the product this Echo reports).  This runs in the firewall service so that it can restart
# the satellite service.
# Two keys count: the owner's (update.pub, written by install-system.sh: push updates) and the project's release key
# (keys/release.pub, in every build: the online updates hassmic downloads once the owner switches them on in Home
# Assistant, update.c).  The release key of the copy that runs wins over the factory copy's: both are root's, written
# from bundles that verified, and so a new release key can come with an update signed by the old one.
release_pub() { for k in $D/release.pub $SYS/release.pub; do [ -s $k ] && { echo $k; return; }; done; }
rejected() { echo "== update rejected: $1"; rm -rf $new; echo "FAILED $1" > $RES; }
# What boot.sh and init will run from the bundle must parse as it is: a CR (packed from a Windows checkout) becomes part
# of every value, `D=/system/hassmic\r` and nothing after boot starts, the egress lock included.
broken_scripts() {
    for f in $1/*.sh $1/device.conf $1/hassmic.rc; do
        [ -f "$f" ] || continue
        if grep -q "$(printf '\r')" "$f"; then echo "${f##*/} has CR LF line endings"; return; fi
        case "$f" in *.sh) sh -n "$f" 2> /dev/null || { echo "${f##*/} does not parse"; return; };; esac
    done
    return 1
}
# Every bundle CI ever published verifies against the release key: handed in again (a hassmic taken over through the
# network, a push replayed), an old one would put back what a later release fixed.  So a release-signed bundle must not
# be older than the copy that runs ($D) or the factory copy ($SYS); going back is the owner's call, with update.pub.
# Versions are the commit's time in UTC, 2026.10.02.091530, maybe with +<commit>[-dirty] (a build of one's own) or a
# tag's -beta: ver_time gives its 14 digits, or nothing for any other form (a factory copy without VERSION, a test).
# Compared as text, both the same length: mksh's arithmetic is 32 bits.
ver_time() {
    _t=$(echo "${1%%[+-]*}" | tr -d .)
    case "$_t" in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) echo $_t;; esac
}
# older VERSION: prints the newer version this Echo has, if it has one
older() {
    _n=$(ver_time "$1"); [ -n "$_n" ] || return 1
    for _f in $D/VERSION $SYS/VERSION; do
        _h=$(ver_time "$(cat $_f 2>/dev/null)")
        [ -n "$_h" ] && [ "$_n" != "$_h" ] && [ "$(printf '%s\n%s\n' $_n $_h | sort | head -1)" = $_n ] && { cat $_f; return 0; }
    done
    return 1
}
# The hassmic that runs is the installed update's own binary: not the factory copy after a fall back, not a test binary
# from deploy.sh in /data.
runs_current() {
    for p in $(pidof hassmic); do [ "$(readlink /proc/$p/exe)" = "$1/hassmic" ] && return 0; done
    return 1
}
# The installed update passed its self test (hassmic left state/ota/healthy: started, wake word engine loaded, a second
# of microphone audio; main.c): it becomes the factory copy on the system partition, bootstrap included, so the Echo
# falls back to the last version that worked.  Only if it is the update installed now and runs_current.
factory() {
    want=$1 cur=$(readlink $OTA/current)
    have=$(cat $cur/VERSION 2>/dev/null)
    if [ -z "$cur" ] || [ "$have" != "$want" ]; then echo "FAILED $want is not the installed update (that is ${have:-none}); nothing written"
    elif ! runs_current $cur; then echo "FAILED $want is installed but not what runs now (fell back to the factory copy?); nothing written"
    elif cmp -s $cur/VERSION $SYS/VERSION; then echo "OK $want is the factory copy already"
    elif out=$(sh $cur/sysinstall.sh factory $cur 2>&1); then echo "$out" >&2; echo "OK $want is now the factory copy"
    else echo "$out" >&2; echo "FAILED $(echo "$out" | tail -1)"
    fi
}
# state/ota/ is the daemon's: whatever root writes there goes through a name the daemon may have made a link to another
# file (result.tmp -> /data/local/hassmic/hassmic, and root would create and chown that for it, then run it).  So the
# result is written in root's own directory and renamed into place; a rename replaces a link, it does not follow it.
RES=$OTA/result.tmp
# Root side of Wi-Fi setup over Bluetooth (Improv; hassmic's improv.c).  hassmic got an SSID and passphrase from a
# client after a press of the action button and left them in state/wifi-request (0600): line 1 the SSID in hex, line 2
# the passphrase.  It is moved into root's own directory before anything reads it (the daemon cannot swap it for a link
# from then on), checked again, joined by wifi-join.sh, which hands both to wpa_cli as single arguments and never
# evaluates them, and answered in state/wifi-result as ota_watch answers ("OK ..." / "FAILED ..."), written in root's
# directory and renamed into the daemon's.  The passphrase is never logged.
WREQ=$BASE/state/wifi-request
NL='
'
# The answer to a request of hassmic's, as state/$1: written in root's own directory and renamed into the daemon's, so a
# link the daemon left there is replaced, not written through
answer() {
    echo "$2" > $BASE/$1.tmp
    chown $DAEMON_USER $BASE/$1.tmp; chmod 644 $BASE/$1.tmp
    rm -rf $BASE/state/$1                   # a link to a directory there would take the rename into it
    mv -f $BASE/$1.tmp $BASE/state/$1
}
wifi_bad() {        # prints why SSID ($1, hex) and passphrase ($2) are refused; nothing if fine
    case "$1" in ''|*[!0-9a-f]*) echo "SSID not in hex"; return;; esac
    { [ ${#1} -le 64 ] && [ $((${#1} % 2)) = 0 ]; } || { echo "SSID length"; return; }
    [ -z "$(printf %s "$2" | tr -d ' -~')" ] || { echo "passphrase not printable ASCII"; return; }
    case ${#2} in
    0) ;;
    64) case "$2" in *[!0-9a-fA-F]*) echo "64-character key not hex";; esac;;
    *) { [ ${#2} -ge 8 ] && [ ${#2} -le 63 ]; } || echo "passphrase length";;
    esac
}
wifi_watch() {
    [ -e $WREQ ] || [ -L $WREQ ] || return 0
    t=$BASE/wifi-request.taken c=$BASE/wifi-join.req
    rm -rf $t $c; mv -f $WREQ $t 2>/dev/null || return 0
    # The inode is still the daemon's (an open descriptor, a hard link): read once, at most 201 bytes, and hand
    # wifi-join.sh a file of root's own with what was checked.
    why=
    if [ -L $t ] || [ ! -f $t ]; then why="not a plain file"
    else
        req=$(head -c 201 $t)
        if [ ${#req} -gt 200 ]; then why="too long"
        else
            ssid=${req%%"$NL"*}; psk=
            case "$req" in *"$NL"*) psk=${req#*"$NL"};; esac       # more lines: not printable, refused
            why=$(wifi_bad "$ssid" "$psk")
        fi
    fi
    if [ -n "$why" ]; then res="FAILED request refused: $why"
    else
        (umask 077; printf '%s\n%s\n' "$ssid" "$psk" > $c)
        echo "== Wi-Fi setup over Bluetooth: joining the network asked for through hassmic"
        if out=$(sh $D/wifi-join.sh -x $c 2>&1); then res="OK $(echo "$out" | tail -1)"; else res="FAILED $(echo "$out" | tail -1)"; fi
    fi
    rm -rf $t $c; ssid=; psk=; req=
    echo "== Wi-Fi setup over Bluetooth: $res"
    answer wifi-result "$res"
}

# Root side of the task manager's kill (hassmic's taskmgr.c: the action "kill_process" in Home Assistant, which hassmic
# takes only over the connection with the key).  hassmic may not signal another user's process, so it leaves
# state/kill-request (0600): "<pid> <start time> TERM|KILL", the start time being field 22 of /proc/<pid>/stat as it read
# it: a process that ended meanwhile cannot take whatever got its number with it.  Taken into root's own directory as the
# Wi-Fi requests are, checked as numbers, and only signalled if it neither keeps the Echo running and reachable nor is part
# of what keeps the egress lock: not init and the core daemons (KILL_KEEP), no kernel thread, not these scripts (this
# service, lockdown.sh watch) nor anything they run (their sleeps: one killed ends its loop), not wpa_supplicant or dhcpcd
# (the way to the Echo), not the mixer (hassmic's audio front end; not seen to come back on its own).  Amazon's other
# daemons may go, and hassmic itself: the satellite service starts it again.  TERM, KILL after 3 s if it is still there.
KREQ=$BASE/state/kill-request
KILL_KEEP="init ueventd logd servicemanager hwservicemanager vndservicemanager vold netd adbd zygote main surfaceflinger
           watchdogd lmkd healthd kthreadd wpa_supplicant dhcpcd mixer iptables ip6tables iptables-restore ip6tables-restore"
# kstat PID: kcomm, kstate, kppid, kstart of it; fails when it is gone.  The name may hold spaces and ")": the fields
# start after the last ") ".  read, not a tool: no process per look.
kstat() {
    _s=; { IFS= read -r _s < /proc/$1/stat; } 2>/dev/null; [ -n "$_s" ] || return 1
    kcomm=${_s#*\(}; kcomm=${kcomm%\)*}
    set -- ${_s##*") "}
    kstate=$1 kppid=$2 kstart=${20}
    [ -n "$kstart" ]
}
# kcmd PID: its command line in kcmdline, arguments after a space each; empty for a kernel thread
kcmd() {
    kcmdline=
    # read -d is mksh's (and bash's), not POSIX: the Echo's shell has it
    # shellcheck disable=SC3045
    while IFS= read -r -d '' _a; do kcmdline="$kcmdline $_a"; done 2>/dev/null < /proc/$1/cmdline
}
scripted() { case "$kcmdline" in *main.sh*|*boot.sh*|*lockdown.sh*|*sysinstall.sh*|*wifi-join.sh*) return 0;; esac; return 1; }
kalive() { kstat $1 && [ "$kstart" = "$2" ] && [ "$kstate" != Z ]; }    # PID START: that process, not a zombie of it
# why PID (just looked at with kstat) must not be killed; nothing if it may
kill_bad() {
    [ $1 -gt 2 ] || { echo "pid $1 is init or the kernel's"; return; }
    [ "$kppid" != 2 ] || { echo "$1 is a kernel thread"; return; }
    kcmd $1; [ -n "$kcmdline" ] || { echo "$1 has no command line (a kernel thread, or ending)"; return; }
    _a0=${kcmdline# }; _a0=${_a0%% *}; _a0=${_a0##*/}
    for _k in $KILL_KEEP; do
        if [ "$kcomm" = "$_k" ] || [ "$_a0" = "$_k" ]; then echo "$1 is $_k, which the Echo needs"; return; fi
    done
    scripted && { echo "$1 is one of the boot and firewall scripts"; return; }
    case "$(readlink /proc/$1/exe)" in */hassmic) return;; esac     # the satellite service's loop starts it again
    kcmd $kppid
    scripted && echo "$1 runs for the boot and firewall scripts"
}
kill_watch() {
    [ -e $KREQ ] || [ -L $KREQ ] || return 0
    t=$BASE/kill-request.taken
    rm -rf $t; mv -f $KREQ $t 2>/dev/null || return 0
    why=; req=
    if [ -L $t ] || [ ! -f $t ]; then why="not a plain file"
    else req=$(head -c 65 $t); [ ${#req} -le 64 ] || why="too long"
    fi
    rm -rf $t
    set -f; set -- $req; set +f
    if [ -n "$why" ]; then :
    elif [ $# != 3 ]; then why="not three fields"
    else
        case "$1" in ''|0*|*[!0-9]*) why="pid not a number";; esac
        case "$2" in ''|*[!0-9]*) why="start time not a number";; esac
        case "$3" in TERM|KILL) ;; *) why="signal neither TERM nor KILL";; esac
        { [ ${#1} -le 7 ] && [ ${#2} -le 20 ]; } || why="number too long"
    fi
    pid=$1 start=$2 sig=$3
    if [ -z "$why" ]; then
        if ! kstat $pid; then why="no process $pid"
        elif [ "$kstart" != "$start" ]; then why="$pid is not the process asked for any more (it ended, the number is reused)"
        else why=$(kill_bad $pid)
        fi
    fi
    if [ -n "$why" ]; then res="FAILED refused: $why"
    else
        name="$(printf %s "$kcomm" | tr -cd ' -~') ($pid)"     # what it calls itself, on one line: log, Home Assistant
        kill -$sig $pid 2>/dev/null
        i=0; while [ $i -lt 3 ] && kalive $pid $start; do sleep 1; i=$((i + 1)); done
        if ! kalive $pid $start; then res="OK $name ended on $sig"
        elif [ $sig = TERM ] && kill -KILL $pid 2>/dev/null && sleep 1 && ! kalive $pid $start; then
            res="OK $name ended on KILL, TERM was ignored for 3 s"
        else res="FAILED $name still runs after $sig"
        fi
    fi
    echo "== kill: $res"
    answer kill-result "$res"
}
ota_watch() {
    IN=$BASE/state/ota
    while sleep 2; do
        wifi_watch
        kill_watch
        if [ -f $IN/healthy ]; then
            rm -f $IN/healthy
            cur=$(readlink $OTA/current)
            # Only a self test that passed counts as the update coming up: a hassmic that merely runs may hang without a
            # push port, and the factory copy running after a fall back must not wipe the count that caused it.
            [ -n "$cur" ] && runs_current $cur && echo 0 > $OTA/tries
            # every start says so; only an update that is not the factory copy yet has anything to do
            if [ -n "$cur" ] && [ -f $cur/VERSION ] && ! cmp -s $cur/VERSION $SYS/VERSION; then
                echo "== factory copy: $(factory "$(cat $cur/VERSION)")"      # its stderr: the log
            fi
        fi
        [ -f $IN/request ] || continue
        rm -f $IN/request $IN/result $RES
        new=$OTA/v$(cut -d. -f1 /proc/uptime)-$$
        key=
        for k in $SYS/update.pub $(release_pub); do
            [ -s $k ] && $SYS/otatool verify $k $IN/bundle $IN/bundle.sig > /dev/null 2>&1 && { key=$k; break; }
        done
        if [ ! -f $SYS/update.pub ]; then echo "FAILED no update key on this device (install-system.sh puts it there)" > $RES
        elif [ -z "$key" ]; then rejected "the signature verifies against neither the update key nor the release key"
        elif ! ver=$($SYS/otatool install $key $IN/bundle $IN/bundle.sig $new 2>&1); then
            rejected "$(echo "$ver" | tail -1)"
        elif [ "$key" != $SYS/update.pub ] && have=$(older "$ver"); then
            rejected "version $ver is older than $have on this Echo; only the update key (scripts/ota-push.sh) goes back"
        elif why=$(broken_scripts $new); then
            rejected "version $ver: $why; not installed"
        elif prod=$(. $new/device.conf 2>/dev/null && echo "$PRODUCT"); [ "$prod" != "$(getprop ro.product.device)" ]; then
            rejected "version $ver is built for ${prod:-an unknown model}, this Echo is $(getprop ro.product.device); not installed"
        elif ! { chmod 755 $new && [ -f $new/main.sh ] && $new/runas $DAEMON_USER shell $new/hassmic -T > /dev/null 2>&1; }; then  # the daemon's user can really run it
            rejected "version $ver does not run as the daemon's user (self-check failed), not installed"
        elif [ -f $new/otatool ] && ! $new/otatool verify $key $IN/bundle $IN/bundle.sig > /dev/null 2>&1; then
            # Once it passes its self test, its otatool becomes the one on the system partition that checks every later
            # update: so it must pass that check itself.
            rejected "version $ver brings an otatool that does not verify it; not installed"
        else
            old=$(readlink $OTA/current)
            ln -sfn $new $OTA/current                   # toybox: replaces the link itself (checked on the device); no mv -T there
            echo 0 > $OTA/tries
            for d in $OTA/v*; do [ "$d" = "$new" ] || [ "$d" = "$old" ] || rm -rf "$d"; done        # keep the previous one
            # A test binary from deploy.sh would go on running instead, and the update never become the factory copy.
            [ -f $BASE/hassmic ] && rm -f $BASE/hassmic && echo "== test binary $BASE/hassmic removed: the update runs instead"
            echo "== update $ver installed (signed with ${key##*/}), restarting"
            echo "OK $ver" > $RES
        fi
        rm -f $IN/bundle $IN/bundle.sig
        res=$(cat $RES)
        # The rename replaces a file or a link, but onto a directory (or a link to one) mv moves the result into it,
        # wherever that is.  rm -f above leaves a directory standing, and the daemon may plant either meanwhile: clear it.
        # rm does not follow a link it is given, nor one inside a directory.
        if [ -L $IN/result ] || { [ -e $IN/result ] && [ ! -f $IN/result ]; }; then rm -rf $IN/result; fi
        chown $DAEMON_USER $RES; mv -f $RES $IN/result
        case "$res" in OK*)
            sleep 2                                     # let hassmic relay the result to the pusher
            stop hassmic; start hassmic
            exec sh $SYS/boot.sh firewall               # and run the new firewall script as well
        esac
    done
}

if [ "$MODE" = stock-online ]; then
    # hassmic is off on purpose: that must not count as an update that failed to come up.
    [ "$1" = firewall ] || { echo 0 > $OTA/tries; exit 0; }
    { rotate_log; echo "== stock-online${ALEXA_ON:+ (alexa-on.sh, until reboot or alexa-off.sh)}, uptime $(cut -d. -f1 /proc/uptime)s: Alexa runs, updaters cut off"; } >> $LOG 2>&1
    exec sh $D/lockdown.sh ota-only watch >> $LOG 2>&1
fi

case "$1" in
firewall)
    quiet
    sh $D/lockdown.sh watch >> $LOG 2>&1 &
    ota_watch >> $LOG 2>&1
    ;;
satellite)
    # The installer that unpacked us may have been an older one running with umask 077: make sure the daemon's user gets in.
    [ "$D" != "$SYS" ] && chmod 755 $D
    {
        rotate_log
        echo "== satellite start, uptime $(cut -d. -f1 /proc/uptime)s, $(cat $D/VERSION 2>/dev/null || echo factory) from $D"
        sh $D/lockdown.sh services        # stops the cloud daemons that were not up yet at "on boot"; the firewall is the watcher's
        sh $D/alexa-off.sh services; quiet
    } >> $LOG 2>&1
    # A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
    BIN=$D/hassmic; [ -x $BASE/hassmic ] && BIN=$BASE/hassmic
    # hassmic offers Wi-Fi motion where the module can be loaded (wifimotion.c), the loop below loads it when needed
    [ -n "$KMOD" ] && [ -f $D/$KMOD ] && export HASSMIC_WIFI_KMOD=/proc/${KMOD%.ko}
    # online updates: hassmic checks downloads against the key root will check them against (ota.c)
    export HASSMIC_RELEASE_PUB=$(release_pub)
    netwatch >> $LOG 2>&1 & NETWATCH=$!
    # mDNS through the stock avahi-daemon: hassmic prints the service file for its protocol, name and MAC address.
    # The MAC in it is how Home Assistant tells devices apart.  On radar wlan0 appears only later in the boot, hassmic
    # printed its placeholder MAC, and Home Assistant offered the adopted Echo as a new device.  So wait for Wi-Fi
    # (there is no mDNS without it anyway); hassmic keeps going meanwhile.  The directory belongs to the daemon's user so
    # hassmic can rewrite the file itself when Home Assistant sets or clears the encryption key.
    # That makes every name in it the daemon's to point elsewhere: root writes the file in its own directory, reads the
    # node name there and renames it in (a rename replaces a link, it does not follow it).  The binary runs as the
    # daemon's user like below: it may be the test binary in /data, and -S only reads state/api_key and the MAC.
    AVAHI=${HASSMIC_AVAHI:-/data/misc/avahi}           # only tests/boot_test.sh sets it
    mkdir -p $AVAHI/services
    chown $DAEMON_USER $AVAHI/services
    (
        i=0
        while [ $i -lt 120 ] && ! grep -q '[1-9a-f]' /sys/class/net/$WLAN/address 2>/dev/null; do sleep 1; i=$((i + 1)); done
        [ $i -gt 0 ] && echo "mDNS: waited ${i}s for the $WLAN address"
        svc=$AVAHI/hassmic.service.new
        $D/runas $DAEMON_USER $DAEMON_GROUPS $BIN -P ${PROTO:-esphome} -n "$NAME" $ARGS -S > $svc
        chown $DAEMON_USER $svc; chmod 644 $svc
        node=$(sed -n 's|^ *<name>\([a-z0-9-]*\)</name>$|\1|p' $svc)
        # as in ota_watch: onto a directory (or a link to one) mv would move the file into it
        out=$AVAHI/services/hassmic.service
        if [ -L $out ] || { [ -e $out ] && [ ! -f $out ]; }; then rm -rf $out; fi
        mv -f $svc $out
        # The init-started avahi runs in its own SELinux domain, which may not read /data/misc/avahi/services (avc denied),
        # and magiskpolicy cannot parse a rule for a type with a hyphen ("avahi-daemon").  So run it from here, in our domain.
        # Host name = the ESPHome node name, as on a real ESPHome device.  Stock avahi calls every Echo "linux" (a second
        # one "linux-2"), and Home Assistant showed that next to the name.  [server] is the stock file's first section, so
        # host-name lands in it.
        conf=/system/etc/avahi-daemon.conf
        if [ -n "$node" ]; then
            { echo "[server]"; echo "host-name=$node"; grep -v '^\[server\]' $conf; } > $AVAHI/avahi-daemon.conf
            conf=$AVAHI/avahi-daemon.conf
        fi
        stop avahi-daemon; pkill avahi-daemon; sleep 1
        avahi-daemon -f $conf --no-drop-root > /dev/null 2>&1 &
    ) >> $LOG 2>&1 &
    # The bootstrap counted this start as an attempt; the self test passing counts as success (ota_watch).
    fast=0
    while :; do
        t0=$(cut -d. -f1 /proc/uptime)
        # AIPC refuses uid 0, so run as the stock Alexa client's user (device.conf).  Real group 3990, which no stock process
        # has: hassmic creates its outgoing sockets under it and lockdown.sh lets that reach any address, so replies and music
        # play from wherever Home Assistant points.  Not the effective group: the mixer only records for group aipc.
        $D/runas -r 3990 $DAEMON_USER $DAEMON_GROUPS \
            $BIN -P ${PROTO:-esphome} -n "$NAME" $ARGS >> $LOG 2>&1
        echo "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # An update whose daemon does not stay up is worse than no update: with hassmic down there is no push port either.
        # Five exits within 20 s each -> back to the factory copy right now, without waiting for three reboots.
        if [ $(( $(cut -d. -f1 /proc/uptime) - t0 )) -lt 20 ]; then fast=$((fast + 1)); else fast=0; fi
        if [ $fast -ge 5 ] && [ "$D" != "$SYS" ]; then
            echo "== update $(cat $D/VERSION 2>/dev/null) keeps exiting: running the factory copy" >> $LOG
            echo 3 > $OTA/tries
            kill $NETWATCH 2>/dev/null     # the factory copy starts its own: two would both reload the firewall
            exec sh $SYS/boot.sh satellite
        fi
        # ALEXA_PROP may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh services > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
