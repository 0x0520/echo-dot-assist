# Installing from Windows (WSL2 + usbipd + Windows adb)

How an Echo Dot 3 (`donut`) was unlocked and installed from a Windows 11 laptop, 2026-10-06, branch
`feat/task-manager` (no CI build of that branch, so everything was built locally). The README's way (WSL2 with
usbipd-win) works for the unlock, but **adb file transfers through usbipd stalled**; adb on Windows, shared with WSL,
fixed that. What had to be done, in order.

## 1. WSL2 and Ubuntu

```bat
wsl --install                     :: admin PowerShell; restart Windows
wsl --install -d Ubuntu-24.04 --no-launch
```

The first `wsl --install` only enables the features: before the restart `wsl --status` says the "Virtual Machine
Platform" is missing. `--no-launch` creates no user; one was added as root, with passwordless sudo (the setup scripts
call `sudo -n` / `sudo -v`) and systemd (udev rules):

```sh
useradd -m -s /bin/bash -G sudo,plugdev <user>
echo '<user> ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/<user>; chmod 440 /etc/sudoers.d/<user>
printf '[user]\ndefault=<user>\n[boot]\nsystemd=true\n' > /etc/wsl.conf
apt-get install -y git make gcc unzip e2fsprogs sqlite3 curl coreutils bc xz-utils python3 python3-usb \
    python3-venv adb fastboot usbutils udev libopus-dev pkg-config flex bison libssl-dev libelf-dev
```

Calling WSL from PowerShell with `bash -c '...'` breaks on nested quotes (`(`, `"`); run script files instead
(`wsl -d Ubuntu-24.04 -- bash /mnt/c/.../script.sh`).

## 2. Clone, firmware, NDK, build (inside WSL)

Clone into the WSL file system (`~`), not `/mnt/c`. The three files from the donut README go into `firmware/donut/`
(check the sha256 values there). The NDK must be the **Linux** one (`android-ndk-r21e-linux-x86_64.zip`); copying an
unpacked NDK from `/mnt/c` is slow, downloading it again inside WSL was faster. Then step 2 of the donut README as
written (`unzip` payload, `tools/payload_dump.py`, `debugfs rdump`, `unzip boot-root`, `make`). Built in about a
minute; only the Wi-Fi motion kernel module is left out without `make kernel-tools`.

## 3. USB into WSL: usbipd-win

The Echo changes its USB identity on the way (bootrom `0e8d:0003`, preloader `0e8d:2000`, fastboot, adb
`18d1:4ee2`), and the bootrom waits only briefly, so it has to land in WSL without anyone typing `usbipd attach`.
What worked: an `AutoBind` policy per USB port (admin, once), then one auto-attach per port (no admin):

```powershell
# admin; leave out the ports of built-in devices (here 1-7 Bluetooth, 1-10 camera, see `usbipd list`)
foreach ($p in 1..16) { usbipd policy add --effect Allow --operation AutoBind --busid "1-$p" }
# normal user, one per port, keep running
usbipd attach --wsl --auto-attach --unplugged --busid 1-<port>
```

"Run with PowerShell" from the context menu is **not** elevated: `usbipd policy add` then fails with "Access
denied". Start it with `Start-Process -Verb RunAs`. Undo with `usbipd policy remove --guid <guid>` per rule.

## 4. Unlock (kamakiri) through usbipd: works, two catches

The bootrom handshake and the whole exploit (rpmb downgrade, tee/lk/kaeru/payload/preloader writes) went through usbipd
without a problem, at 0.2–0.5 MB/s.

- `bootrom-step.sh` asks for **Enter** ("If you have a short attached, remove it now") right after the handshake. Run
  without a terminal (in the background, from a tool), `input()` gets EOF, the thread dies and the main loop waits
  forever (it keeps kicking the watchdog, so nothing breaks; nothing has been written yet at that point). Feed exactly
  one newline: `printf '\n' | sudo ./bootrom-step.sh`. A second prompt ("rpmb looks broken") then gets EOF and waits
  in the same harmless way, so a human decides that one.
- A bootrom that already did its handshake does not do it again: after killing a run, **unplug the Echo's power**,
  then plug it in again with the action button held. A rerun that finds the old `0e8d:0003` hangs in "Handshake".
- `fastboot-step.sh` (TWRP into `recovery`) worked through usbipd as well.

## 5. adb through usbipd: transfers stall → use Windows adb

In TWRP, `adb shell` worked, `twrp wipe` worked, but `adb push` of the 112 MB firmware stalled with no progress after
a few seconds. After that the device went `offline`; after re-attaching, even `adb shell echo` and a 16 KB push timed
out (WSL `dmesg`: `vhci_hcd: unlink ... urb->status -104`, then a USB reset), and `adb reboot recovery` returned 0 but
never arrived. TWRP's adbd was wedged: power the Echo off and on with **Volume Up** held to get back into TWRP.

Fix: give the Echo back to Windows and use Google's platform-tools there.

```powershell
# stop the usbipd auto-attach processes, then (admin) release the port from usbipd
usbipd detach --busid 1-<port>
usbipd unbind --busid 1-<port>
# https://dl.google.com/android/repository/platform-tools-latest-windows.zip
adb.exe devices          # Windows already has a driver: "Echo Dot (3rd Gen), ADB Interface"
```

The firmware push then took 9 s (11.9 MB/s), checksum correct. Both slots and `boot-root` were installed from there
exactly as in the donut README (TWRP switched the active slot after each install: B, then A).

## 6. WSL scripts on Windows adb: mirrored networking

The PC scripts (`probe.sh`, `deploy.sh`, `wifi-join.sh`, `install-system.sh`) have to run in WSL, but adb only worked on
Windows. With WSL's mirrored networking, WSL's `adb` client finds the Windows adb server on `localhost:5037` by itself:

```ini
# %USERPROFILE%\.wslconfig, then: wsl --shutdown
[wsl2]
networkingMode=mirrored
```

Start the server on Windows first (`adb.exe devices`), and do not start one in WSL (no `sudo adb`, no
`adb kill-server` there). Ubuntu's adb 1.0.41 and platform-tools 37 speak the same protocol (both report 1.0.41).
`adb push` from WSL then sends files from the WSL file system through the Windows server, at full speed.

After that, steps 2 (`probe.sh`: no differences), 4 (`deploy.sh`, `lockdown.sh`, `wifi-join.sh`) and 5
(`install-system.sh "<name>"`) of the donut README ran unchanged. First boot: `self test: passed`, wake word heard.

## Afterwards

- Copy `secrets/update.key` out of WSL and keep it safe (it signs updates and opens adb over Wi-Fi).
- Remove the usbipd policies (`usbipd policy list`, `usbipd policy remove --guid ...`).
- Updates from then on go over Wi-Fi from WSL: `scripts/ota-push.sh <echo-ip>`; no USB needed.

## Ideas for the scripts

- `scripts/setup.sh` could detect WSL and use Windows adb (`adb.exe` through interop, or checking `localhost:5037`)
  instead of telling the user to pass USB through for everything; usbipd is only needed for the bootrom/fastboot part.
- `bootrom-step.sh` run from setup needs a tty or the one newline above.
