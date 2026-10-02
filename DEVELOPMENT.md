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
dm-verity is off): for `install-system.sh`, and for every update (pushed or online) once it passed its self test
(`main.c`: wake word engine loaded, ports bound, a second of mic audio; `ota_healthy()` leaves `state/ota/healthy`, root's
`ota_watch` promotes), which renews the factory copy and the bootstrap (`boot.sh`, `otatool`, `hassmic.rc`) over Wi-Fi.

Everything that differs between Echo models lives in `devices/<codename>/`: build settings, hardware constants linked
into the daemon (`board.c`), service names and install method for the scripts (`device.conf`), init rc and SELinux
rules. `DEVICE` picks one (default `donut`). The adb scripts check it against the connected Echo. Porting guide:
[devices/README.md](devices/README.md).

## Repository layout

| Path | What |
|---|---|
| `devices/` | one directory per Echo model (`donut`, `biscuit`, `radar`): `device.mk`, `board.c`, `device.conf`, `hassmic.rc`, `sepolicy.rules`, `setup.sh`, install instructions |
| `src/hassmic/` | the daemon: core (capture, wake word, playback, LEDs, buttons), `sound_pryon.c` (optional sound detection, `docs/re-aed.md`), `proto_esphome.c`, `proto_wyoming.c`, `arb.c` (wake word arbitration between Echos), `micdenoise.c` (RNNoise on the mic audio sent to the pipeline), `micgain.c` (gain of the mic audio sent to the pipeline), `sendspin.c`, `a2dp.c` (Bluetooth speaker both ways), `btout.c` (playing on a Bluetooth speaker: the mixer's A2DP route, `docs/re-a2dp-source.md`), `ble.c`, push updates (`ota.c`), online updates (`update.c`), `adbwifi.c` (the debug access switch) |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `aed_test` (stock sound detector, `docs/re-aed.md`), `latency`, `otatool`, `runas` (AIPC refuses uid 0, the image has no `su`), `curlspy`, `hciscan` (raw HCI on `/dev/stpbt`), `a2dpprobe` (stands in for the Bluetooth stack on the mixer's A2DP output) |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `src/third_party/` | monocypher 4.0.2, `dr_flac.h`, `minimp3.h`, libfreeaptx 0.2.2, RNNoise 0.1.1 (own licences, see README) |
| `scripts/` | PC side: `setup.sh` (guided install), `artifacts.sh` (Amazon artifacts for an installed Echo: more wake words, the sound detection model), `deploy.sh`, `probe.sh`, `capture-test.sh`, `mic-compare.sh` (micRaw against micAsr on a running Echo), `wifi-join.sh`, `install-system.sh`, `ota-push.sh`, `bundle.sh` (packs and signs an update; also CI's), `adb-wifi.sh` (adb over Wi-Fi with the update key); `lib/device.sh` picks the model, `lib/setup.sh` has the guided setup's helpers, `lib/wakeword.sh` the wake word installer |
| `scripts/device/`, `scripts/system/` | run on the Echo, reading the model's `device.conf` next to them; boot integration (`boot.sh`, `main.sh`) |
| `tools/` | `mkstubs.sh` (stand-ins for the stock libraries, for building without the firmware), OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm), `davs-fetch.py` |
| `tests/` | protocol tests against the reference implementations |
| `keys/` | `release.pub`: the key online updates are signed with (CI and releases, below) |
| `.github/workflows/` | `build.yml`: builds, tests, betas and releases |

## Build and test

Device binaries need the NDK and the unpacked firmware in `firmware/<codename>/` (the model's install page, steps 1–2, e.g. `devices/donut/README.md`), or
`STUBS=1` instead of the firmware (below). Without the NDK only the host targets build. The test references:
`python -m venv .venv && .venv/bin/pip install -r tests/requirements.txt`.

```sh
make [DEVICE=donut]                               # ARM binaries into build/donut/
make DEVICE=donut STUBS=1                         # the same, without the firmware: stand-ins for its libraries
make host                                         # PC build (build/hassmic-host) + qemu build for the tests (needs libopus)
make unit                                         # C unit tests
.venv/bin/python tests/fake_ha_esphome.py         # ESPHome native API, as Home Assistant (aioesphomeapi)
.venv/bin/python tests/fake_ha_arbitration.py     # two Echos + Home Assistant + an unknown device: wake word arbitration
.venv/bin/python tests/fake_ha.py [--qemu]        # Wyoming (wyoming)
.venv/bin/python tests/fake_ma_sendspin.py        # Sendspin, as Music Assistant (aiosendspin)
tests/ota_push_test.sh                            # signed push-update path end to end
.venv/bin/python tests/fake_ha_update.py          # online updates: Home Assistant, GitHub and root's installer in one
```

`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against
`firmware/$DEVICE/rootfs`.

Trial runs on the device: `scripts/deploy.sh`, then `adb shell sh /data/local/hassmic/run.sh`.

## CI and releases

`.github/workflows/build.yml` builds every model, runs the PC tests and publishes the update bundles that online
updates install (`src/hassmic/update.c`):

- a push to `main` publishes `v<version>-beta` (a prerelease: the `beta` channel);
- a push to `release` publishes `v<version>` (the `release` channel);
- pull requests build and test only.

The version is the commit's time in UTC, `2026.10.02.091530` (`make version`): nothing to bump. Home Assistant
compares these as calendar versions, so newer is newer across both channels, and a commit published as beta and then
as release has the same version: an Echo running that beta is not offered the release of it. A commit that is
published already is not published again (a re-run). Builds of your own report `<version>+<commit id>[-dirty]`;
Home Assistant cannot compare that form and so offers whatever is on the channel, once online updates are on.

Each release has `hassmic-<codename>.bundle` and `.bundle.sig` per model; the Echo picks its own by the board's
`codename`. CI builds with `RELEASE=1`: the daemon reports the bare version.

**No firmware in CI.** `make STUBS=1` links against stand-ins for the stock libraries, built from
`devices/<codename>/stubs/*.syms`: the functions our binaries take from each library, plus the symbols of the
executable it refers to or defines too (the linker exports those), in the library's own order (the order the linker
exports them in). They come out byte-identical to a firmware link. `tools/mkstubs.sh` writes the lists from the
firmware and checks exactly that, for every binary of every model; run it when the code starts using another function
of a stock library (a stub build then fails to link until it has). For the same reason the build is reproducible:
ESPHome's "compiled" time is the commit's (`BUILD_TIME`), not the clock's.

**The release key.** `keys/release.pub` is in every build and install; root accepts bundles signed with it, next to the
owner's `update.pub` (`scripts/system/main.sh`), but hassmic only downloads any once the owner picks a channel in Home
Assistant. The secret half is `secrets/release.key` on the maintainer's PC (back it up) and, base64-encoded
(`base64 -w0 secrets/release.key`), the secret `RELEASE_SIGNING_KEY` of the GitHub environment `release`, whose
deployment branches are `main` and `release` only. The one job that names that environment packs and signs the
bundles, from the binaries the build jobs left; the build jobs run whatever a branch or pull request brings, so they
only ever sign with a throwaway key (to check the bundling). A pull request runs on `refs/pull/<n>/merge` and is refused
the environment, workflow changes in it included. That leaves whoever can push to `main` or `release` directly:
protect both branches. A new key goes out signed by the old one: Echos prefer the `release.pub` of the copy they run
over the one on their system partition.

What the key does not cover, on purpose: root accepts a release-signed bundle that hassmic hands it whatever "Online
updates" is set to (the select is enforced by hassmic, which only downloads with a channel picked), and an older one
as well as a newer one (switching from beta back to release relies on that). So a hassmic taken over through the
network could put an older release back; it could not install code of its own.

**Trying the workflow locally** with [nektos/act](https://github.com/nektos/act): the publishing step only says what it
would publish under act. The toolchains go on a volume: unpacked inside the job containers (3 GB), act times out
removing them and fails the job after every step passed; one job at a time for the same reason.

```sh
echo "RELEASE_SIGNING_KEY=$(base64 -w0 secrets/release.key)" > /tmp/act.secrets
mkdir -p /tmp/act-toolchain
act push --secret-file /tmp/act.secrets --artifact-server-path /tmp/act-artifacts --concurrent-jobs 1 \
    --container-options "-v /tmp/act-toolchain:$PWD/toolchain"
```

Without a payload act takes the branch checked out; `-e` with `{"ref": "refs/heads/release"}` plays a push to `release`.

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
  image, unpacked rootfs, unlock zips, `re/` disassembly), `toolchain/`, `build/`, `device-logs/`, `secrets/` (`wifi.conf`, `update.key`, `release.key`), `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence.
