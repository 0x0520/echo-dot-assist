#!/bin/sh
# Push update path on the PC: real hassmic-host receiver, real otatool, and this script in the role of the root-side
# installer loop of main.sh (same commands, temp directories).
cd "$(dirname "$0")/.."; T=$(mktemp -d); O=build/otatool-host; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
$O keygen $T/k.sec $T/k.pub; $O keygen $T/evil.sec $T/evil.pub
mkdir $T/state
HASSMIC_STATE=$T/state HASSMIC_ADB_OPEN=$T/adb-open HASSMIC_UPDATE_PUB=$T/k.pub HASSMIC_SETTINGS=$T/settings build/hassmic-host -p 16971 -z 0 -o 16972 -L 2>$T/log &
PID=$!; sleep 0.7
installer() {        # what ota_watch in main.sh does, once
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    [ -f $T/state/ota/request ] || return
    rm -f $T/state/ota/request
    if v=$($O install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1); then echo "OK $v" > $T/state/ota/result; else echo "FAILED $v" > $T/state/ota/result; fi
}
printf '#!/bin/sh\necho main\n' > $T/main.sh; cp build/hassmic-host $T/hassmic
$O pack $T/k.sec 9.9.9+test $T/good.bundle $T/main.sh $T/hassmic >/dev/null
installer & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ] && [ "$(cat $T/installed/VERSION)" = 9.9.9+test ]; ok $? "signed bundle is received, installed and reported: $r"
$O pack $T/evil.sec 6.6.6 $T/evil.bundle $T/main.sh >/dev/null
r=$($O push 127.0.0.1 16972 $T/evil.bundle $T/evil.bundle.sig); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/request ]; ok $? "bundle from another key is refused before it reaches the installer: $r"
cp $T/good.bundle $T/bad.bundle; printf X | dd of=$T/bad.bundle bs=1 seek=30 conv=notrunc 2>/dev/null
r=$($O push 127.0.0.1 16972 $T/bad.bundle $T/good.bundle.sig); echo "$r" | grep -q "^FAILED signature"; ok $? "tampered bundle is refused: $r"
r=$(printf 'GET / HTTP/1.0\r\n\r\n' | timeout 3 nc -q1 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "garbage on the port is refused: $r"

# adb over Wi-Fi with the update key.  This script is the firewall watcher: it takes the request and answers with adb-open.
watcher() {
    for i in $(seq 1 40); do [ -f $T/state/adb-request ] && break; sleep 0.25; done
    [ "$(cat $T/state/adb-request 2>/dev/null)" = 1 ] && : > $T/adb-open; rm -f $T/state/adb-request
}
watcher & r=$($O adb 127.0.0.1 16972 $T/k.sec); wait $!
[ "$r" = "OK adb over Wi-Fi open for 30 min" ] && [ -f $T/adb-open ]; ok $? "update key opens adb over Wi-Fi: $r"
rm -f $T/adb-open
r=$($O adb 127.0.0.1 16972 $T/evil.sec); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/adb-request ]; ok $? "another key does not, and asks for nothing: $r"
# a recorded answer does not work again: the challenge differs each time
n1=$(printf 'HMOTA-ADB1\n' | timeout 3 nc -q2 127.0.0.1 16972 | head -1); n2=$(printf 'HMOTA-ADB1\n' | timeout 3 nc -q2 127.0.0.1 16972 | head -1)
echo "$n1" | grep -qE '^NONCE [0-9a-f]{64}$' && [ "$n1" != "$n2" ]; ok $? "a new challenge every time"
# Approving an installed update as the factory copy.  This script is the installer loop: it takes the version asked for.
approver() {
    for i in $(seq 1 40); do [ -f $T/state/ota/factory ] && break; sleep 0.25; done
    [ -f $T/state/ota/factory ] || return
    echo "OK $(cat $T/state/ota/factory) is now the factory copy" > $T/state/ota/factory-result; rm -f $T/state/ota/factory
}
approver & r=$($O factory 127.0.0.1 16972 $T/k.sec 9.9.9+test); wait $!
[ "$r" = "OK 9.9.9+test is now the factory copy" ]; ok $? "update key approves a version, the installer gets exactly that one: $r"
r=$($O factory 127.0.0.1 16972 $T/evil.sec 9.9.9+test); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/factory ]; ok $? "another key does not, and asks for nothing: $r"
r=$(printf 'HMOTA-FACTORY1 1;reboot\n' | timeout 3 nc -q1 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "a version with shell characters is refused: $r"
# a wrong signature approves nothing
r=$(python3 - "$T/k.sec" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", 16972)); s.sendall(b"HMOTA-FACTORY1 9.9.9+test\n")
line = s.makefile("rb").readline(); s.sendall(b"\0" * 64); print(s.makefile("rb").readline().decode().strip())
PY
); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/factory ]; ok $? "a wrong signature approves nothing: $r"
kill $PID; rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
