# Development

For users: [README.md](README.md). Progress, open issues and every measurement: [PLAN.md](PLAN.md). User-visible
changes by date: [CHANGELOG.md](CHANGELOG.md). Reverse-engineering notes: [docs/](docs/).

## How it works

Amazon's `mixer` daemon owns the audio hardware and runs the whole front end (`libasp`: AEC, beamforming, mic
calibration). `hassmic` takes the place of the Alexa client `PuffinApp` as the mixer's client through the reversed C API
of `libmixerAPI.so`, feeds the 16 kHz post-AEC stream to the stock `libpryon.so` wake word engine, and speaks the ESPHome
native API (default) or Wyoming to Home Assistant.

Boot integration (`scripts/system/`): `hassmic.rc` (init) starts `boot.sh` (fixed, on `/system`), which picks the factory
copy or a verified push update and runs `main.sh` (updatable): `main.sh firewall` keeps the egress lock in place and is
the root side of push updates, `main.sh satellite` stops Alexa, updater and telemetry and keeps hassmic running.
No `hassmic.conf` = stock behaviour. `sysinstall.sh` writes the system partition from the running OS (`otatool remount`;
dm-verity is off): for `install-system.sh`, and for a push update the owner tried and approved (`ota-push.sh` asks;
`HMOTA-FACTORY1` in `ota.c`), which renews the factory copy and the bootstrap (`boot.sh`, `otatool`, `hassmic.rc`) over Wi-Fi.

Everything that differs between Echo models lives in `devices/<codename>/`: build settings, hardware constants linked
into the daemon (`board.c`), service names and install method for the scripts (`device.conf`), init rc and SELinux
rules. `DEVICE` picks one (default `donut`). The adb scripts check it against the connected Echo. Porting guide:
[devices/README.md](devices/README.md).

## Repository layout

| Path | What |
|---|---|
| `devices/` | one directory per Echo model (`donut`, `biscuit`, `radar`): `device.mk`, `board.c`, `device.conf`, `hassmic.rc`, `sepolicy.rules`, `setup.sh`, install instructions |
| `src/hassmic/` | the daemon: core (capture, wake word, playback, LEDs, buttons), `sound_pryon.c` (optional sound detection, `docs/re-aed.md`), `proto_esphome.c`, `proto_wyoming.c`, `arb.c` (wake word arbitration between Echos), `micdenoise.c` (RNNoise on the mic audio sent to the pipeline), `micgain.c` (gain of the mic audio sent to the pipeline), `sendspin.c`, `a2dp.c`, `ble.c`, push updates, `adbwifi.c` (the debug access switch) |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `aed_test` (stock sound detector, `docs/re-aed.md`), `latency`, `otatool`, `runas` (AIPC refuses uid 0, the image has no `su`), `curlspy`, `hciscan` (raw HCI on `/dev/stpbt`) |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `src/third_party/` | monocypher 4.0.2, `dr_flac.h`, `minimp3.h`, libfreeaptx 0.2.2, RNNoise 0.1.1 (own licences, see README) |
| `scripts/` | PC side: `setup.sh` (guided install), `artifacts.sh` (Amazon artifacts for an installed Echo: more wake words, the sound detection model), `deploy.sh`, `probe.sh`, `capture-test.sh`, `mic-compare.sh` (micRaw against micAsr on a running Echo), `wifi-join.sh`, `install-system.sh`, `ota-push.sh`, `adb-wifi.sh` (adb over Wi-Fi with the update key); `lib/device.sh` picks the model, `lib/setup.sh` has the guided setup's helpers, `lib/wakeword.sh` the wake word installer |
| `scripts/device/`, `scripts/system/` | run on the Echo, reading the model's `device.conf` next to them; boot integration (`boot.sh`, `main.sh`) |
| `tools/` | OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm), `davs-fetch.py` |
| `tests/` | protocol tests against the reference implementations |

## Build and test

Device binaries need the NDK and the unpacked firmware in `firmware/<codename>/` (the model's install page, steps 1–2, e.g. `devices/donut/README.md`). Without
them only the host targets build.

```sh
make [DEVICE=donut]                               # ARM binaries into build/donut/
make host                                         # PC build (build/hassmic-host) + qemu build for the tests (needs libopus)
make unit                                         # C unit tests
.venv/bin/python tests/fake_ha_esphome.py         # ESPHome native API, as Home Assistant (aioesphomeapi)
.venv/bin/python tests/fake_ha_arbitration.py     # two Echos + Home Assistant + an unknown device: wake word arbitration
.venv/bin/python tests/fake_ha.py [--qemu]        # Wyoming (wyoming)
.venv/bin/python tests/fake_ma_sendspin.py        # Sendspin, as Music Assistant (aiosendspin)
tests/ota_push_test.sh                            # signed push-update path end to end
```

`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against
`firmware/$DEVICE/rootfs`.

Trial runs on the device: `scripts/deploy.sh`, then `adb shell sh /data/local/hassmic/run.sh`.

## Debugging on the device

- Log: `/data/local/hassmic/boot.log`. State (API key, Bluetooth keys, Sendspin, settings): `/data/local/hassmic/state/`.
- What the wake word hears: `kill -TTIN $(pidof hassmic)` starts writing the processed mic stream to
  `/data/local/hassmic/state/capture.raw` (16 kHz mono s16le); the same signal stops it. `mixcap` cannot read that
  stream while hassmic runs: the mixer feeds it to one client only.

## Wake word download without the tool

`GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=` + URL-quoted base64 of
`{"artifactType":"wakeword","artifactKey":"echo","filters":{"engineCompatibilityIdList":[…],"locale":["de-DE"],"modelClass":["B"]}}`,
header `Authorization: Bearer <access_token>`. The answer is JSON with a signed CloudFront `downloadUrl` (`.tar.gz`) that
expires within minutes. Found by preloading `src/tools/curlspy.c` into the stock downloader: `scripts/device/davs-spy.sh`.

## Contributing

- Each model is supported on exactly one firmware (`FIRMWARE_ID` in its `device.conf`; `donut`: 6574.1). `scripts/probe.sh`
  checks the device matches, and `install-system.sh` refuses another one.
- No model `#ifdef`s in shared code: a new difference becomes a `struct board` field, a `device.conf` variable or a
  backend (see [devices/README.md](devices/README.md)).
- Feature commits update [CHANGELOG.md](CHANGELOG.md) (user-facing, dated, plain language), [PLAN.md](PLAN.md) (status,
  measurements) and the README when behaviour visible to users changes.
- Code comments explain *why*, with device facts and measurements.
- Every listening port must stay in TCP 16384–32767: the Echo's firewall admits nothing else inbound.
- Proprietary, derived or secret material stays out of git (git-ignored): `firmware/` (per model: stock
  image, unpacked rootfs, unlock zips, `re/` disassembly), `toolchain/`, `build/`, `device-logs/`, `secrets/` (`wifi.conf`, `update.key`), `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence.
