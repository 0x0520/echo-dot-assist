#!/bin/sh
# What the Echo's CPU and memory go to, live over adb (the same as Home Assistant's task manager entities, every 2 s
# instead of 10): CPU and memory of the whole Echo (/proc/stat over one second, /proc/meminfo), the busiest processes
# (toybox top, or ps where its top cannot run in batch mode), and hassmic's threads by name (each names itself as it
# starts) with their CPU over that second.  CPU shares are of all cores together, as in Home Assistant.  Needs only adb
# and awk, so it runs on Windows (Git Bash) too; Ctrl-C ends it.
#   scripts/top.sh [seconds]       refresh interval, 2 by default; ANDROID_SERIAL picks one of several Echos, as for adb
#   ONCE=1 scripts/top.sh          one look, no screen clearing (for a log)
set -e
cd "$(dirname "$0")/.."
SECS=${1:-2}
case "$SECS" in ''|*[!0-9]*|0) echo "usage: scripts/top.sh [seconds]" >&2; exit 2;; esac
command -v adb > /dev/null || { echo "adb not found" >&2; exit 1; }
[ "$(adb get-state 2>/dev/null)" = device ] || { echo "no Echo on adb (scripts/adb-wifi.sh opens it over Wi-Fi)" >&2; exit 1; }

# Runs on the Echo (mksh, toybox): two looks at the machine's jiffies and hassmic's threads one second apart, then
# memory, load and top.  read instead of cat: no process per thread.
# shellcheck disable=SC2016
DEV='P=$(pidof hassmic); P=${P%% *}
snap() { head -1 /proc/stat; [ -n "$P" ] || return 0; for t in /proc/$P/task/[0-9]*; do l=; { read -r l < $t/stat; } 2>/dev/null; echo "T $l"; done; }
echo "@1"; snap; sleep 1; echo "@2"; snap
echo "@M"; cat /proc/meminfo; echo "@L"; cat /proc/loadavg; echo "@P $P"
echo "@TOP"
o=$(top -b -n 1 -m 15 2>/dev/null); if [ -n "$o" ]; then echo "$o"
else echo "(no batch mode in this top: ps)"; ps -A -o PID,USER,PCPU,RSS,NAME 2>/dev/null || ps; fi'

render() {
    awk -v when="$(date +%H:%M:%S)" -v serial="${ANDROID_SERIAL:-adb}" '
    function total(l,   f, n, i, s) { n = split(l, f, " "); s = 0; for (i = 2; i <= 9 && i <= n; i++) s += f[i]; return s }
    # one thread line of /proc/<pid>/task/<tid>/stat: its name in parentheses (may hold spaces), then the fields
    function thread(l, k,   name, rest, f) {
        name = substr(l, index(l, "(") + 1); rest = name
        while (match(rest, /\) /)) { rest = substr(rest, RSTART + 2); }
        name = substr(name, 1, length(name) - length(rest) - 2)
        split(rest, f, " "); tid = substr(l, 3); sub(/ .*/, "", tid)
        names[tid] = name; ticks[k, tid] = f[12] + f[13]; seen[tid] = 1
    }
    /^@1$/ { sect = 1; next } /^@2$/ { sect = 2; next } /^@M$/ { sect = "m"; next } /^@L$/ { sect = "l"; next }
    /^@P/ { pid = $2; next } /^@TOP$/ { sect = "t"; next }
    sect == 1 && /^cpu / { t1 = total($0); next } sect == 2 && /^cpu / { t2 = total($0); next }
    (sect == 1 || sect == 2) && /^T [0-9]/ { thread($0, sect); next }
    sect == "m" { mem[$1] = $2; next }
    sect == "l" { load = $1 " " $2 " " $3; next }
    sect == "t" { top[++ntop] = $0; next }
    END {
        d = t2 - t1; if (d <= 0) d = 1
        avail = ("MemAvailable:" in mem) ? mem["MemAvailable:"] : mem["MemFree:"] + mem["Buffers:"] + mem["Cached:"]
        printf "%s  %s   load %s\n", when, serial, load
        if (mem["MemTotal:"]) printf "memory %.1f %% used, %d MB available of %d MB\n\n", 100 * (mem["MemTotal:"] - avail) / mem["MemTotal:"], avail / 1024, mem["MemTotal:"] / 1024
        for (i = 1; i <= ntop; i++) print top[i]
        if (pid == "") { print "\nhassmic is not running"; exit }
        n = 0; for (t in seen) if ((1, t) in ticks && (2, t) in ticks) { n++; id[n] = t; c[n] = 100 * (ticks[2, t] - ticks[1, t]) / d }
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (c[j] > c[i]) { x = c[i]; c[i] = c[j]; c[j] = x; x = id[i]; id[i] = id[j]; id[j] = x }
        printf "\nhassmic (pid %s), %d threads, CPU over 1 s (all cores = 100 %%):\n", pid, n
        for (i = 1; i <= n; i++) printf "  %6s  %-16s %5.1f %%\n", id[i], names[id[i]], c[i]
    }'
}

while :; do
    out=$(adb shell "$DEV" | tr -d '\r')
    if [ -n "$ONCE" ]; then echo "$out" | render; exit 0; fi
    screen=$(echo "$out" | render)
    printf '\033[H\033[2J%s\n' "$screen"
    [ "$SECS" -gt 1 ] && sleep $((SECS - 1))
done
