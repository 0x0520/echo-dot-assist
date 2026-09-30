# Changelog

What changed for people using the Echo, newest first. Details and measurements are in [PLAN.md](PLAN.md).

## 2026-09-30

- **The light ring shows when the Echo is ready to pair.** While the "Bluetooth pairing" switch is on, the ring runs
  Amazon's blue chaser, the one the stock Echo shows while it searches for devices. It stops when a phone has paired,
  when the two minutes are up or when you switch it off.
- **Updates now ask you to try them, then renew the Echo's fallback copy.** The copy on the system partition, which the
  Echo falls back to when an update does not come up, was the one from the day it was installed; changing it took USB
  and a trip through TWRP. `scripts/ota-push.sh` now pushes the update as before, asks you to try it, and when you say
  yes the Echo writes it over that copy, together with its start script and the tool that checks updates. Say no (or
  run it without a terminal) and it stays an update only; `scripts/ota-push.sh --approve <echo-ip>` approves it later.
  Echos installed before this change get there the same way, over Wi-Fi.
- **Installing no longer goes through TWRP.** `scripts/install-system.sh` writes the system partition while the Echo
  runs normally (one reboot instead of two); `--twrp` does it the old way, e.g. for an Echo that no longer starts.
- **adb over Wi-Fi is closed.** On an unlocked Echo adb is a root shell that asks for no key, and it was open to
  everyone on the network (issue #1). It is now closed at every boot and opened only on purpose: the new switch
  **Debug access (adb over Wi-Fi)** in Home Assistant opens it for 30 minutes (it closes by itself, or when you turn
  it off, or at a reboot). Without Home Assistant (an Echo not adopted yet, one that lost its key, Wyoming),
  `scripts/adb-wifi.sh <echo-ip>` does the same with the key that signs your updates. `ADB_WIFI=1` in `hassmic.conf`
  keeps it open. adb over USB works as before. **Keep `secrets/update.key`**: with neither it nor Home Assistant, only
  USB is left. Update with `scripts/ota-push.sh`; to keep adb over Wi-Fi as it was, add `ADB_WIFI=1` first.
- **`hassmic.conf` can only be changed by root.** It was writable by every user on some Echos, although the Echo runs
  what it says as root.

- **Commands no longer fade out after the first second.** Amazon's audio processing has to be told when a command is
  being spoken; otherwise it treats a voice that keeps talking as background noise and removes it after about 1.5
  seconds. The stock Alexa software did that, the Echo as a Home Assistant satellite did not: quietly spoken commands
  lost their second half (wrong words, or Home Assistant stopped listening mid-sentence). It does now, from the wake
  word until Home Assistant has heard the command.
- **New setting "Noise reduction"**: off (default), low, medium, high. Takes background noise out of what the voice
  assistant hears (RNNoise, by up to 6, 9 or 12 dB) before the volume is evened out. Worth trying if quietly spoken
  commands are misunderstood; more than that was audible as artefacts. It costs some processor time while a command
  is being heard.
- **The wake sound no longer makes the command quieter.** The Echo's own wake sound is still faintly in what the
  microphones pick up; the volume control took it for a loud talker and turned the command after it down.
- **Echo 2 and Echo Dot 2: the mute could show the wrong way round.** On these models the Echo only counted presses of
  the mute button. If it restarted (an update, for example) while the microphones were off, it started from "on":
  button lit and microphones cut, but no red ring and "unmuted" in Home Assistant, and every press wrong from then
  on. The Echo now reads the real state of the mute circuit, at start, on every press and once a second besides.
  Update with `scripts/ota-push.sh`. (Read on an Echo 2; on the Dot 2 the same file is expected but not yet seen.)
- **Echo 2: the firewall could fail to come up after a boot.** A helper of the firewall service could hang right at
  its start (a quirk of the Echo 2's system tools), and then the rule that keeps Amazon's software from reaching the
  internet was missing until the next boot, and push updates were not installed. Alexa and the firmware updaters
  were stopped all the same. Fixed; update the Echo 2 (`scripts/ota-push.sh`).
- **The firewall is now watched from a second place.** Should the firewall service ever fail again, the Echo notices
  within 20 seconds, puts the rule back itself, restarts the service and writes it into its log.
- **An Echo could be "unavailable" in Home Assistant after a boot while everything on it was running.** The Echo
  relies on a handful of Amazon's own firewall rules: the one that lets its traffic out, the ones that let Home
  Assistant, Music Assistant and answers in. Amazon's firewall script can lose any of its rules at boot, and nothing
  noticed: with the wrong one missing the Echo was cut off until the next boot
  ([issue #1](https://github.com/Gamer92000/echo-dot-assist/issues/1)). The Echo now checks all of its firewall
  every 5 seconds, not only that its own rule comes first: each of its own rules and their order, each of Amazon's
  rules it needs, that everything else inbound is still refused, and on models that cannot filter IPv6 that IPv6 is
  still off. What is missing or changed is put back within seconds, with a log line naming it. The Echo also loads
  its rules in one step now instead of some thirty: there is no moment in which they are half there, and it gets in
  the way of Amazon's script far less (about seven times fewer of its rules lost). Update with
  `scripts/ota-push.sh`.
- **Several Echos: better pick of the one that answers.** The Echos now compare the measurement Amazon's audio
  processing itself takes of each wake word (the one Alexa's cloud used), instead of one taken from the finished
  microphone stream. Echos on an older version still take part.

## 2026-09-29

- **Quiet speech is understood.** The Echo's microphones deliver speech far quieter than a Voice PE (about 30 dB):
  Amazon's cloud was tuned for that, Home Assistant's speech recognition and "finished speaking" detection are not, so
  softly spoken commands came out as wrong words or were cut off mid-sentence. The Echo now brings speech to a steady
  level itself before sending it, starting from how loud the wake word was, without raising the room noise in pauses
  and without clipping when someone speaks up close. The wake word is not affected.
- **One mic setting that works: "Mic level".** "Noise suppression level", "Auto gain" and "Mic volume multiplier" never
  had an effect: Home Assistant ignores them for ESPHome devices. They are replaced by "Mic level" (-35 to -15 dBFS,
  default -26, the usual reference level for speech): how loud speech reaches the voice assistant; raise it if quiet speech is still missed. Delete the three
  leftover entities in Home Assistant.

## 2026-09-28

- **Echo 2 (`radar`) supported.** Tried on a real Echo 2 with the guided setup (`scripts/setup.sh radar`). It has no
  USB socket: the setup shows where to solder the USB wires (TP13/14/15 on the amplifier/tweeter board) and that the
  power adapter is needed for the unlock. Its firmware is Fire OS 6572 (one build older than the Dot 3's): the same
  334 `libmixerAPI.so` exports as donut, the slimmer Pryon engine generation (as on `crumpet`) with all needed
  symbols. The keys are where the Echo Dot 2 has them. Bluetooth stays off for now (`-B`, written at install).
  Unlock zip and firmware go to `firmware/radar/`.
- **Music Assistant: paired servers only.** Music Assistant now has to pair with the Echo's token before it can play
  (an unpaired connection is encrypted under a key everyone knows). An Echo that played unpaired so far goes quiet in
  Music Assistant until it is paired, or until the new switch "Music Assistant without pairing" in Home Assistant is
  switched on (off by default; switching it off cuts off an unpaired server that is playing).
- **An adopted Echo no longer shows up as "discovered" again.** On the Echo 2 Wi-Fi comes up late in the boot, so the
  Echo announced itself with a placeholder MAC address and Home Assistant took it for a new device. The announcement
  now waits for Wi-Fi. Every Echo also announces its own host name (e.g. `echo-dot.local`) instead of `linux.local`,
  which Home Assistant showed next to the name. Names with umlauts become readable host names ("Küchen Echo" ->
  `kuechen-echo`).
- **Other wake words on every Echo.** `scripts/wakeword.sh` asks each Echo's engine which model sets it can load: the
  Echo 2's engine is older and gets its own.
- **Echo Dot 2 (`biscuit`) supported.** Tried on a real Echo Dot 2 with the guided setup (`scripts/setup.sh biscuit`):
  micro-USB, no soldering. Its pinned firmware (Fire OS 6574.1, the same build generation as `donut`) has
  `libmixerAPI.so` and `libpryon.so` byte-identical to donut's. Unlock files (R0rt1z2's amonet v2.0.0, `boot-root.zip`,
  firmware) go to `firmware/biscuit/`. It has no mute latch: the mute button is a key, toggled in software.
- **Bluetooth on the Echo Dot 2.** Its chip only knows Bluetooth 4.0 LE events and refused hassmic's start-up, so
  Bluetooth was off. hassmic now falls back to the 4.0 set: Bluetooth proxy and speaker mode start on the Dot 2
  (pairing uses the older LE method there, which the chip is limited to). `-B` leaves Bluetooth to the stock stack on
  a model where it does not work yet.
- **Firewall on Echos without IPv6 filtering.** The Dot 2's firmware has no `ip6tables`, so the lock could not cover
  IPv6 while the log claimed it did. There IPv6 is now switched off entirely; hassmic only uses IPv4.
- **Other wake words in one command.** `scripts/wakeword.sh <echo-ip>` puts "Echo", "Computer", "Amazon", "Ziggy" (or
  "Alexa" in another language) on an installed Echo. Models fetched once work on every Echo, so a second Echo needs
  no Amazon account at all: pick from the list, it checks the model on that Echo and restarts it. For a new one it
  does the Amazon part for you and only stops for registering and deregistering in the Alexa app; the update block
  stays on the whole time and everything is undone at the end. Then pick the wake word in Home Assistant. The guided
  setup offers the same as its last step.
- **Guided installation.** `scripts/setup.sh` is a terminal app: it recognises the Echo on adb, shows a progress bar
  and the step list, runs the steps one after the other and only stops when you have to do something (download,
  solder, hold a button, type a name). Downloads are picked up from `~/Downloads` by themselves and checked, missing
  tools are offered for install, the Android NDK is fetched without a question. Command output goes to
  `build/<codename>/setup.log`; you see it only when something fails. One typed `yes` at the start covers everything
  that wipes or flashes the Echo. You can stop at any point; it goes on where it left off. The install instructions
  moved from this README to a page per model: [devices/donut/README.md](devices/donut/README.md).
- **Setup ends with a finished satellite.** The install step asks the name, installs, waits until the Echo is up as a
  satellite, prints its address and tells you to adopt it. Model-specific hassmic arguments are written at install.
  The end screen shows the Sendspin pairing token and reminds you to allow the Echo to perform Home Assistant actions
  (needed for several Echos to agree which one answers).
- **Ready for more Echo models.** Everything that differs between models now sits in one folder per model under
  `devices/`, so other Echos can be added later. The 2018 Echo Dot 3 (`donut`) behaves exactly as before. If you
  build it yourself, two things move:
  - the firmware image, `kamakiri-donut-v1.0.0.zip`, `boot-root.zip` and what is unpacked from them go to
    `firmware/donut/`; move your existing `firmware/rootfs`, `firmware/images`, `kamakiri/` and `boot-root/` there;
  - the Echo binaries are built into `build/donut/`.

  The scripts that use adb now check which model is connected and that it runs the right firmware, and the installer
  refuses to write to any other. An Echo turns down a pushed update built for another model, once it has received one
  update of this version.
- The stock-online guard (install step 3) needs `devices/donut/device.conf` pushed next to `lockdown.sh`. Without it,
  the guard reports "OTA GUARD NOT ACTIVE" instead of running.

## 2026-09-25

- **Only one Echo answers, like Alexa.** With several Echos in earshot, only the one that heard "Alexa" most clearly
  answers; the others stay silent and dark. The Echos agree on it among themselves on your network in 0.2 s. An Echo
  you are already talking to, or that is ringing, keeps the wake word. With one Echo nothing changes and nothing waits.
  The Echos find each other by themselves (new "Join arbitration network" switch, on by default), and the shared key
  is handed from one to the next through your Home Assistant, so another device on the network cannot join or silence
  them. For that, each Echo needs "Allow the device to perform Home Assistant actions" ticked in its ESPHome options
  (Settings → Devices & services → ESPHome → the Echo → Configure), the same option the Bluetooth announcements use;
  Home Assistant shows a repair until it is. Renaming the Echo or its entities in Home Assistant does not matter. Each
  Echo needs its own `NAME` in `hassmic.conf`. Also on UDP port 28930.
- **Pick the wake word in Home Assistant.** The Echo's wake word select now lists every wake word installed on it (the
  stock "Alexa" plus any you fetched, such as "Echo"), and switching takes effect at once and survives restarts. Until
  now Home Assistant was only ever shown "Alexa", even when the Echo actually listened for "Echo".
- **The microphone comes back by itself.** An Echo could stop hearing anything after hours of running (the wake word
  did nothing, the buttons still worked) until hassmic was restarted. It now notices within a few seconds and
  reconnects the microphone.
- **No red flash on the second satellite.** When another voice satellite reports the wake word first, Home Assistant
  lets only that one answer. The Echo that came second used to show the error light; now it just goes quiet.

## 2026-09-24

- **Bluetooth announcements in your language.** "Connected to …" and "Disconnected from …" can now be said in German,
  French, Spanish, Italian, Portuguese, Dutch, Swedish, Danish, Norwegian, Finnish or Polish instead of English: pick
  it in the new "Bluetooth announcement language" setting in Home Assistant, to match the language of the Echo's
  assistant. It cannot follow the assistant by itself because Home Assistant does not tell the Echo which language
  that is. The choice survives restarts.
- **Equalizer, like the Alexa app's.** Three new sliders in Home Assistant: "Equalizer bass", "Equalizer mid" and
  "Equalizer treble", each from −6 to +6 dB. They use Amazon's own equalizer inside the Echo, so they shape everything
  it plays: replies, music from Music Assistant or a phone, and sounds, on the built-in speaker and on the 3.5 mm
  output alike. The Echo keeps the setting itself, so it stays after a restart.
- **Do not disturb, like Alexa's.** A new "Do not disturb" switch in Home Assistant. While it is on, announcements
  (`assist_satellite.announce`, "ask a question") are not played. Everything you start yourself still works: the wake
  word, replies, timers, music and the Bluetooth "Connected to …" message. Turning it on shows Alexa's single purple
  pulse on the ring. The setting survives restarts. For a schedule, use a Home Assistant automation, and you can switch
  it by voice if the switch is exposed to Assist.
- **"Connected to <phone>" like Alexa.** When a phone or computer connects to the Echo as a Bluetooth speaker, the Echo
  plays Amazon's Bluetooth chime and says "Connected to" and the device's name; on disconnect the other chime and
  "Disconnected from …". The words come from Home Assistant's text-to-speech, so Home Assistant has to let the Echo
  ask for it: Settings → Devices & services → ESPHome → the Echo → Configure → tick "Allow the device to perform Home
  Assistant actions" (until then only the chime plays, and Home Assistant shows a repair about it). The new
  "Bluetooth announcements" switch turns both chime and words off.
- **Bluetooth speaker again.** Phones and computers can play to the Echo over Bluetooth, beside the Bluetooth proxy.
  To pair, turn on the new "Bluetooth pairing" switch in Home Assistant and pick the Echo on the phone within two
  minutes; afterwards the phone connects by itself whenever you choose the Echo. Music from the phone is ducked while
  you talk to the assistant, and the wake word listens through it like through other music.
  Codecs: SBC like stock Alexa, and in addition AAC (what iPhones, iPads and Macs use), aptX and aptX HD. While a
  phone plays, the Echo stops scanning for Home Assistant's Bluetooth devices: the radio cannot do both without the
  music stuttering.
  The phone's volume slider moves the Echo's volume and the other way round, and the action button pauses and resumes
  the phone.
- **One music source at a time.** When a phone starts playing over Bluetooth, Music Assistant pauses (the whole group
  the Echo is in); when Music Assistant starts playing on the Echo, the phone pauses. Verified with Music Assistant and
  a Pixel.
- **Bluetooth proxy for Home Assistant.** The Echo now scans for Bluetooth LE devices and passes what it hears to Home
  Assistant, like an ESPHome Bluetooth proxy. After updating, Home Assistant picks it up by itself: the Echo appears
  under Settings → Devices & services → Bluetooth, and BLE sensors, trackers and beacons in range show up.
  Integrations that have to connect to a device can do so through the Echo too, up to 3 devices at a time, and pair
  with it where the device allows pairing without a PIN (like ESPHome's proxies). Paired devices are remembered across
  restarts. Amazon's Bluetooth service is stopped for this, so the Echo no longer works as a Bluetooth speaker (without
  Alexa nothing could pair with it anyway).

## 2026-09-23

- **No more silent speaker after Alexa's mute.** Amazon's mixer has a global mute that silences every sound whatever the
  volume, and it survives reboots, so an Echo muted under stock Alexa stayed silent under hassmic: no replies, no music,
  no sounds. hassmic now clears it when it finds it set.
- **Replies and music from wherever Home Assistant points.** hassmic may now connect to any address, not only local
  ones, so a Home Assistant reached by a public domain, a Tailscale address or IPv6 works too. Amazon's own services
  stay locked to the local network. Safe now that only the paired Home Assistant can tell the Echo what to fetch.
- **Encrypted connection to Home Assistant.** The ESPHome connection now uses the same encryption as ESPHome devices.
  Home Assistant creates the key by itself when the Echo is added, and from then on only Home Assistant can connect.
  Already added? Home Assistant sets the key on its next connection, nothing to do. See "Encryption key" in the README
  if it ever needs a reset.
- **Replies and music from a Home Assistant with a host name.** The Echo now finds `.local` names (like
  `homeassistant.local`) by mDNS, may ask the DNS servers the network hands out even when one is public (8.8.8.8 from
  DHCP used to be dropped), and tries every address a name resolves to instead of giving up after the first. Before, an
  internal URL with a name instead of an IP could leave the Echo silent at any volume. What still has to hold is in the
  README under "No sound from replies or music?".
- **No more 20 s of deafness after a restart or update.** Amazon's mixer waits for its performance monitor before it opens
  the mic, and the lockdown used to stop that daemon. It now keeps running, so the wake word listens again right after
  hassmic starts.
- **Alexa's original sounds** for the wake word, the action button, the volume keys and the mic-off button (mics off / mics
  on), taken from the stock firmware on the device. The generated blip is gone. The "Wake sound" switch in Home Assistant
  silences all of them.
- **Button sounds no longer drop out.** About a third of them were lost to a race in Amazon's mixer library; playback now
  waits for the mixer to take the sound before closing the stream.
- **Volume stays consistent.** Replies play on a separate volume (`TTSVolume`) that only followed the main volume when
  hassmic changed it. It is now kept in line every 2 s, and a main volume changed from outside is reported to Home
  Assistant and Music Assistant.
- **Diagnostics in Home Assistant**: SoC temperature and CPU usage as sensors, disabled by default. Enable them in the
  entity settings of the Echo Dot device.
- For developers: `kill -TTIN $(pidof hassmic)` writes what the wake word hears to `state/capture.raw`, the next one stops
  it. The only way to record the processed mic stream while hassmic runs.

## 2026-09-22

- **Wake word during alarms and playback.** hassmic now tells Amazon's wake word model when a timer rings, music plays or a
  reply is spoken, and the model switches to the lower accept threshold it carries for those moments ("Echo" needs a score
  of 0.45 instead of 0.75 while an alarm rings). Stopping an alarm or interrupting music by voice works from further away.
