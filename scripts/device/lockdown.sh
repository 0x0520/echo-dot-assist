#!/system/bin/sh
# Egress lock: the Echo may talk to local addresses only, never to the internet (Amazon).  Local = private ranges
# (RFC 1918), link-local, multicast, DHCP broadcast; IPv6: link-local, unique local, link multicast.  Home Assistant, media
# URLs, other VLANs of the home network all stay reachable without naming a subnet.
# Run BEFORE the first Wi-Fi join, and at every boot.
#   lockdown.sh          apply once
#   lockdown.sh watch    apply, then re-assert every 5 s (stock firewall.sh flushes all rules at sys.boot_completed and
#                        inserts its own chains at position 1)
#   lockdown.sh services only stop the cloud daemons, leave the firewall to the watcher: two runs rebuilding the chain at
#                        the same time interleave (duplicate rules, or the DROP ahead of some of them)
#   lockdown.sh ota-only [watch]
#                        stock Alexa with internet, firmware updates still impossible (MODE=stock-online in hassmic.conf):
#                        no egress lock, nothing stopped except the updaters.  Those that run as UPDATE_UID (donut: otad,
#                        ace_otad) lose all network access; UPDATE_ONDEMAND (update_engine, root) is kept stopped.
# hassmic itself may connect anywhere: it creates its outgoing sockets with filesystem group NET_GID (runas -r in main.sh
# and run.sh, setfsgid in net.c; the owner match checks that group) and only fetches what Home
# Assistant (encrypted, paired connection) or Music Assistant send it.  That can be a public host name, a Tailscale address
# or a global IPv6 address when that is how Home Assistant is reached.  The lock is about Amazon's daemons; none of them
# has that group.
# DNS (port 53) is also allowed to the resolvers the network handed out, whatever their address: DHCP often names a public
# one next to the router (8.8.8.8), and a name that resolves publicly to a LAN address then still works.  Queries already
# leave through a local resolver anyway, so this opens nothing new.
# Firmware without ip6tables (biscuit's 6574.1 has none) cannot filter IPv6 at all: there IPv6 is switched off on every
# interface instead, so a router that hands out global IPv6 addresses cannot open a way around the lock.  hassmic only
# speaks IPv4, so nothing of ours is lost.
# Does not cover the seconds between Wi-Fi association at boot and this script: block the device's MAC at the router as
# well if "never" has to be strict.
LOCAL4="10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 169.254.0.0/16 224.0.0.0/4 255.255.255.255"
LOCAL6="fe80::/10 fc00::/7 ff02::/16"
NET_GID=3990
# Service names and the updaters' user: device.conf of this model, next to this script.  Without it the egress lock
# still goes up (it needs none of that); only stopping services and the stock-online guard have nothing to work with.
CONF="${0%/*}/device.conf"
if [ -f "$CONF" ]; then . "$CONF"; else echo "!! $CONF missing: services not stopped, stock-online guard unavailable"; fi

HAVE6=; command -v ip6tables > /dev/null && HAVE6=1
v6off() { for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do echo 1 > $f || echo "!! IPv6 NOT OFF ($f): take the Echo offline"; done; }

# DNS servers from DHCP (dhcp.<iface>.dnsN) and the system's own (net.dnsN), one per line, sorted
resolvers() { getprop | sed -nE 's/^\[(dhcp\.[^.]+|net)\.dns[0-9]+\]: \[([^]]+)\]$/\2/p' | sort -u; }

apply() {
    DNS=$(resolvers)
    iptables -w -N hassmic_out 2>/dev/null
    iptables -w -F hassmic_out
    iptables -w -A hassmic_out -o lo -j RETURN
    for d in $LOCAL4; do iptables -w -A hassmic_out -d $d -j RETURN; done
    for d in $DNS; do case $d in *:*) continue;; esac
        for p in udp tcp; do iptables -w -A hassmic_out -d $d -p $p --dport 53 -j RETURN; done; done
    iptables -w -A hassmic_out -m owner --gid-owner $NET_GID -j RETURN || echo "!! hassmic limited to local addresses (iptables)"
    iptables -w -A hassmic_out -j DROP
    while iptables -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
    iptables -w -I OUTPUT 1 -j hassmic_out
    [ -n "$HAVE6" ] || { v6off; return; }
    ip6tables -w -N hassmic_out 2>/dev/null
    ip6tables -w -F hassmic_out
    ip6tables -w -A hassmic_out -o lo -j RETURN
    for d in $LOCAL6; do ip6tables -w -A hassmic_out -d $d -j RETURN; done
    for d in $DNS; do case $d in *:*) ;; *) continue;; esac
        for p in udp tcp; do ip6tables -w -A hassmic_out -d $d -p $p --dport 53 -j RETURN; done; done
    ip6tables -w -A hassmic_out -m owner --gid-owner $NET_GID -j RETURN || echo "!! hassmic limited to local addresses (ip6tables)"
    ip6tables -w -A hassmic_out -j DROP
    while ip6tables -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
    ip6tables -w -I OUTPUT 1 -j hassmic_out
}

# Stock firewall.sh uses the owner match itself, so the kernel has it.  A rule that fails to load must not pass silently.
apply_ota() {
    [ -n "$HAVE6" ] || v6off
    for t in iptables ${HAVE6:+ip6tables}; do
        $t -w -N hassmic_out 2>/dev/null
        $t -w -F hassmic_out
        $t -w -A hassmic_out -o lo -j RETURN
        $t -w -A hassmic_out -m owner --uid-owner "$UPDATE_UID" -j DROP || echo "!! OTA GUARD NOT ACTIVE ($t): take the Echo offline"
        while $t -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
        $t -w -I OUTPUT 1 -j hassmic_out
    done
}
ota_off() { for s in $UPDATE_SERVICES $UPDATE_ONDEMAND; do stop $s 2>/dev/null; done; }

first() { iptables -w -S OUTPUT 2>/dev/null | grep -m1 '^-A' | grep -q hassmic_out; }

if [ "$1" = ota-only ]; then
    apply_ota; ota_off
    echo "stock-online: only the updaters are cut off"; iptables -w -S hassmic_out
    # update_engine is started on demand, and "start" undoes a "stop": keep at it.
    [ "$2" = watch ] && while sleep 5; do first || { apply_ota; echo "OTA guard re-applied"; }; ota_off; [ -n "$HAVE6" ] || v6off; done
    exit 0
fi

[ "$1" = services ] || apply
# Everything that phones home.  mixer, shmd, ledcontroller, acebuttond, netmgrd, wifisvc stay.
# perfmonitord stays too: every new micAsr stream makes the mixer connect to it over AIPC and wait up to 20 s for it
# before opening the mic, so without it each hassmic (re)start was 20 s deaf.  Anything it sends out is dropped by the egress lock.
for s in $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND $CLOUD_SERVICES; do stop $s 2>/dev/null; done
[ -n "$ALEXA_PROP" ] && setprop $ALEXA_PROP 0
[ "$1" = services ] && exit 0
echo "egress limited to local addresses$([ -n "$HAVE6" ] || echo ", IPv6 off (no ip6tables)")"; iptables -w -S hassmic_out

# Re-apply as well when the resolvers change (other network, new DHCP lease).
[ "$1" = watch ] && while sleep 5; do
    if ! first; then apply; echo "lockdown re-applied"
    elif [ "$(resolvers)" != "$DNS" ]; then apply; echo "lockdown re-applied, resolvers:" $DNS; fi
    [ -n "$HAVE6" ] || v6off                        # an interface that comes up later starts with IPv6 on
done
exit 0
