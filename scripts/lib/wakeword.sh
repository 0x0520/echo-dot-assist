# Wake word models for an installed Echo, shared by scripts/wakeword.sh and the last step of scripts/setup.sh.  bash;
# sourced from the repository root after scripts/lib/device.sh and scripts/lib/setup.sh.
# The models are Amazon's (DAVS) and the same for every Echo, so ones fetched before (device-logs/models/, git-ignored)
# are only copied over.  Each is first loaded by the Echo's own engine (pryon_test): an older engine (radar) cannot load
# every set.  A model not there yet is fetched from Amazon, which needs the Echo registered to an Amazon account once:
# it runs stock Alexa with the updaters cut off (MODE=stock-online) until then, and everything is undone afterwards
# (registration, the Wi-Fi the Alexa app added, the mode).  Home Assistant then offers every installed model in the
# Echo's wake word select.  Stopped halfway, a new run finds the Echo in stock-online mode and goes on there.

MODELS=device-logs/models
D=/data/local/hassmic
MAPDB=/data/ace/kvstorage/map.db
WW_LOCAL=(connect "Connect" choose "Choose" install "Install")
WW_AMAZON=(connect "Connect" choose "Choose" online "Online with Amazon" register "Register in the Alexa app"
           fetch "Download from Amazon" install "Install" back "Back to satellite")

# ww_stage ID: on its own, a screen per step of WW_PLAN; inside the setup (WW_SETUP set) only a line
ww_stage() {
    local i list=() cur=0
    if [ -n "$WW_SETUP" ]; then
        for ((i = 0; i < ${#WW_PLAN[@]}; i += 2)); do
            [ "${WW_PLAN[i]}" = "$1" ] && [ "$1" != connect ] && [ "$1" != choose ] && printf '\n  %s%s%s\n' "$B" "${WW_PLAN[i+1]}" "$N"
        done
        return 0
    fi
    DONE_IDS=
    for ((i = 0; i < ${#WW_PLAN[@]}; i += 2)); do
        list+=("${WW_PLAN[i]}|${WW_PLAN[i+1]}"); [ "${WW_PLAN[i]}" = "$1" ] && cur=${#list[@]}
        [ $cur = 0 ] && DONE_IDS="$DONE_IDS ${WW_PLAN[i]}"
    done
    [ "$1" = done ] && cur=$((${#list[@]} + 1))
    header $cur ${#list[@]} "${list[@]}"
}

# model id "echo-de-DE" -> "Echo (de-DE)"
label() { local k=${1%%-*}; printf '%s (%s)' "${k^}" "${1#*-}"; }
wpa() { ashell "wpa_cli -i $WLAN -p $WPA_SOCKETS $*"; }
net_ids() { wpa list_networks | awk 'NR > 1 { print $1 }' | tr '\n' ' '; }
token() {                              # the registered Echo's access token, pulled into $TMP/map.db
    rm -f $TMP/map.db*
    for f in $(ashell "ls $MAPDB*" 2>/dev/null); do adb pull "$f" $TMP/ > /dev/null 2>&1; done
    [ -n "$(sqlite3 $TMP/map.db "select value from deviceData where key='access_token'" 2>/dev/null)" ]
}
satellite_up() { waitfor "Waiting for the satellite|Satellite running" '[ -n "$(ashell pidof hassmic)" ]' "" 60 180; }

# install_model ID: load it with the Echo's engine first; only a set that loads goes into $D/models (hidden until then:
# hassmic skips names starting with a dot)
install_model() {
    local id=$1 t=$D/models/.try-$1
    task "Copying $(label $id)" sh -c "adb shell 'rm -rf $t; mkdir -p $D/models' && adb push $MODELS/$id/unpacked $t && adb shell chmod -R a+rX $t" || return 1
    # pryon_test: 1 = the set did not load, 3 = loaded but heard nothing (there is no audio)
    if ! task "Trying it on this Echo's engine" sh -c "adb shell '$PT -m $t/pryon.manifest /dev/null > /dev/null 2>&1; echo rc=\$?' | tee /dev/stderr | grep -qv rc=1"; then
        ashell "rm -rf $t"
        fail "$(label $id) does not work with this Echo's wake word engine"
        return 1
    fi
    ashell "rm -rf $D/models/$id; mv $t $D/models/$id"
    INSTALLED+=("$id")
}

# wakeword_run [setup]: the whole thing; with "setup" as a step of scripts/setup.sh (the Echo on adb is the one just
# installed, and the first choice keeps "Alexa")
wakeword_run() {
    local r
    WW_SETUP=$1 WW_PLAN=("${WW_LOCAL[@]}") INSTALLED=()
    if [ -n "$DRY" ]; then info "offers the models in $MODELS/ and one from Amazon; the dry run keeps \"Alexa\""; return 0; fi
    TMP=$(mktemp -d)                  # map.db is the account's device credential: never kept on the PC
    _wakeword_run; r=$?
    rm -rf "$TMP"
    return $r
}

_wakeword_run() {
    # --- connect
    ww_stage connect
    if [ -z "$WW_SETUP" ]; then
        pick_serial
        waitfor "Waiting for the Echo on adb|Echo on adb" "adb_is device" \
            "Nothing? Connect it by USB, or give its address: scripts/wakeword.sh <echo-ip>" 15 || return 1
        device_load adb
        MODEL_NAME="Wake word · $MODEL_NAME"
    else
        wait_adb device || return 1
    fi
    [ "$(ashell id -u)" = 0 ] || { fail "adb shell is not root: is this Echo set up with scripts/setup.sh?"; return 1; }
    [ -n "$(ashell "ls $D/hassmic.conf 2>/dev/null")" ] ||
        { fail "no hassmic installed on this Echo: scripts/setup.sh first"; return 1; }
    PT=
    for p in $D/pryon_test /system/hassmic/pryon_test; do [ -n "$(ashell "ls $p 2>/dev/null")" ] && { PT=$p; break; }; done
    if [ -z "$PT" ]; then
        [ -f build/$DEVICE/pryon_test ] || { fail "no pryon_test on the Echo or in build/$DEVICE: make DEVICE=$DEVICE"; return 1; }
        adb push build/$DEVICE/pryon_test /data/local/tmp/ > /dev/null && PT=/data/local/tmp/pryon_test
    fi
    ECIDS=$(ashell "$PT -m /nonexistent /dev/null 2>&1" | grep -o '"wakeword_ecids":\[[0-9,]*\]' | grep -o '[0-9][0-9,]*')
    NETS=build/wakeword-$(adb get-serialno | tr -c 'A-Za-z0-9\n' _).nets    # Wi-Fi networks before the Alexa app
    ONLINE=; [ -n "$(ashell "grep '^MODE=stock-online' $D/hassmic.conf")" ] && ONLINE=1
    HAVE=" $(ashell "ls $D/models 2>/dev/null" | tr '\n' ' ') "
    [ -n "$WW_SETUP" ] || ok "$MODEL_NAME${ANDROID_SERIAL:+, $ANDROID_SERIAL}"
    [ -n "$ONLINE" ] && warn "This Echo is in stock-online mode from an earlier run: going on with that."

    # --- choose
    ww_stage choose
    ids=() items=()
    [ -n "$WW_SETUP" ] && { ids+=(KEEP); items+=("Keep \"Alexa\" $DIM(more later: scripts/wakeword.sh)$N"); }
    for m in $MODELS/*/unpacked/pryon.manifest; do
        [ -f "$m" ] || continue
        id=${m#$MODELS/}; id=${id%%/*}
        # installed under its own name, or by hand under the short one (echo-de)
        [[ $HAVE == *" $id "* || $HAVE == *" ${id%-*} "* ]] && continue
        ids+=("$id"); items+=("$(label $id)")
    done
    n_local=0; for id in "${ids[@]}"; do [[ $id == *-* ]] && n_local=$((n_local + 1)); done
    [ $n_local -gt 1 ] && { ids+=(ALL); items+=("All of the above"); }
    ids+=(AMAZON); items+=("Another one, from Amazon ${DIM}(needs an Amazon account once)$N")
    [ -n "$ONLINE" ] && { ids=(AMAZON); items=("Another one, from Amazon"); }
    [ -n "${HAVE// }" ] && info "On this Echo already:$HAVE"
    say "Which wake word?"; printf '\n'
    menu c "${items[@]}"
    PICK=${ids[c]}
    [ "$PICK" = KEEP ] && { ok "wake word: Alexa"; return 0; }

    if [ "$PICK" = AMAZON ]; then
        WW_PLAN=("${WW_AMAZON[@]}")
        ww_stage choose
        need_tools python3 sqlite3 || return 1
        keys=(echo computer amazon ziggy alexa)
        say "Wake word:"; printf '\n'; menu c Echo Computer Amazon Ziggy Alexa; KEY=${keys[c]}
        locs=(de-DE en-US en-GB fr-FR it-IT es-ES ja-JP pt-BR en-CA fr-CA en-AU en-IN es-MX)
        def=0; for i in "${!locs[@]}"; do [ "${locs[i]//-/_}" = "${LANG%%.*}" ] && def=$i; done    # the PC's language first
        printf '\n'; say "Language:"; printf '\n'; MENU_SEL=$def menu c "${locs[@]}"; LOC=${locs[c]}
        ID=$KEY-$LOC

        # --- online: stock Alexa with internet, updaters cut off
        ww_stage online
        if [ -z "$ONLINE" ]; then
            # the Alexa app adds its own Wi-Fi network; the ones there now are kept, the rest is removed at the end
            net_ids > $NETS
            ashell "sed -i '/^MODE=/d' $D/hassmic.conf; echo MODE=stock-online >> $D/hassmic.conf"
            task "Restarting the Echo as a stock Echo" adb reboot || return 1
            sleep 10
        fi
        wait_adb device || return 1
        # a firmware update would cost the unlock: nothing goes further without the guard in place
        waitfor "Waiting for the update block|Firmware updates blocked" \
            "ashell iptables -S hassmic_out | grep -q 'uid-owner.*-j DROP'" "" 60 120 ||
            { fail "the update block is not in place: unplug the Echo's power and run this again"; return 1; }
        tell "Give the Echo internet access" "If your router blocks it, allow it until this is done."

        # --- register
        ww_stage register
        if ! token; then
            tell "Set the Echo up in the Alexa app" \
                "Devices → + → Add device → Amazon Echo, on the Wi-Fi Home Assistant is on." \
                "The app may show \"updating\" for a while: that is the blocked update check, it is fine."
            waitfor "Waiting for the registration|Registered" token || return 1
        else ok "registered already"; fi

        # --- fetch
        ww_stage fetch
        task "Downloading $(label $ID)" python3 tools/davs-fetch.py ${ECIDS:+--ecids $ECIDS} $TMP/map.db $KEY $LOC $MODELS || {
            info "The token may have expired: wait a minute (the Echo renews it) and run this again."; return 1; }
        rm -f $TMP/map.db*
        ww_stage install
        install_model $ID

        # --- back: deregister, forget the app's Wi-Fi and the registration, satellite mode
        ww_stage back
        todo "Remove the Echo from your Amazon account" "Alexa app → Devices → this Echo → ⚙ → Deregister"
        keep=" $(cat $NETS 2>/dev/null) " drop=
        if [ "$keep" != "  " ]; then
            for n in $(net_ids); do [[ $keep == *" $n "* ]] || drop="$drop $n"; done
            # in one go: the Echo may be on the app's network right now, and adb over Wi-Fi goes with it
            [ -n "$drop" ] && task "Removing the Wi-Fi the Alexa app added" adb shell "for n in$drop; do
                wpa_cli -i $WLAN -p $WPA_SOCKETS remove_network \$n; done; wpa_cli -i $WLAN -p $WPA_SOCKETS save_config" &&
                { wait_adb device || return 1; }
        fi
        task "Clearing the registration" sh -c "adb pull $MAPDB $TMP/map.db && sqlite3 $TMP/map.db 'delete from deviceData; vacuum;' &&
            adb push $TMP/map.db $MAPDB && adb shell 'rm -f $MAPDB-wal $MAPDB-shm; chown ace_maplite:ace_maplite $MAPDB; chmod 660 $MAPDB'" || return 1
        rm -f $TMP/map.db* $NETS
        ashell "sed -i '/^MODE=/d' $D/hassmic.conf"
        task "Restarting the Echo as a satellite" adb reboot || return 1
        sleep 10
        wait_adb device && satellite_up || return 1
        info "The Echo can lose its internet access at the router again."
    else
        ww_stage install
        todo_ids=("$PICK")
        [ "$PICK" = ALL ] && { todo_ids=(); for id in "${ids[@]}"; do [[ $id == *-* ]] && todo_ids+=("$id"); done; }
        for id in "${todo_ids[@]}"; do install_model $id; done
        [ ${#INSTALLED[@]} -gt 0 ] || return 1
        # main.sh starts a hassmic that exits again; the new one scans $D/models
        task "Restarting hassmic" adb shell 'kill $(pidof hassmic)' && sleep 3 && satellite_up || return 1
    fi

    [ ${#INSTALLED[@]} -gt 0 ] || return 1
    ww_stage done
    names=(); for id in "${INSTALLED[@]}"; do names+=("$(label $id)"); done
    ok "installed: ${names[*]}"
    tell "Pick it in Home Assistant" "Settings → Devices & services → this Echo → Wake word"
    printf '\n'
}
