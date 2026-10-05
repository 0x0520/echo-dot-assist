#!/system/bin/sh
# Join Wi-Fi without the Alexa app, through the stock wpa_supplicant.  Run lockdown.sh first.
#   wifi-join.sh "<ssid>" "<passphrase>"
#   wifi-join.sh <file>                   line 1 = SSID, line 2 = passphrase (keeps the secret out of shell history;
#                                         scripts/wifi-join.sh on the PC pushes secrets/wifi.conf and deletes it after)
#   wifi-join.sh -x <file>                the same with the SSID in hex: any bytes (Wi-Fi setup over Bluetooth, which
#                                         main.sh wifi_watch runs as root for hassmic)
# The passphrase: 8..63 printable characters, 64 hex digits (the key itself), or empty in a file for an open network.
# Profile is saved to /data/misc/wifi/wpa_supplicant.conf (update_config=1), so it survives reboots.
# Exit 0 once the link is up with an address (the last line says which); otherwise 1, and the new profile is removed
# again with the others enabled as before: a wrong passphrase must not cost the Echo the network it had.
# Nothing read here is ever evaluated: SSID and passphrase go to wpa_cli as single arguments.
# The join itself is UNTESTED on device (tests/boot_test.sh runs it against a stand-in wpa_cli).  If wpa_cli cannot
# connect, wpa_supplicant is not running yet: it is started.
if [ -f "${0%/*}/device.conf" ]; then . "${0%/*}/device.conf"; fi
WLAN=${WLAN:-wlan0}
HEX=; [ "$1" = -x ] && { HEX=1; shift; }
if [ -f "$1" ]; then { IFS= read -r SSID; IFS= read -r PSK; } < "$1"
else SSID=${1:?usage: wifi-join.sh <ssid> <passphrase> | [-x] <file>}; PSK=${2:?passphrase missing}; fi
W="wpa_cli -i $WLAN -p ${WPA_SOCKETS:-/data/misc/wifi/sockets}"
iptables -S OUTPUT | grep -q hassmic_out || { echo "refusing: run lockdown.sh first"; exit 1; }
if [ -n "$HEX" ]; then
    case "$SSID" in ''|*[!0-9a-f]*) echo "not joined: SSID not in hex"; exit 1;; esac
    X=$SSID
else
    X=$(printf %s "$SSID" | od -An -tx1 | tr -d " \n")      # hex form: apostrophes and spaces are safe
fi
case ${#PSK} in
0) ;;
64) case "$PSK" in *[!0-9a-fA-F]*) echo "not joined: a 64-character key must be hex"; exit 1;; esac;;
*) [ ${#PSK} -ge 8 ] && [ ${#PSK} -le 63 ] || { echo "not joined: a passphrase has 8 to 63 characters"; exit 1; };;
esac
$W status >/dev/null 2>&1 || { start wpa_supplicant; sleep 3; }
N=$($W add_network | tail -1)
case "$N" in ''|*[!0-9]*) echo "not joined: wpa_supplicant does not answer"; exit 1;; esac
$W set_network "$N" ssid "$X" >/dev/null
case ${#PSK} in
0) $W set_network "$N" key_mgmt NONE >/dev/null;;
64) $W set_network "$N" psk "$PSK" >/dev/null; $W set_network "$N" key_mgmt WPA-PSK >/dev/null;;
*) $W set_network "$N" psk "\"$PSK\"" >/dev/null; $W set_network "$N" key_mgmt WPA-PSK >/dev/null;;
esac
$W enable_network "$N" >/dev/null
$W select_network "$N" >/dev/null
# Up to WIFI_JOIN_SECS for the association and an address.  DHCP is normally started by Amazon's netmgrd once the link
# is up; 8 s after association without one, the init service is started as a fallback.
i=0; assoc=0; state=
while [ $i -lt "${WIFI_JOIN_SECS:-30}" ]; do
    sleep 1; i=$((i + 1))
    state=$($W status 2>/dev/null | sed -n 's/^wpa_state=//p')
    [ "$state" = COMPLETED ] || continue
    assoc=$((assoc + 1))
    addr=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    if [ -n "$addr" ]; then
        $W enable_network all >/dev/null        # select_network disabled the others: kept as fallbacks, not switched off
        $W save_config >/dev/null
        echo "joined, address $addr"
        exit 0
    fi
    [ $assoc = 8 ] && start dhcpcd-$WLAN
done
$W remove_network "$N" >/dev/null
$W enable_network all >/dev/null
$W save_config >/dev/null
echo "not joined: ${state:-no answer from wpa_supplicant} after ${i}s (wrong passphrase, or out of reach)"
exit 1
