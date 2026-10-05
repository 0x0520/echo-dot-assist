# Changelog

What changed for people using the Echo, newest first. Details and measurements are in [PLAN.md](PLAN.md).

## 2026-10-06

- **Stock Alexa back without a reboot, and away again.** `scripts/alexa.sh on` stops the satellite and starts Amazon's
  Alexa with internet, firmware updates still blocked; `scripts/alexa.sh off` brings the satellite and the internet
  lock back. A reboot also returns to the satellite. Before, `alexa-on.sh` left the lock in place (Alexa could not
  reach Amazon), started the firmware updaters, which only that lock kept offline, and on an installed Echo the
  satellite stopped Alexa again within seconds.
- **Alarm clock on the Echo.** Three alarms, set in Home Assistant (time, on/off, and once, every day, weekdays,
  weekends or one weekday), ring on the Echo itself for up to 10 minutes, also when Home Assistant or the network is
  down. Stop them with the action button, "Alexa, stop" or the "Stop alarm" button; "Snooze alarm" rings again in 9
  minutes. "Alarm ringing", the "Alarm" event and "Next alarm" are there for automations. The Echo takes the time and
  time zone from Home Assistant (summer time included); after the Echo restarts, alarms ring only once Home Assistant
  has told it the time again.
- **New Wi-Fi without a PC: Wi-Fi setup over Bluetooth.** When the Echo has had no Wi-Fi for 2 minutes, or after you
  hold its action button for 5 seconds, it offers [Improv Wi-Fi](https://www.improv-wifi.com/) over Bluetooth for 5
  minutes (orange spinner on the ring). Home Assistant's Improv integration or the Improv web app then asks you to press
  the action button and hands over the network name and password; the Echo joins and keeps the network. A wrong
  password leaves the Echo on the network it had. A switch in Home Assistant, "Wi-Fi setup over Bluetooth" (on by
  default), turns it off. See [Wi-Fi setup over Bluetooth](README.md#wifi-setup). **Not tried on an Echo yet.**
- **Alarms are heard at volume 0.** An alarm rings at 30 % at least, and the volume goes back to what it was once it
  stops (unless you changed it meanwhile). Timers ring at the volume as it is, as before.
- **New switch "Bluetooth AAC", off by default.** Phones now play over SBC or aptX unless you switch AAC on: AAC is
  decoded by the Echo's old built-in FFmpeg, which is fed whatever arrives over the radio. On the Echo's speaker the
  difference is small. A change counts from the phone's next connection.
- **New switch "Bluetooth proxy: secure pairing only", off by default.** On, the Bluetooth proxy pairs only devices
  that can do LE Secure Connections and no longer uses older "legacy" pairings, whose keys can be worked out by anyone
  who recorded the pairing. Off, nothing changes, so older sensors keep working.
- **Hardened the boot script further** against a compromised hassmic: the mDNS service file is now made by hassmic
  running as its own user (it ran as root) and put in place by a rename, so a link left in its directory cannot make
  root write elsewhere; and a directory planted where root puts an update's result no longer swallows it.
- **A client with a bad connection no longer stalls the Echo.** When one of the connected clients (Home Assistant, or a
  second one such as a debugging tool) stopped taking data, for example because its Wi-Fi dropped, the wake word,
  the buttons and every other client could hang for up to 5 seconds. Each client now gets what is sent to it from a
  queue of its own; one that falls far behind is disconnected and reconnects.

## 2026-10-05

- **Updates from a Windows PC can no longer break the boot.** A checkout on Windows turned the line endings of the
  Echo's boot scripts into CR LF, which its shell reads as part of every word. The repository now keeps them LF on every
  PC, `scripts/bundle.sh` refuses to pack such a file, and the Echo refuses to install a bundle whose scripts carry CR LF
  or do not parse, saying so in the push result.
- **A broken `hassmic.conf` no longer takes the firewall down.** A stray quote or an `exit` in a hand-edited config now
  means "no satellite" and a line in `boot.log`; the egress lock comes up regardless. A config with Windows line endings
  is converted. A name given to `scripts/install-system.sh` may contain quotes and `$`.
- **Falling back to the last working version sticks.** After an update failed to come up, the factory copy reset the
  start counter a minute later, and the next boot tried the broken update again. Only the update's own self test
  (started, wake word engine, a second of microphone audio) now counts as it coming up, so an update that hangs falls
  back after three boots too.
- **A test binary left by `scripts/deploy.sh` no longer hides updates.** It went on running after every push or online
  update, and the update never became the fallback; installing an update now removes it.
- **Hardened the root side of updates** against a compromised hassmic: root no longer writes the update result through
  a name in hassmic's own directory, which could have been made to point at a file root then runs.
- **No going back to an older release behind your back.** The Echo now refuses a build signed with the project's
  release key that is older than the version it runs or falls back to: every release ever published carries a valid
  signature, so an old one handed in again could have put back a problem a later release fixed. Switching online
  updates from `beta` to `release` keeps the newer beta until a newer release is out. Going back on purpose still
  works: push the older version from your PC with `scripts/ota-push.sh` (your own update key).
- **The PC scripts say right away when they are started on Windows** (Git Bash, MSYS2, Cygwin) and point to WSL2,
  instead of failing halfway through. `scripts/ota-push.sh` with a published release build and `scripts/adb-wifi.sh`
  keep working there. `scripts/install-system.sh` now also installs the `latency` tool, as updates already did.
- **Stopping the media player while an announcement or reply is still loading no longer silences the Echo.** Every
  reply after such a stop used to stay silent until hassmic restarted; now the stop drops only what it was meant for.
- **A Wi-Fi hiccup no longer makes the Echo deaf.** The microphone audio for Home Assistant is now sent by a thread of
  its own: while the network stalls, the wake word, the buttons and everything else carry on, and the Echo drops the
  audio Home Assistant could not take instead of stopping.
- **A stock helper tool that hangs can no longer stall the Echo.** The tools hassmic runs for the volume, the
  equalizer and the wake word's loudness are stopped after a second.
- **Fewer background processes:** the volume is checked with one helper every 2 s instead of three, the rest once a
  minute.
- **A wake word model that does not load** falls back to "Alexa" cleanly, and Home Assistant then shows "Alexa" as the
  active wake word instead of the one that failed.
- **Replies are no longer held in memory without limit:** a client sending audio much faster than it plays (Wyoming
  has no login) is slowed down instead.
- **A sound asked for while a timer rings plays at once**, not after the pause between the beeps.
- **A Wyoming reply cut off by Home Assistant reconnecting is ended properly:** the Echo no longer keeps the speaker
  stream open and the wake word at its "something is playing" threshold until the next reply.
- **Nothing on the network can lock Home Assistant out of the Echo any more.** Four connections that never said
  anything used to take every place the Echo has for Home Assistant until they closed; now a connection has 10 s to
  start talking, and a new one pushes the longest silent one out. With Wyoming a new connection replaces the old one,
  so a stray idle connection no longer keeps Home Assistant waiting for ever.
- **Push updates and the adb way back in can no longer be held up by a slow sender.** A connection to the update port
  gets a fixed time for each step (10 s for the request, the upload in proportion to its size) instead of 30 s per
  byte, so a trickle of data can no longer keep that port busy.
- **When Home Assistant sets the encryption key, connections made before it are closed at once.** Until their next
  request they still received every state, the Sendspin pairing token among them.
- **Forged arbitration packets cost little now.** Another Echo is handed the network key only after it has been heard
  twice, ten seconds apart, and all hand-overs together at most every 5 s; joining an arbitration network takes about
  10 s longer (two networks merging, up to a minute).
- **Sturdier playback of announcements and replies:** a cancelled reply that was still downloading could garble the
  next one; odd WAV and Wyoming formats the Echo cannot play are refused instead of being handed to the mixer. A
  `homeassistant.local` address is looked up once and remembered, and only answers from the local network count.
- **Music Assistant (Sendspin) is harder to disturb from the network.** With "Allow Music Assistant without pairing"
  off, anyone on the network could still connect with the public key, claim to be pairing and push your paired Music
  Assistant off the Echo, then play or change the volume; now such a connection can only pair. A server that stops
  reading can no longer freeze the Echo (volume buttons included), connections that never finish connecting are dropped
  after 10 seconds, and at most four connect at a time, so a flood of them cannot lock your Music Assistant out.
- **Bluetooth is harder to attack.** A phone or device in radio range could crash hassmic with crafted AAC audio, and
  a device pretending to be a paired one could make the Echo forget the real pairing and pair itself in its place.
  Both are closed, along with a series of smaller checks on what other Bluetooth devices send (see PLAN.md).
- **A forgotten Bluetooth pairing is no longer repaired behind your back.** If a speaker the Echo plays on is reset,
  run "Bluetooth speaker search" again; a phone that forgot the Echo pairs again with "Bluetooth pairing" on, as
  before. For the Bluetooth proxy, Home Assistant's "pair" pairs such a device afresh. Before, the Echo deleted the
  pairing on the other side's word and re-paired with whoever answered.
- **Bluetooth proxy pairing needs full-length keys** (16 bytes), which every current device offers.
- **Whisper detection: one download, the one that exists.** Amazon has a single whisper model for every language
  and hands it out only when asked for American English, so `scripts/artifacts.sh` now asks for just that, whatever
  language is picked. Before, it asked for the picked language first and took the English one as a fallback.
- **`scripts/artifacts.sh` says plainly at the end when whisper detection was ticked but not installed.** The log of
  the run before is kept as `build/artifacts.log.1`, so a second try no longer wipes the record of the first.
- **Downloads from Amazon work with older Python 3** (e.g. Ubuntu 20.04's 3.8): unpacking a model failed there after
  the download.
- **`scripts/artifacts.sh` no longer downloads with a dead registration.** An Echo could still carry the registration of
  an earlier run, with a token Amazon refuses; the script took that for "registered already" and every download
  failed. It now asks Amazon whether the token works, and otherwise has you set the Echo up in the Alexa app.
- **`scripts/artifacts.sh` asks you to deregister the Echo only once it is a satellite again.** It asked before,
  while the Echo still ran as a stock Echo online, and a stock Echo resets itself to factory settings when
  deregistered: hassmic's settings, its Home Assistant key and the wake word models were gone, and the Echo stayed a
  plain Alexa. If that happened to yours: block the Echo's internet at the router, then
  `adb shell 'mkdir -p /data/local/hassmic; printf "NAME=\"Echo Dot\"\nARGS=\"\"\n" > /data/local/hassmic/hassmic.conf'`
  and `adb reboot`, add it to Home Assistant again, and run `scripts/artifacts.sh` for the wake words.
- **`scripts/artifacts.sh` tells you when the Echo's adb hangs.** After the Alexa app moved an Echo Dot 3 to another
  Wi-Fi network and back, adb over Wi-Fi took the connection but never answered, and the script waited forever. It
  now says to unplug the Echo's power and plug it back in. An Echo left waiting for the Alexa app (orange ring) by a
  run stopped halfway is explained too.

## 2026-10-04

- **Whisper detection.** Whisper to the Echo, and Home Assistant knows: the binary sensor "Last request whispered" is
  on when your last request was whispered. Use it in your conversation agent's instructions to have the answer
  whispered as well, as a stock Echo does ([how](README.md#whisper)). It is Amazon's own detector, run on the Echo
  with a model that only Amazon hands out: the guided setup's last step offers it next to the wake words, and
  `scripts/artifacts.sh` ("Other artifacts") adds it to an Echo set up before. ESPHome only.
- **Newest sound detection model.** `scripts/artifacts.sh` and the guided setup can install Amazon's newest sound
  detection model; hassmic then takes it in place of the one in the firmware (and goes back to that one if it does not
  load). Not ticked by default: in tests it scored the same.
- **`scripts/artifacts.sh` over Wi-Fi no longer hangs** after restarting the Echo as a stock Echo. A stock Echo
  that is not registered drops your Wi-Fi and opens its own setup network, so it now asks you to set it up in the
  Alexa app first and waits for it to come back afterwards. With the Echo on USB as well, it uses USB.
- **`scripts/artifacts.sh` menus** no longer get garbled when an entry is wider than the terminal.

- **Cancel a request with the action button**, as with the center button of a Voice PE: pressed while Home Assistant is
  still listening or thinking, the request is aborted, the conversation agent included, so a misheard command does not
  go on to switch things it should not (a tool call already under way still finishes). The wake word said while it
  thinks cancels too and starts a new request straight away. Before, both worked only once the reply was being
  spoken. ESPHome only (Wyoming has no way to abort a request).

## 2026-10-03

- **Install without compiling anything.** On a commit that GitHub has a build of (every commit on `main` and
  `release` once CI has published it), `scripts/setup.sh` offers that build: no Android NDK (1 GB), no compilers, no
  unpacking of the firmware, and fewer tools to install. It is the build online updates install, and it is checked
  against the project's release key before anything uses it. With changes of your own in the checkout it builds as
  before. `deploy.sh`, `install-system.sh` and `ota-push.sh` take that build too when there is no NDK on the PC
  (`PREBUILT=1` to insist on it, `PREBUILT=0` to always build).
- **Update keys, push updates and `adb-wifi.sh` no longer need a C compiler on the PC.** The PC's side of the update
  tool is Python now (`scripts/otatool.py`). Keys, signatures and bundles are the same as before; nothing changes on
  the Echo.

- **`scripts/install-system.sh` no longer stops silently on a build without the Wi-Fi motion module** ([issue
  #5](https://github.com/Gamer92000/echo-dot-assist/issues/5)). That module is optional and is only built when the
  kernel source and its toolchain are there. Without it, the install quit before writing anything and gave no message.
  It now installs without the module. If you hit this, run the install again.

- **Guided setup: Wi-Fi motion's kernel module is now part of the build.** Until now only released updates had it.
  The downloads step now also fetches the kernel sources and the compiler the module is built with (115 MB, checked
  against fixed checksums), so a setup build matches the released one. Building by hand: `make kernel-tools`. The
  build needs `bc`, which the setup offers to install along with the other tools.

- **Guided setup: Echo Dot 3 unlock fixed.** `scripts/setup.sh` stopped at "Waiting for the Echo's bootrom" with
  "./bootrom-step.sh: No such file or directory": the kamakiri zip unpacks into a folder of its own, and the step looked
  for its scripts one level too high. It now finds them wherever the zip puts them. If you hit this, run
  `scripts/setup.sh` again; nothing needs deleting. The Echo Dot 2 and Echo 2 steps find amonet the same way now, in
  case a later zip is laid out differently.

- **Guided setup: the Echo Dot 3 unlock no longer hangs after the handshake** ([issue
  #4](https://github.com/Gamer92000/echo-dot-assist/issues/4)). kamakiri waits for Enter right after it reaches the
  Echo's bootrom, and the setup gave it no keyboard, so it waited forever with the ring dark. It now runs in front of
  you: hold the dot button, plug in, and when it asks, release the button and press Enter. Nothing is written to the
  Echo before that point, so an Echo stuck there is unchanged; unplug it and run `scripts/setup.sh` again.

- **Guided setup: the Echo Dot 3's build step unpacks the firmware again** ([issue
  #4](https://github.com/Gamer92000/echo-dot-assist/issues/4)). On a PC where `firmware/donut/rootfs` did not exist yet,
  the unpack silently wrote nothing and the build then stopped with "missing .../libmixerAPI.so". The folder is now
  created first, and the step fails if the firmware did not come out. The manual steps in the README had the same gap.

- **`scripts/probe.sh` says when there is no Echo on adb** instead of listing every file as different. A stock Echo
  has no adb until it is rooted, so this check only works after that.

## 2026-10-02

- **Online updates from Home Assistant** ([issue #3](https://github.com/Gamer92000/echo-dot-assist/issues/3)), off by
  default. A new "Online updates" select picks a channel: `release` (releases only), `beta` (every build of the main
  branch, plus releases) or `off`. The Echo's "Firmware" update entity then shows when there is something newer and
  installs it with one click, without a PC. The builds come from the project's GitHub releases; the Echo only installs
  them if they are signed with the project's release key, and goes back to the previous version by itself if the new
  one does not stay up. Echos installed before this need one push from the PC (or a fresh install) first, to bring that
  key along. Versions are now the date and time of the build's code (UTC), like `2026.10.02.091530`.

- **Updates become the fallback by themselves.** Every update, pushed from the PC or installed from Home Assistant,
  checks itself as it starts (wake word engine, network ports, a second of microphone audio) and, once that passes, is
  what the Echo falls back to from then on. `scripts/ota-push.sh` no longer asks, and `--approve` is gone.

## 2026-10-01

- **Wi-Fi motion, experimental.** The Echo Dot 3, Echo Dot 2 and Echo 2 can now work as a motion sensor, from their
  Wi-Fi signal: someone walking between the Echo and the router changes it. Switch on "Wi-Fi motion detection
  (experimental)" (off by default); "Wi-Fi motion (experimental)" then shows motion in Home Assistant, and "Wi-Fi motion
  sensitivity (experimental)" sets how much it takes. It notices movement, not someone sitting still, and has been
  tried for a few minutes and one night, so expect false alarms; see the README. It works through a small kernel
  module that reads the signal of every frame from your router; the module comes with the update and is only loaded
  once you switch Wi-Fi motion on.

- **First install fixed** ([issue #2](https://github.com/Gamer92000/echo-dot-assist/issues/2)). Installing on an
  Echo for the first time stopped at the "Install" step ("No such file or directory", then "Permission denied" for
  `/sepolicy.new`); only Echos installed before 2026-09-30 got through, because they had run the older installer. Fixed,
  with two smaller hiccups of the guided setup: the firmware unpacking failing on a missing `images` folder, and the
  lockdown failing when adb was gone for a moment. An Echo that is unlocked already is now taken straight to TWRP by
  the "Unlock" step instead of having to be marked done by hand.

- **Play on a Bluetooth speaker.** The Echo can now send everything it plays to a Bluetooth speaker, as a stock Echo
  can. Put the speaker in pairing mode and switch on "Bluetooth speaker search" in Home Assistant; "Play on Bluetooth
  speaker" switches between the speaker and the Echo, and the Echo reconnects by itself. While on the speaker, the
  volume buttons and Home Assistant set the speaker's own volume (shown on the light ring); the Echo's volume comes back
  when you switch back. Music Assistant keeps it in time with other players through "Bluetooth speaker delay".

- **Sound detection, optional.** A new "Sound detection" switch (off by default) runs Amazon's Alexa Guard model on
  the Echo itself, and a "Sound" entity in Home Assistant reports what it heard: smoke or CO alarm, breaking glass, a
  dog barking, a baby crying, snoring, coughing, water, a beeping appliance. It is less reliable than on a stock Echo,
  where Amazon's cloud checks every hit first, and it takes up to 10 s; see the README before you rely on it.

- **`scripts/wakeword.sh` is now `scripts/artifacts.sh`, and asks once.** Its menu has an entry per kind of
  artifact: "Wake words" (to install, from the PC or downloaded from Amazon) and "Other artifacts" (Alexa Guard's
  sound detection model, only kept on the PC for tests; Home Assistant does not use it yet), each a list of ticks with
  everything new ticked, plus the language for Amazon downloads. "Go on" shows what will happen, asks once, and then
  does all of it with a single Amazon registration.

- **The Echo's light sensor in Home Assistant.** A new "Illuminance" sensor reports the room's light in lux, the
  reading the stock Echo uses to dim its light ring. The ring keeps dimming with the room as before (that is Amazon's
  own code on the Echo, on by default); the new "LED brightness" slider holds it at a level of your choice instead, and
  "LED auto brightness" hands it back to the light sensor. Both are kept across restarts.

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
