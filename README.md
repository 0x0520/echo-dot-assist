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
| Wake word on the device                       | ✅                      | ✅ "Alexa"; "Echo", "Computer", … with `scripts/artifacts.sh` ([details](devices/donut/README.md#3-optional-another-wake-word)) | ✅ same      |
| Wake word in Home Assistant instead           | ❌                      | ✅ (`-w remote`)                             | ✅ (`-w remote`)         |
| Interrupt a reply ("Alexa" / "Alexa, stop")   | ✅                      | ✅                                           | ✅                       |
| Several Echos hear it, only the nearest answers | ✅ (Amazon cloud)      | ✅ between these Echos, on the LAN           | ❌                       |
| Timers                                        | ✅                      | ✅                                           | ❌                       |
| Alarm clock, also without Home Assistant      | ✅                      | ✅ three alarms, set in HA ([details](#alarm-clock)) | ❌                |
| Announcements, follow-up questions            | ✅                      | ✅                                           | ❌                       |
| Media player entity (TTS, `play_media`)       | ❌                      | ✅                                           | ❌                       |
| Multiroom music                               | Amazon speaker groups   | Music Assistant (Sendspin)                   | Music Assistant (Sendspin) |
| Bluetooth speaker                             | SBC                     | SBC, aptX, aptX HD, AAC optional; pairing from HA | reconnects already paired devices only |
| Play on a Bluetooth speaker                   | ✅ (Alexa app)          | ✅ found and paired from HA, SBC ([details](#bluetooth-speaker-output)) | keeps playing on one already set up |
| Bluetooth proxy for Home Assistant            | ❌                      | ✅ scanning, connections, pairing            | ❌                       |
| Buttons, LED ring, hardware mute              | ✅                      | ✅                                           | ✅                       |
| Mute state and audio settings in HA           | ❌                      | ✅                                           | ❌                       |
| Do not disturb                                | ✅ (Alexa app)          | ✅ switch in HA                              | ❌                       |
| Equalizer (bass, mid, treble)                 | ✅ (Alexa app)          | ✅ sliders in HA                             | ❌                       |
| Light ring follows the room's light           | ✅                      | ✅ same, or a fixed level from HA; illuminance sensor | ❌ (stock's automatic only) |
| Sound detection (smoke alarm, glass, dog, …)  | ✅ Alexa Guard, checked in Amazon's cloud | optional, off by default: on the Echo only, less reliable ([details](#sound-detection)) | ❌ |
| Whisper detection                             | ✅ answers in a whisper | sensor for the conversation agent's prompt ([details](#whisper)) | ❌ |
| Motion sensor                                 | ❌                      | **experimental**, off by default: from the Wi-Fi signal ([details](#wifi-motion)) | ❌ |
| Encrypted link to Home Assistant              | –                       | ✅ key set by Home Assistant                 | ❌ plain TCP             |
| Change Wi-Fi without a PC                     | ✅ (Alexa app)          | ✅ over Bluetooth, from HA's Improv integration or the Improv app ([details](#wifi-setup)) | ✅ same (no switch) |
| Talks to Amazon                               | always                  | never (firewalled)                           | never (firewalled)       |
| Updates                                       | automatic, from Amazon  | signed: pushed from your PC, or online from Home Assistant (off by default) | signed, pushed from your PC |

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
- **Buttons**: action = talk without the wake word / pause and resume music / stop a timer or alarm / cancel a request while
  Home Assistant is still listening or thinking (as on a Voice PE; the wake word then cancels it too and listens
  again; ESPHome only); volume in 10 % steps;
  mic-off is the hardware mute it always was (red ring, Alexa's own sounds). The LED ring shows listening, thinking,
  speaking, errors and mute. Silent and dark at boot.
- **Alarm clock**<a id="alarm-clock"></a>: three alarms that ring on the Echo itself, like stock's, also while Home
  Assistant or your network is down (Home Assistant itself only has timers). Each has "Alarm 1" (on/off), "Alarm 1 time"
  and "Alarm 1 repeat": once, every day, weekdays, weekends, or one day of the week ("Mondays"). A one-time alarm switches
  itself off after it rang. It rings for up to 10 minutes; the action button, "Alexa, stop" (right behind the wake
  word, as always) or the "Stop alarm" button end it, and "Snooze alarm" makes it ring again in 9 minutes. For
  automations: "Alarm ringing" (on while one rings), the "Alarm" event (`alarm_1`, `alarm_2`, `alarm_3`, when it starts
  ringing; only while Home Assistant is connected) and "Next alarm" (a timestamp; unknown when none is on).
  - **The time comes from Home Assistant**, with its time zone, so summer time is handled (an alarm in the hour the
    clocks skip rings an hour later on the clock, one in the hour they repeat rings once). The Echo asks when Home
    Assistant connects and every hour, and keeps counting on its own in between, also while Home Assistant is away.
  - **After the Echo restarts, alarms wait for Home Assistant.** Locked away from the internet, the Echo has no clock it
    can trust after a reboot or a power cut, so no alarm rings until Home Assistant has told it the time; one that
    should have rung in the last 10 minutes then rings late, an older one is skipped. "Next alarm" stays unknown until
    then. hassmic itself restarting (an update) does not lose the time.
  - Same sound as a timer, at the Echo's volume, but **at least 30 %**: an alarm set at night with the volume turned
    down to 0 would ring unheard. The volume goes back once it stops, unless you changed it while it rang. Timers ring
    at the volume as it is (you set them a moment before). ESPHome only, not with Wyoming.
- **Music**: one source at a time, the newest wins. A phone starting over Bluetooth pauses Music Assistant (the whole
  group), Music Assistant starting on the Echo pauses the phone. The voice assistant ducks both.
- **Playing on a Bluetooth speaker**<a id="bluetooth-speaker-output"></a>: everything the Echo plays (replies, timers,
  its sounds, music) can come out of a Bluetooth speaker instead of its own, as with stock. Put the speaker in pairing
  mode near the Echo and switch on "Bluetooth speaker search": within a minute the Echo pairs with the strongest one it
  hears (speakers, headphones, and PCs that offer to play audio) and plays on it. You cannot pick one from a list: Home
  Assistant reads an ESPHome select's choices only when it connects, so keep only the speaker you want in pairing mode.
  "Play on Bluetooth speaker" switches between it and the Echo; the Echo reconnects by itself when the speaker comes
  back, and takes it when the speaker calls the Echo on switching on. "Bluetooth speaker" shows its name and state.
  If the speaker forgets the Echo (reset, or paired with too many others), the Echo does not pair with it again on its
  own: run "Bluetooth speaker search" again. The same for a phone: pair it again with "Bluetooth pairing" on.
  - **Volume**: the speaker has its own. While the Echo plays on it, the volume buttons, Home Assistant and Music
    Assistant set the speaker's volume, and the light ring shows it; the Echo starts from the speaker's own volume and
    the speaker's buttons move it too. Back on the Echo, its own volume returns. With a speaker that supports
    Bluetooth absolute volume (most do) the Echo sends the sound at full level and the speaker turns it down, which
    sounds best; with one that does not, the Echo turns it down itself, as stock does.
  - **Music Assistant**: a Bluetooth speaker plays late, by its buffer. "Bluetooth speaker delay" (default 250 ms) is
    what the Echo allows for, so that it stays in time with other players; set it by ear for your speaker.
  - The Echo still listens for the wake word while the sound comes from the speaker. How well it hears through loud
    music played elsewhere in the room has not been measured yet.
  - SBC only (every speaker has it), one speaker at a time. ESPHome only for setting it up.
- **Bluetooth**: the proxy works like an ESPHome `bluetooth_proxy` with `active: true`, up to 3 connections, "Just Works"
  pairing only, with full-length (16-byte) keys. A device that has forgotten its pairing keeps its bond on the Echo until
  Home Assistant pairs it again (which replaces the bond) or unpairs it. While a phone plays, the proxy stops scanning:
  the radio cannot do both without the music stuttering.
  - **"Bluetooth proxy: secure pairing only"** (off by default): pairs only devices that can do LE Secure Connections.
    Older devices only know "legacy" pairing, and its key can be worked out by anyone who recorded the pairing over the
    air (tools such as crackle do it in seconds), and with it everything sent later. On, such a device is refused (Home
    Assistant shows the pairing failed), and a legacy pairing made earlier is no longer used: the link stays
    unencrypted until you pair the device again, which works only if it can do Secure Connections. Off, as before, so
    that sensors which only know legacy pairing keep working.
  - **"Bluetooth AAC"** (off by default): whether phones may send AAC. AAC is decoded by the Echo's own copy of FFmpeg,
    an old version this project cannot update, fed by whatever a paired phone (or something pretending to be it) sends
    over the radio; SBC and aptX are decoded by small code of this project's own. Off, phones use SBC, or aptX where
    they have it. On the Echo's small speaker the difference is small: SBC at the bitrate most phones use is close to
    AAC; a phone that sends SBC at a low bitrate may sound slightly duller in the highs. A change
    counts from the phone's next connection: disconnect it and connect it again.
- **Settings in Home Assistant**: "Mic level" (how loud speech reaches the voice assistant, -35 to -15 dBFS, default
  -26; the Echo adjusts its gain to it), "Noise reduction" (off by default; low, medium, high: RNNoise on what the voice
  assistant gets takes the background down by up to 6, 9 or 12 dB), mute switch, "Do not disturb"
  switch (drops announcements, purple pulse when switched on), "Wake sound" switch (covers all local sounds),
  "Bluetooth pairing" switch (blue chaser on the ring while it is on), "Bluetooth announcements" switch and their language,
  "Bluetooth AAC" and "Bluetooth proxy: secure pairing only" switches (both off by default, see Bluetooth above;
  switching either the less safe way is taken only over the encrypted connection with Home Assistant's key),
  "Bluetooth speaker search" and "Play on Bluetooth speaker" switches, "Bluetooth speaker" state and "Bluetooth speaker
  delay" (see [Playing on a Bluetooth speaker](#bluetooth-speaker-output)), "Join arbitration network" switch, "Music Assistant without pairing" switch (off by default:
  only Sendspin servers paired with the token may play), equalizer (bass, mid, treble, −6 to +6 dB, Amazon's own,
  applied to everything the Echo plays), "Debug access (adb over Wi-Fi)" switch (see [Configuration](#configuration)).
  "LED auto brightness" switch and "LED brightness" slider: the ring dims with the room as on a stock Echo (Amazon's
  own logic, on by default); setting a level holds it there and switches the automatic off. "Illuminance": the Echo's
  light sensor in lux, as Amazon reads it, for automations.
  Diagnostics, off by default: SoC temperature, CPU usage.
- **Task manager**<a id="task-manager"></a>: what the Echo's CPU and memory go to, to see how much room is left. In
  Home Assistant on the Echo's device page under Diagnostic: "Memory used" (on by default), and, to be enabled there
  (they are off by default, the two lists most of all: a new text every 10 s fills the recorder): "Memory available",
  "Load average", "hassmic CPU", "hassmic memory", "Top processes" (the five busiest in the last 10 s as
  `name pid cpu% memory`, e.g. `mixer 512 6.3% 21MB`) and "hassmic threads" (hassmic's own, each by name, busiest
  first: `capture`, `mic sender`, `esphome client`, ...). CPU shares are of all cores together, as "CPU usage".
  Updated every 10 s while Home Assistant is connected; one look takes a few milliseconds (hassmic's log says how long
  once, and the `diag` thread shows its share). The same live every 2 s on a PC: `scripts/top.sh` (needs only adb,
  works on Windows).
  **Ending a process**: the action `esphome.<node>_kill_process` (Developer tools, Actions) with `pid` from "Top
  processes" and `signal` `term` (or empty) or `kill`. "Last kill" shows the outcome. It is taken only over the
  encrypted connection with Home Assistant's key, and root decides: Amazon's daemons may be ended, and hassmic itself
  (it starts again within seconds), but not what keeps the Echo running, reachable or locked down (init and the system
  daemons, kernel threads, `wpa_supplicant`, `dhcpcd`, the `mixer` hassmic's audio depends on, the firewall and boot
  scripts and whatever they run). `term` first; whatever is still there 3 s later gets `kill`. A process ended this way
  may be started again by Android's init; it stays gone only until the next reboot either way.
- **Sound detection** (optional, off by default)<a id="sound-detection"></a>: the "Sound detection" switch runs Amazon's
  own Alexa Guard model on the Echo, beside the wake word, and the "Sound" event entity reports what it heard:
  `smoke_or_co_alarm`, `glass_break`, `dog_bark`, `baby_cry`, `snoring`, `cough`, `water`, `beeping_appliance`. Use it
  in automations ("When Sound fires with smoke_or_co_alarm"). Please read before relying on it:
  - **Less reliable than on a stock Echo.** Amazon checks every hit in its cloud before it tells anyone; that check
    cannot be had without Amazon, so here every hit of the model counts. In tests it also took a barking dog, pouring
    water and a toilet flush for breaking glass, and a cough for a beeping appliance. Treat an event as a hint, not as
    an alarm system, and never as a replacement for a smoke or CO detector.
  - **Slow**: the model listens in windows of 10 s, so an event comes up to 10 s after the sound, and once per window
    while the sound goes on.
  - **Coarser than stock**: the model gives smoke alarms, smoke sirens and CO alarms the same score, and coughs the same
    as running water, so they are one event each (`smoke_or_co_alarm`; `cough`). "Human presence" is left out: it fires
    on any talk, TV or knock.
  - Nothing is reported while the Echo is muted, or for a window in which the Echo itself played something (a reply, a
    timer, music, its sounds): those are what it would hear.
  - **Private**: it all happens on the Echo; nothing leaves it except the event to Home Assistant (a stock Echo uploads
    the recordings, and near misses for training). Costs about 13 % of one CPU core while on (Echo Dot 2).
  - It uses the model in the Echo's firmware. Amazon's newest can be installed in its place with `scripts/artifacts.sh`
    ("Other artifacts"; so far it scored the same on every test).
  - ESPHome only, not with Wyoming. Background: [docs/re-aed.md](docs/re-aed.md).
- **Whisper detection** (optional)<a id="whisper"></a>: a stock Echo answers a whispered request in a whisper. Here the
  binary sensor "Last request whispered" says whether the last request was whispered, for the conversation agent to
  answer the same way. It uses Amazon's own whisper detector on the Echo, with a model that only Amazon hands out:
  install it from a PC with `scripts/artifacts.sh` ("Other artifacts" → "Whisper detection"). It needs the Echo
  registered to an Amazon account for a few minutes (the script walks you through it and undoes it), as for other
  wake words. Over Wi-Fi, first turn on the Echo's "Debug access (adb over Wi-Fi)" switch in Home Assistant, then run
  `scripts/artifacts.sh <echo-ip>`. The model stays through updates; without it there is no sensor.
  - The sensor is set when you stop speaking, before speech to text has finished, so the agent's prompt template can
    read it. For example, in the LLM conversation agent's instructions (the entity id has your Echo's name in it):

    ```jinja
    {% if is_state('binary_sensor.echo_dot_last_request_whispered', 'on') %}
    The user whispered. Answer in a whisper: mark the whole answer the way your text-to-speech engine whispers.
    {% endif %}
    ```

    Replace the second line with the markup your text-to-speech engine understands; Piper has none.
  - In tests (Echo Dot 2, German commands from 1–2 m) whispered commands scored 984–999 out of 1000, spoken ones
    0–18, quietly spoken ones too. Saying the wake word normally and whispering the rest is fine. Sounds without words (breathing, rustling) can score high, but only what the
    pipeline took for a command is scored.
  - It all happens on the Echo, during your request only. ESPHome only, not with Wyoming. Background:
    [docs/re-whisper.md](docs/re-whisper.md).
- **Wi-Fi motion** (**experimental**, off by default)<a id="wifi-motion"></a>: "Wi-Fi motion detection (experimental)"
  turns the Echo into a motion sensor without any extra hardware. Someone walking between the Echo and your Wi-Fi router
  changes how strongly the Echo receives the router, and "Wi-Fi motion (experimental)" (a motion binary sensor) goes on
  while that happens and off 30 s after it stops, like a PIR sensor. "Wi-Fi motion sensitivity (experimental)", 1 to 10
  (default 5), sets how much change counts. It is a first version, tried in one flat for a few minutes and one night,
  where it mostly did what it should; please read:
  - **Motion, not presence.** Someone sitting still does not show; an empty room and a quiet one look the same.
  - **Only between the Echo and the router.** It sees best what crosses the path between them (also in the next room,
    if the router is there); someone moving elsewhere in the room may not show at all.
  - **Expect false alarms** from other Wi-Fi devices, doors and people in the router's room; how often has not been
    counted yet. Try the sensitivity before you rely on it. On the Echo Dot 2 and Echo 2
    also when the router switches between its faster speeds: their Wi-Fi does not say at which speed a frame came,
    and a router sends each speed at its own strength (the Echo Dot 3 allows for that).
  - **Through a small kernel module.** The Wi-Fi drivers do not report what this needs (the Echo Dot 3's only for the
    last frame from any device nearby), so hassmic brings a kernel module of its own that reads the level of every
    frame from your router in the driver. It is only loaded once you switch Wi-Fi motion on (within 10 s), and then
    stays loaded until the Echo restarts. Running on an Echo Dot 3, an Echo Dot 2 and an Echo 2.
  - ESPHome only. It does not use the microphones; muting the Echo does not stop it.
- **Wi-Fi setup over Bluetooth**<a id="wifi-setup"></a>: the Echo can be given a new Wi-Fi network without a PC, the
  way ESPHome devices are: with [Improv Wi-Fi](https://www.improv-wifi.com/). For a new router, a new password, or an
  Echo that moved house. It is offered only for a while, and only to someone at the Echo:
  - **When.** By itself when the Echo has had no Wi-Fi address for 2 minutes (after starting without Wi-Fi, or after
    the link went), for 5 minutes, once per outage; or at any time when you **hold the action button (the dot) for 5
    seconds**, as on a stock Echo. While it is offered the ring shows the orange setup spinner, and a short press of
    the action button only allows the setup (it starts no voice command). It closes after 5 minutes, a minute after it
    worked, or as soon as the Echo has Wi-Fi again (if nobody is connected to it).
  - **How.** In Home Assistant the Echo then shows up under Settings → Devices & services as a discovered
    "Improv via BLE" device (Home Assistant needs Bluetooth itself, or a Bluetooth proxy near the Echo; another Echo
    with hassmic is one). Or open [improv-wifi.com](https://www.improv-wifi.com/) in Chrome or Edge on a phone or PC
    with Bluetooth and pick "Connect device to Wi-Fi". Once connected, either asks you to **press the action button** on the
    Echo: that allows the connected app for a minute. Then enter the network name and password. The Echo joins, keeps the network for the
    next boots, and Home Assistant finds it again on its own (as before, over mDNS). With a wrong password the app says
    it could not connect and the Echo stays on the network it had.
  - WPA/WPA2 with a password (8 to 63 characters) or open networks; no enterprise login, no WPA3-only networks.
  - "Wi-Fi setup over Bluetooth" (a switch in Home Assistant, on by default) turns it off completely, button
    included. Needs the Bluetooth radio to be hassmic's (not with `-B`).
- **No cloud**: Alexa client, updater and telemetry are stopped at every boot; a firewall drops everything that is not
  going to a local address. Only hassmic itself may go further, to fetch replies and music from where Home Assistant or
  Music Assistant point it. See [Security](#security).
- **Reversible**: delete one file for stock behaviour, run the uninstaller, or reflash stock from recovery.

## Requirements

- A [supported Echo](#supported-echos) and a USB way into it: a plain cable on the Echo Dot 2, wires soldered or held
  on test pads on the Echo Dot 3 and Echo 2. The model's page says what exactly.
- A **Linux PC** with `adb`, `fastboot`, `python3`, `make`, `unzip`, `debugfs` (e2fsprogs), `sqlite3`, ~5 GB free disk.
  On **Windows** use WSL2 (`wsl --install`, then work from a clone made inside it), with
  [usbipd-win](https://github.com/dorssel/usbipd-win) to hand the Echo's USB connection to WSL. The scripts stop with
  a message when started from Git Bash, MSYS2 or Cygwin, except the two that need no compiler, only Python:
  `scripts/ota-push.sh` with the release build of a published commit, and `scripts/adb-wifi.sh`.
- **Home Assistant** with a working Assist pipeline (speech-to-text, conversation agent, text-to-speech). Test it with
  the app first. Optional: Music Assistant (tested with 2.10.4).
- **Wi-Fi** with WPA2 passphrase (no captive portal, no enterprise login) that reaches Home Assistant. Set at the
  install; changed later without a PC through [Wi-Fi setup over Bluetooth](#wifi-setup).

## Install

Each model's page, linked in [Supported Echos](#supported-echos), has the steps by hand and what to solder.

The guided way, for every supported model:

```sh
scripts/setup.sh              # picks the Echo on adb, or asks which one; then runs every step
```

A terminal screen with a progress bar and the list of steps. It runs everything on its own and only stops when you
have to do something: download a file into `~/Downloads` (it picks it up from there and checks it), solder or plug a
cable, hold a button, type a name or the Wi-Fi password. Its last step offers another wake word ("Echo",
"Computer", …; see `scripts/artifacts.sh`), or keeps "Alexa". It offers to install missing tools. Before it starts it asks
for a typed `yes`, as it wipes the Echo. Command output goes to `build/<codename>/setup.log`; when something fails it
shows the end of it and offers to try again. Ctrl-C stops it at any point and the next run picks up where it left off;
`--dry-run` walks all steps and shows the commands without running any, `--restart` starts over for the next Echo of
the same model. The model's page has the same steps written out.

**Nothing to compile** on a commit that GitHub has a build of: every commit on `main` and `release` once CI has
published it (a few minutes after the push). The setup then offers that build, the one online updates install too, and
skips the Android NDK (1 GB), the compilers and unpacking the firmware; the build is checked against the project's
release key (`keys/release.pub`) before anything uses it. With changes of your own in the checkout, or on a commit
without a build, it builds here as before. The other scripts that need the Echo's programs (`deploy.sh`,
`install-system.sh`, `ota-push.sh`) do the same: the release build where there is no NDK here, `PREBUILT=1` to insist
on it, `PREBUILT=0` to always build.

## Updating

### From Home Assistant (online updates)

Off by default. Pick a channel in the Echo's "Online updates" select:

- `release`: releases only (built from the `release` branch);
- `beta`: every build of `main`, plus every release;
- `off`: nothing is fetched (the default).

The Echo's "Firmware" update entity then shows the newest build on that channel. Versions are the time of the
build's commit in UTC (`2026.10.02.091530`), on both channels. Its install button downloads the
bundle for this model from the project's GitHub releases and installs it, as a push from your PC would: the Echo
checks the release key's signature (`keys/release.pub`, in every build) and falls back by itself if the new version
does not stay up. Only an encrypted connection to Home Assistant, with the key Home Assistant set, may switch the
channel or install. Once the new version passes its self test it also becomes the copy the Echo falls back to, as for a push.
A release-signed build older than what the Echo has is refused, so switching from `beta` to `release` keeps the newer
beta until a newer release is out; to go back on purpose, push the older version from your PC (`scripts/ota-push.sh`).
ESPHome mode only. The release key arrives with the install or with the first push from a build that has it; until
then the entity says so.

Turning online updates on means trusting the project's releases: they are built and signed by GitHub Actions
(`.github/workflows/build.yml`), in a job that only runs for the `main` and `release` branches; its secret is the only
copy of the release key besides the maintainer's.

### From your PC

```sh
git pull
scripts/ota-push.sh <echo-ip>        # remembers the address
```

Builds (or downloads that commit's release build, as the setup does), signs, pushes over Wi-Fi (TCP 28929). The Echo installs only what verifies against your key, restarts hassmic,
and falls back to the installed copy by itself if the new one does not stay up. What changed: [CHANGELOG.md](CHANGELOG.md).

Every update, pushed or online, runs a self test as it starts: wake word engine loaded, ports open, a second of
microphone audio. Once it passes (a few seconds), the Echo makes it the installed copy, start script and update checker
included: the version it falls back to from then on is always the last one that worked.

## Configuration

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
NAME="Kitchen Echo"         # device name in Home Assistant
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see the model's install page
#ADB_WIFI=1                 # leave adb over Wi-Fi open, see below
#ADB_WIFI_FROM=192.168.1.20 # adb over Wi-Fi from this address (or subnet, 192.168.1.0/24) only, see below
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
| `-W <addresses>` | only these may connect: IPv4 addresses or subnets, comma separated (`-W 192.168.1.10` or `-W 192.168.1.10,10.0.0.0/24`). For Wyoming: put Home Assistant's address here, see [Security](#security) |

**adb over Wi-Fi is closed.** adb on an unlocked Echo is a root shell that asks for no key, so an open port 5555
would give it to everyone on the network. Over USB adb always works. Over Wi-Fi:

- run `scripts/adb-wifi.sh <echo-ip>` on the PC you installed from: open for 30 minutes, **for that PC only**,
  proven with the key that signs your updates (`secrets/update.key`). This is also the way in when Home Assistant
  cannot be: the Echo is not adopted yet, has lost its key, runs `PROTO=wyoming`, or Home Assistant is down. It needs
  hassmic running (like `scripts/ota-push.sh`).
- or turn on **Debug access (adb over Wi-Fi)** in Home Assistant (the Echo's device page, Configuration), then
  `adb connect <echo-ip>:5555`. It closes by itself after 30 minutes, when you turn the switch off, and at every
  reboot. The switch only works once Home Assistant has set the encryption key (it does so when you add the Echo).
  The Echo cannot tell which PC you will connect from (the switch comes from Home Assistant's address), so this opens
  it to your **whole network**, unless `ADB_WIFI_FROM` in `hassmic.conf` names the address or subnet to admit.
- or put `ADB_WIFI=1` into `hassmic.conf`: open for good, until you take the line out (no reboot needed either way),
  to the whole network or to `ADB_WIFI_FROM`. For development, and the only way with `MODE=stock-online` (no hassmic
  running there).

`ADB_WIFI_FROM=192.168.1.20` (or a subnet, `192.168.1.0/24`) limits the switch and `ADB_WIFI=1` to that source. Written
as just that: a value that is not one IPv4 address or subnet keeps adb over Wi-Fi closed (`boot.log` says why) rather
than opening it to everyone. Nothing else opens it; `boot.log` says when it opens, for whom, and when it closes. If
hassmic itself does not run, or the update key is lost, only USB is left.

The push port (28929, updates and `scripts/adb-wifi.sh`) serves one connection at a time. An address whose
connections fail three times within a minute (no request, a wrong signature, too slow) is turned away for a minute,
so nobody can hold it for long by connecting over and over. A wrong key from your own PC three times in a row means
waiting that minute.

## Troubleshooting

Log: `adb shell tail -30 /data/local/hassmic/boot.log` (over USB, or over Wi-Fi after the "Debug access" switch or
`scripts/adb-wifi.sh <echo-ip>`).

**Wake word and button do nothing.** Most likely no connection to Home Assistant; the Echo does not signal that (known
gap). In the log, `wake: ALEXA type=2` means it heard you, `client connected` / `voice assistant: subscribed` means Home
Assistant is there. Nothing after the last `client disconnected`: check the network (`adb shell ifconfig wlan0`; can Home
Assistant reach that address?). Keep exactly one Wi-Fi profile on the Echo.

**No sound from replies or music.** The Echo fetches every reply, announcement and `play_media` from the URL Home
Assistant or Music Assistant gives it. Home Assistant builds that from its internal URL (Settings → System → Network),
or its LAN IP when none is set. The Echo must resolve the name (DNS from DHCP; `.local` via mDNS works) and route to the
address; on a network without internet that means a URL inside your network. The log names what failed
(`net: cannot ...`). If not even button sounds play, check the volume. Prefer Home Assistant's IP address in that URL
(`http://192.168.1.10:8123`) over `homeassistant.local`: any device on the network can answer an mDNS question, so
with a `.local` name a device that answers first decides where the Echo fetches what it plays.

**"Invalid encryption key" in Home Assistant** (Echo reset, or something else set a key first):
`scripts/adb-wifi.sh <echo-ip>`, `adb shell rm /data/local/hassmic/state/api_key`, restart hassmic (or reboot), delete the device in Home Assistant, add it
again.

Open issues and measurements: [PLAN.md](PLAN.md).

## Stock Alexa for a while

```sh
scripts/alexa.sh on      # stock Alexa with internet, hassmic off
scripts/alexa.sh off     # Alexa off, the satellite back
```

No reboot either way (on the Echo itself: `sh <dir>/alexa-on.sh`, `sh <dir>/alexa-off.sh`, `<dir>` being where
`main.sh` is). `on` stops the satellite and lifts the egress lock but keeps firmware updates blocked, exactly as
`MODE=stock-online` does: the update guard has to be in place before any of Amazon's services starts, otherwise the
satellite comes back instead. Alexa then needs internet (allow it at the router if the Echo sits in a VLAN without).
It lasts until `off` or the **next reboot**, which always brings the satellite back. While Alexa runs there is no
hassmic: no Home Assistant switch for adb over Wi-Fi, no `scripts/adb-wifi.sh`, no push updates. adb over Wi-Fi stays
open only for the rest of a window already open, or with `ADB_WIFI=1`; otherwise `off` needs USB, or reboot the Echo.
For Alexa across reboots use `MODE=stock-online` in `hassmic.conf` instead. Needs a version from 2026-10-06 on; an
older one installed is refused with a message.

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
- **Wyoming link**: unencrypted and unauthenticated, like every Wyoming satellite: whoever connects last is the Echo's
  Home Assistant and hears its microphone. Limit it to Home Assistant's address with `-W` in `ARGS`
  (`ARGS="-W 192.168.1.10"`): every other address is closed on at once. That is an address check, not a key: a device
  that takes over Home Assistant's address on your network still gets in. ESPHome (the default) is the recommended
  protocol for that reason.
- **Egress**: Amazon's daemons may only reach local addresses (plus DNS to the servers DHCP hands out); `otad` and
  `ace_otad` never get out. hassmic itself may reach any address. Put the Echo on a network without internet as a second
  layer.
- **Inbound**: TCP 16384–32767 only (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates), UDP 16384–32767
  (28930 arbitration between Echos).
- **adb**: a root shell without authentication (the unlock turns adbd's key check off). Over Wi-Fi it is closed: adbd
  runs without its network listener and the firewall drops port 5555. Opened only by `scripts/adb-wifi.sh` (30
  minutes, for the signing PC's address only; a fresh challenge signed with your update key, so a recorded exchange
  does not work twice), by the "Debug access" switch (30 minutes; taken only over the encrypted connection with Home
  Assistant's key) or by `ADB_WIFI=1` in `hassmic.conf`; the last two open it to the whole network unless
  `ADB_WIFI_FROM` names an address or subnet, and while it is open, whoever it admits has root. USB always works:
  physical access is root access anyway.
  Without `hassmic.conf` (stock behaviour, or before the install) it is open, as stock leaves it.
- **Arbitration between Echos**: an Echo takes the network key only from Home Assistant, over its encrypted API link,
  as a call of its own action `esphome.<node>_arbitration_key`; a member hands it over by asking Home Assistant to run
  that action, which needs "Allow the device to perform Home Assistant actions". So only devices you adopted into Home
  Assistant and allowed to act take part; the key travels encrypted to the receiving Echo, so it is not readable in
  Home Assistant's traces or logbook. Rounds are authenticated with the key and cannot be replayed. The keys are in
  `state/arb_key` and `state/arbitration`. Echos not yet in the network are remembered two per source address and
  eight new ones a minute at most, so a device beaconing made-up keys cannot crowd out one that is really joining
  (while such a flood lasts, a new Echo may take longer to join).
- **Updates**: only bundles signed with your `secrets/update.key` (pushed from your PC) or with the project's release key
  (`keys/release.pub`; downloaded by hassmic itself, only once "Online updates" is switched on) are installed. Root
  checks the signature with the tool and keys from the system partition or the installed copy before anything is
  unpacked. Your key also opens adb over Wi-Fi; the release key does not.
- **Bluetooth**: keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), both under `/data/local/hassmic/`.
  Legacy LE pairing (crackable when recorded) is allowed unless "Bluetooth proxy: secure pairing only" is on; AAC from
  phones, decoded by the firmware's old FFmpeg, only while "Bluetooth AAC" is on.
- **Ending processes** (task manager): hassmic cannot signal another user's process. It asks root through a file in
  `state/`, naming the process by pid and start time, so a request for one that has ended cannot hit another that got
  its number; root reads both again and refuses what keeps the Echo running, reachable or locked down (see
  [Task manager](#task-manager)). Only over the encrypted connection with Home Assistant's key.
- **Wi-Fi setup over Bluetooth**: the Echo advertises it only while it has no Wi-Fi (2 minutes after it lost it, then
  for 5 minutes) or after its action button was held 5 seconds, and takes a network only within a minute of a press of
  that button: someone has to be at the Echo, as with stock's setup. Improv has no encryption of its own: the password
  crosses the air in the clear to the Echo, within Bluetooth range and those few minutes, as with every Improv
  device. hassmic hands it to root in a file only root and hassmic can read; root checks it again and passes it to
  `wpa_cli` as data, never through a shell. Switch it off in Home Assistant if you do not want it.

## Development

Architecture, repository layout, building for the PC, tests and contribution notes: [DEVELOPMENT.md](DEVELOPMENT.md).

## Licence

[MIT](LICENSE), for everything written here. The files in `src/third_party/` keep their own licences, stated in each file:
monocypher (BSD-2-Clause OR CC0-1.0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0-1.0), `rnnoise/`
(BSD-3-Clause, `COPYING` beside it), `freeaptx.c`/`.h` (LGPL-2.1-or-later; hassmic links it statically, and everything needed to rebuild and relink it is in this repository).

Nothing of Amazon's is in this repository and nothing of it is covered by this licence: firmware, libraries and wake-word
models come from your own device and stay Amazon's. Not affiliated with or endorsed by Amazon, Home Assistant or
Music Assistant; "Alexa" and "Echo" are Amazon's trademarks.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
