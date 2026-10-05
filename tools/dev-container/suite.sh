#!/bin/bash
# What CI runs (build.yml: build, test, lint), one line per step; run inside the dev container (run.sh).
cd /src
rm -rf build
fail=0
step() {
    local n=$1; shift; local t0=$(date +%s)
    if "$@" > /tmp/step.log 2>&1; then echo "ok   $n ($(( $(date +%s) - t0 ))s)"; else echo "FAIL $n"; tail -25 /tmp/step.log; fail=1; fi
}
for d in donut biscuit radar; do step "build $d" make -j8 DEVICE=$d STUBS=1 all; done
step "host builds" make -j8 build/hassmic-host build/otatool-host
step unit make unit
step lint make lint
step otatool_test sh tests/otatool_test.sh
step ota_push_test sh tests/ota_push_test.sh
step boot_test sh tests/boot_test.sh
step alexa_test sh tests/alexa_test.sh
for t in fake_ha_esphome fake_ha fake_ha_arbitration fake_ha_update fake_ma_sendspin; do
    step $t timeout 900 .venv/bin/python tests/$t.py
done
exit $fail
