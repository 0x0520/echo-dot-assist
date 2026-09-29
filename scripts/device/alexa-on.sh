#!/system/bin/sh
# Revert alexa-off.sh without rebooting.
. "${0%/*}/device.conf" || exit 1
pkill hassmic; pkill mixcap; pkill mixplay
rm -f /data/misc/avahi/services/hassmic.service
ledctrl -c
for s in $UPDATE_SERVICES $BT_SERVICE; do start $s; done
setprop $ALEXA_PROP 1      # init trigger starts the Alexa services ($ALEXA_SERVICES)
