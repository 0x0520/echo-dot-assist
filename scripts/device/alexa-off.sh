#!/system/bin/sh
# Stop the Alexa client and OTA until next reboot.  Leaves mixer, shmd, ledcontroller, Wi-Fi untouched.
#   alexa-off.sh            by hand.  After alexa-on.sh (hassmic.alexa=1) also the satellite back: both init services
#                           started anew, the egress lock first (main.sh firewall), then the satellite (main.sh satellite)
#   alexa-off.sh services   what main.sh and run.sh run before every start of hassmic: only stop Amazon's services.  Never
#                           undoes alexa-on.sh: between its setprop and its stop of the satellite this may still run.
# Which services that are: device.conf of this model, next to this script.
. "${0%/*}/device.conf" || exit 1
BACK=; [ "$1" != services ] && [ "$(getprop hassmic.alexa)" = 1 ] && BACK=1
[ -z "$BACK" ] || setprop hassmic.alexa 0
# ledcontroller sets ALEXA_PROP=1 after the boot animation, which starts the Alexa services.
setprop $ALEXA_PROP 0
for s in $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND; do stop $s; done
# Amazon's Bluetooth stack (speaker mode, pairing through the Alexa app).  hassmic drives the radio itself, for Home
# Assistant's Bluetooth proxy and as a Bluetooth speaker, and /dev/stpbt does not keep a second user out: it has to go.
stop $BT_SERVICE
# Its AIPC service directory outlives it and stays its own (0710): hassmic answers the mixer on that service in its place
# when it plays to a Bluetooth speaker (btout.c) and could neither remove nor reuse it.
i=0; while [ "$(getprop init.svc.$BT_SERVICE)" = running ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
rm -rf /dev/aipc/0
# Setup mode (unregistered device): oobed advertises for the Alexa app and keeps the orange setup animation on the ring.
for s in $SETUP_SERVICES; do stop $s; done
# uxeventd starts the orange `setup-mode` spinner; `ledctrl -c` does not clear it and `-g` does not list it. Unset by name.
ledctrl -c >/dev/null; ledctrl -u setup-mode >/dev/null
# oobed leaves a Wi-Fi Direct group up for the Alexa app (p2p-p2p0-0 + dnsmasq).  wpa_supplicant itself must stay: it also runs wlan0.
wpa_cli -p $WPA_SOCKETS -i p2p0 p2p_group_remove p2p-p2p0-0 >/dev/null 2>&1
sleep 1
if [ -n "$BACK" ]; then
    # lockdown.sh watch stops the cloud daemons again and puts the egress lock back in place of the update guard
    stop hassmic_fw; start hassmic_fw
    start hassmic
    echo "alexa-off: satellite back, egress lock restored"
fi
getprop | grep -E "init.svc.($(echo $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND mixer shmd ledcontroller | tr ' ' '|'))\]"
