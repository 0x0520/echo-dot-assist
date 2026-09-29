# echo-dot-assist

> [!CAUTION]
> **Vibecoded.** Code, scripts, reverse-engineering notes, plan and this README were written by an AI (Claude) in
> conversation with the author, not by hand. Tried on one Echo of each supported model; nobody has reviewed or audited it.
>
> That includes the parts that can hurt: bootloader unlock, system partition and SELinux writes, the firewall that keeps
> the Echo away from Amazon and firmware updates, and signed push updates. Any of them can brick your Echo, leave it
> online when you think it is not, or open it up on your network.
>
> **Read what you run. No warranty, no support, your risk.**

Turns an **Amazon Echo** into a **Home Assistant voice satellite** that never talks to Amazon. Works on the Echo Dot 3
(2018), the Echo Dot 2 and the Echo 2; see [Supported Echos](#supported-echos).

Amazon's microphone processing (echo cancellation, beamforming, per-device mic calibration) and wake word engine stay,
so it hears you across the room and over its own music like before. Only the Alexa client is replaced, by a small daemon
called `hassmic` that speaks to Home Assistant as an ESPHome device (default) or as a Wyoming satellite.

## Supported Echos

|    | Echo                         | Model  | Codename                               | Way in                                           |
|----|------------------------------|--------|----------------------------------------|--------------------------------------------------|
| ✅ | Echo Dot 3rd gen (2018)      | D9N29T | [`donut`](devices/donut/README.md)     | open the case, wires on test pads                |
| ✅ | Echo Dot 2nd gen (2016)      | RS03QR | [`biscuit`](devices/biscuit/README.md) | micro-USB, no soldering                          |
| ✅ | Echo 2nd gen (2017)          | XC56PY | [`radar`](devices/radar/README.md)     | open the case, solder USB to the amplifier board |
| ❌ | Echo Dot 3rd gen (2019–2020) | C78MP8 | `crumpet`                              | the Dot 3 unlock does not work on it             |
| ❌ | Echo Dot 3rd gen with clock  | 36EBT3 | `doebrite`                             | thought to be `crumpet` hardware                 |

✅ works, tried on one Echo of that model · ❌ not supported yet

Each model needs exactly the firmware its page names (`donut`: Fire OS 6574.1 only). The unlock can brick the device.
Models not in the table: what is known and how to add one is in [`devices/`](devices/README.md).

## Features

|                                               | Stock Alexa             | hassmic, ESPHome (default)                   | hassmic, Wyoming         |
|-----------------------------------------------|-------------------------|----------------------------------------------|--------------------------|
| Voice assistant                               | Alexa (Amazon cloud)    | Home Assistant Assist                        | Home Assistant Assist    |
| Amazon's mic processing (AEC, beamforming)    | ✅                      | ✅                                           | ✅                       |
| Wake word on the device                       | ✅                      | ✅ "Alexa"; "Echo", "Computer", … with `scripts/wakeword.sh` ([details](devices/donut/README.md#3-optional-another-wake-word)) | ✅ same      |
| Wake word in Home Assistant instead           | ❌                      | ✅ (`-w remote`)                             | ✅ (`-w remote`)         |
| Interrupt a reply ("Alexa" / "Alexa, stop")   | ✅                      | ✅                                           | ✅                       |
| Several Echos hear it, only the nearest answers | ✅ (Amazon cloud)      | ✅ between these Echos, on the LAN           | ❌                       |
| Timers                                        | ✅                      | ✅                                           | ❌                       |
| Announcements, follow-up questions            | ✅                      | ✅                                           | ❌                       |
| Media player entity (TTS, `play_media`)       | ❌                      | ✅                                           | ❌                       |
| Multiroom music                               | Amazon speaker groups   | Music Assistant (Sendspin)                   | Music Assistant (Sendspin) |
| Bluetooth speaker                             | SBC                     | SBC, AAC, aptX, aptX HD; pairing from HA     | reconnects already paired devices only |
| Bluetooth proxy for Home Assistant            | ❌                      | ✅ scanning, connections, pairing            | ❌                       |
| Buttons, LED ring, hardware mute              | ✅                      | ✅                                           | ✅                       |
| Mute state and audio settings in HA           | ❌                      | ✅                                           | ❌                       |
| Do not disturb                                | ✅ (Alexa app)          | ✅ switch in HA                              | ❌                       |
| Equalizer (bass, mid, treble)                 | ✅ (Alexa app)          | ✅ sliders in HA                             | ❌                       |
| Encrypted link to Home Assistant              | –                       | ✅ key set by Home Assistant                 | ❌ plain TCP             |
| Talks to Amazon                               | always                  | never (firewalled)                           | never (firewalled)       |
| Updates                                       | automatic, from Amazon  | signed, pushed from your PC                  | signed, pushed from your PC |

Details:

- **Voice**: found automatically by Home Assistant's ESPHome integration, no YAML, no ESPHome add-on. Replies start while
  text-to-speech is still being generated. "Stop" works only right behind the wake word ("Alexa, stop"), because
  Amazon's models only hear it in the two seconds after it; out of silence, "Alexa, stop the music" goes to Home
  Assistant as a normal command. While a timer rings or something plays, the wake word is
  accepted more readily, as Amazon's models are tuned to do.
- **Several Echos**: like stock, only the Echo that heard the wake word best answers (among Echos listening for the same
  word: one on "Echo" and one on "Alexa" each answer their own); the others stay silent (no
  sound, no light). The Echos settle it among themselves on the local network in 0.2 s, by how clearly the word stood
  out of the room's noise; an Echo that is in a conversation or ringing keeps the next wake word. With only one Echo
  there is no delay. They find each other by themselves ("Join arbitration network", on by default); the shared key
  travels through your Home Assistant, so nobody else on the network can join or silence them. For that, tick "Allow
  the device to perform Home Assistant actions" in each Echo's ESPHome options (Home Assistant shows a repair until
  then); give every Echo its own `NAME`. Other satellites (ESP32 and so on) are not part of it; Home Assistant itself then lets the first one
  that reports the wake word answer, and the Echo that is second now just goes quiet instead of flashing an error.
- **Buttons**: action = talk without the wake word / pause and resume music / stop an alarm; volume in 10 % steps;
  mic-off is the hardware mute it always was (red ring, Alexa's own sounds). The LED ring shows listening, thinking,
  speaking, errors and mute. Silent and dark at boot.
- **Music**: one source at a time, the newest wins. A phone starting over Bluetooth pauses Music Assistant (the whole
  group), Music Assistant starting on the Echo pauses the phone. The voice assistant ducks both.
- **Bluetooth**: the proxy works like an ESPHome `bluetooth_proxy` with `active: true`, up to 3 connections, "Just Works"
  pairing only. While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering.
- **Settings in Home Assistant**: noise suppression level, auto gain, mic volume multiplier, mute switch, "Do not disturb"
  switch (drops announcements, purple pulse when switched on), "Wake sound" switch (covers all local sounds),
  "Bluetooth pairing" switch, "Bluetooth announcements" switch and their language, "Join arbitration network" switch, "Music Assistant without pairing" switch (off by default:
  only Sendspin servers paired with the token may play), equalizer (bass, mid, treble, −6 to +6 dB, Amazon's own,
  applied to everything the Echo plays). Diagnostics, off by default: SoC temperature, CPU usage.
- **No cloud**: Alexa client, updater and telemetry are stopped at every boot; a firewall drops everything that is not
  going to a local address. Only hassmic itself may go further, to fetch replies and music from where Home Assistant or
  Music Assistant point it. See [Security](#security).
- **Reversible**: delete one file for stock behaviour, run the uninstaller, or reflash stock from recovery.

## Requirements

- A [supported Echo](#supported-echos) and a USB way into it: a plain cable on the Echo Dot 2, wires soldered or held
  on test pads on the Echo Dot 3 and Echo 2. The model's page says what exactly.
- A **Linux PC** with `adb`, `fastboot`, `python3`, `make`, `unzip`, `debugfs` (e2fsprogs), `sqlite3`, ~5 GB free disk.
- **Home Assistant** with a working Assist pipeline (speech-to-text, conversation agent, text-to-speech). Test it with
  the app first. Optional: Music Assistant (tested with 2.10.4).
- **Wi-Fi** with WPA2 passphrase (no captive portal, no enterprise login) that reaches Home Assistant.

## Install

Each model's page, linked in [Supported Echos](#supported-echos), has the steps by hand and what to solder.

The guided way, for every supported model:

```sh
scripts/setup.sh              # picks the Echo on adb, or asks which one; then runs every step
```

A terminal screen with a progress bar and the list of steps. It runs everything on its own and only stops when you
have to do something: download a file into `~/Downloads` (it picks it up from there and checks it), solder or plug a
cable, hold a button, type a name or the Wi-Fi password. Its last step offers another wake word ("Echo",
"Computer", …; see `scripts/wakeword.sh`), or keeps "Alexa". It offers to install missing tools. Before it starts it asks
for a typed `yes`, as it wipes the Echo. Command output goes to `build/<codename>/setup.log`; when something fails it
shows the end of it and offers to try again. Ctrl-C stops it at any point and the next run picks up where it left off;
`--dry-run` walks all steps and shows the commands without running any, `--restart` starts over for the next Echo of
the same model. The model's page has the same steps written out.

## Updating

```sh
git pull
scripts/ota-push.sh <echo-ip>        # remembers the address
```

Builds, signs, pushes over Wi-Fi (TCP 28929). The Echo installs only what verifies against your key, restarts hassmic,
and falls back to the installed copy by itself if the new one does not stay up. What changed: [CHANGELOG.md](CHANGELOG.md).

## Configuration

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
NAME="Kitchen Echo"         # device name in Home Assistant
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see the model's install page
```

| `ARGS` option | Effect |
|---|---|
| `-m <pryon.manifest>` | wake word model to start with, until one is picked in Home Assistant |
| `-w remote` | wake word detection in Home Assistant (openWakeWord) instead of on the Echo |
| `-E` | no sound on wake |
| `-L` | leave the LED ring alone |
| `-V` | leave the volume buttons alone |
| `-z 0` | no Sendspin player |
| `-a 0` | no arbitration with other Echos (UDP 28930) |
| `-p <port>` | another port (the firewall only admits inbound TCP 16384–32767) |

adb also works over Wi-Fi: `adb connect <echo-ip>:5555`. The cable is only needed for TWRP.

## Troubleshooting

Log: `adb shell tail -30 /data/local/hassmic/boot.log`.

**Wake word and button do nothing.** Most likely no connection to Home Assistant; the Echo does not signal that (known
gap). In the log, `wake: ALEXA type=2` means it heard you, `client connected` / `voice assistant: subscribed` means Home
Assistant is there. Nothing after the last `client disconnected`: check the network (`adb shell ifconfig wlan0`; can Home
Assistant reach that address?). Keep exactly one Wi-Fi profile on the Echo.

**No sound from replies or music.** The Echo fetches every reply, announcement and `play_media` from the URL Home
Assistant or Music Assistant gives it. Home Assistant builds that from its internal URL (Settings → System → Network),
or its LAN IP when none is set. The Echo must resolve the name (DNS from DHCP; `.local` via mDNS works) and route to the
address; on a network without internet that means a URL inside your network. The log names what failed
(`net: cannot ...`). If not even button sounds play, check the volume.

**"Invalid encryption key" in Home Assistant** (Echo reset, or something else set a key first):
`adb shell rm /data/local/hassmic/state/api_key`, restart hassmic (or reboot), delete the device in Home Assistant, add it
again.

Open issues and measurements: [PLAN.md](PLAN.md).

## Uninstall

- **Temporarily**: `adb shell rm /data/local/hassmic/hassmic.conf`, reboot. The Echo is a stock, unregistered Echo (which
  updates itself if it gets internet).
- **Properly**: `scripts/install-system.sh --uninstall` removes the files from the system partition and restores the policy.
- **Completely**: reflash the stock firmware from TWRP as in step 1 of the model's install page ([donut](devices/donut/README.md#1-unlock-flash-stock-firmware-root)).

## Security

- **ESPHome link**: encrypted like an ESPHome device with `api: encryption` but no key in its YAML. Home Assistant
  generates the key when you add the Echo, sets it over an encrypted connection, and clears it when you delete the
  device. **Until then anyone on the network can connect**, or set a key first (then see
  [Troubleshooting](#troubleshooting)). The key lives in `/data/local/hassmic/state/api_key`.
- **Wyoming link**: unencrypted and unauthenticated, like every Wyoming satellite.
- **Egress**: Amazon's daemons may only reach local addresses (plus DNS to the servers DHCP hands out); `otad` and
  `ace_otad` never get out. hassmic itself may reach any address. Put the Echo on a network without internet as a second
  layer.
- **Inbound**: TCP 16384–32767 only (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates), UDP 16384–32767
  (28930 arbitration between Echos).
- **Arbitration between Echos**: an Echo takes the network key only from Home Assistant, over its encrypted API link,
  as a call of its own action `esphome.<node>_arbitration_key`; a member hands it over by asking Home Assistant to run
  that action, which needs "Allow the device to perform Home Assistant actions". So only devices you adopted into Home
  Assistant and allowed to act take part; the key travels encrypted to the receiving Echo, so it is not readable in
  Home Assistant's traces or logbook. Rounds are authenticated with the key and cannot be replayed. The keys are in
  `state/arb_key` and `state/arbitration`.
- **Updates**: only bundles signed with your `secrets/update.key` are installed.
- **Bluetooth**: keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), both under `/data/local/hassmic/`.

## Development

Architecture, repository layout, building for the PC, tests and contribution notes: [DEVELOPMENT.md](DEVELOPMENT.md).

## Licence

[MIT](LICENSE), for everything written here. The files in `src/third_party/` keep their own licences, stated in each file:
monocypher (BSD-2-Clause OR CC0-1.0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0-1.0), `freeaptx.c`/`.h`
(LGPL-2.1-or-later; hassmic links it statically, and everything needed to rebuild and relink it is in this repository).

Nothing of Amazon's is in this repository and nothing of it is covered by this licence: firmware, libraries and wake-word
models come from your own device and stay Amazon's. Not affiliated with or endorsed by Amazon, Home Assistant or
Music Assistant; "Alexa" and "Echo" are Amazon's trademarks.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
