#!/usr/bin/env python3
"""Plays Home Assistant's side of the ESPHome native API against build/hassmic-host, using the reference
`aioesphomeapi` client (the library Home Assistant itself uses), so framing and protobuf layout are checked by the real parser."""
import asyncio, base64, datetime, io, math, os, random, signal, socket, struct, subprocess, sys, tempfile, threading, time, wave
from zoneinfo import ZoneInfo
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import SelectInfo, SelectState, NumberInfo, SwitchInfo, NumberState, SwitchState, TextSensorInfo, TextSensorState, SensorInfo, SensorState
from aioesphomeapi import APIClient, MediaPlayerInfo, MediaPlayerEntityState, VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
from aioesphomeapi import ZERO_NOISE_PSK, EventInfo, BinarySensorInfo, BinarySensorState, MediaPlayerCommand, TimeInfo, TimeState, ButtonInfo
from aioesphomeapi.model import Event
from aioesphomeapi.core import InvalidEncryptionKeyAPIError, RequiresEncryptionAPIError

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, HTTP_PORT = 16953, 16954


def tone(rate, seconds, freq=440):
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(int(rate * seconds)))


def wav_bytes(rate, seconds):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(tone(rate, seconds))
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if "slow" in self.path: time.sleep(1.5)         # TTS still rendering: not even the headers yet
        body = wav_bytes(48000, 3.0 if "s=3" in self.path else 0.5)
        if self.path.endswith(".mp3"):                  # what Home Assistant sends for a TTS announcement before any pipeline ran
            body = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-ar", "24000",
                                   "-ac", "1", "-f", "mp3", "-"], capture_output=True, check=True).stdout
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()   # no length: like a transcoding proxy
        if "late" in self.path:                         # streamed TTS while the LLM still works: the audio comes later
            self.wfile.flush(); time.sleep(6)
        try: self.wfile.write(body)
        except OSError: pass                            # the Echo hung up
    def log_message(self, *a): pass


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def main():
    play = tempfile.mktemp(suffix=".raw")
    settings = tempfile.mktemp(suffix=".settings")
    state = tempfile.mkdtemp(); mdns = os.path.join(state, "hassmic.service")
    with open(settings, "w") as f: f.write("3 9 4.00 0 1 1 0 en\n")         # from before the mic level: gain values for HA
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_SETTINGS=settings, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=play,
               HASSMIC_MDNS_FILE=mdns, HASSMIC_ARB_ADDR="127.255.255.255",      # arbitration beacons stay on this PC
               HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),          # on the Echo: in a directory only root writes
               HASSMIC_LUX=os.path.join(state, "calibrated_lux"),              # the light sensor's sysfs file
               HASSMIC_FAKE_SOUND="dogBark",                                    # every ~10 s window "hears" a dog (sound_none.c)
               HASSMIC_FAKE_WHISPER="1",                                        # a model, and every request whispered (whisper_none.c)
               HASSMIC_FAKE_WIFI=os.path.join(state, "rx_stat"))                # what the Wi-Fi driver answers RX_STAT (wifimotion.c)
    rx_stat = lambda rcpi: open(env["HASSMIC_FAKE_WIFI"], "w").write(f"RX Stat:\nRX SNR (dB)          = 32\nRCPI RX0             = {rcpi}\n")
    rx_stat(112)
    with open(env["HASSMIC_LUX"], "w") as f: f.write("67\n")
    for m in ("echo-de", "computer-en-US"):             # installed wake word models (the PC build loads none of them)
        os.makedirs(os.path.join(state, "models", m)); open(os.path.join(state, "models", m, "pryon.manifest"), "w").close()
    with open(mdns, "w") as f:                          # what main.sh does at boot
        subprocess.run([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-S"], env=env, stdout=f, check=True)
    log = []                                            # hassmic's stderr, passed on and kept: what it does without a client
    def start_hassmic():
        p = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-L"], env=env,
                             stderr=subprocess.PIPE, text=True, bufsize=1)
        def tee():
            for line in p.stderr: sys.stderr.write(line); log.append(line.rstrip("\n"))
        threading.Thread(target=tee, daemon=True).start()
        return p
    proc = start_hassmic()
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    await asyncio.sleep(0.5)
    try:
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        info = await cli.device_info()
        check(info.name == "echo-dot" and info.friendly_name == "Echo Dot", f"device info: {info.name!r} / {info.friendly_name!r}")
        check(info.voice_assistant_feature_flags == 61, f"voice assistant feature flags = {info.voice_assistant_feature_flags}")
        entities, _ = await cli.list_entities_services()
        mp = [e for e in entities if isinstance(e, MediaPlayerInfo)]
        check(len(mp) == 1 and len(mp[0].supported_formats) == 2 and mp[0].supported_formats[1].sample_rate == 48000,
              f"media player entity with formats: {[(f.format, f.sample_rate, int(f.purpose)) for f in mp[0].supported_formats] if mp else None}")
        states = []
        cli.subscribe_states(states.append)
        by = {e.object_id: e for e in entities}
        check("noise_suppression_level" not in by
              and "auto_gain" not in by and "mic_volume_multiplier" not in by
              and isinstance(by.get("mic_level"), NumberInfo) and (by["mic_level"].min_value, by["mic_level"].max_value) == (-35, -15)
              and isinstance(by.get("mute"), SwitchInfo) and isinstance(by.get("wake_sound"), SwitchInfo)
              and isinstance(by.get("noise_reduction"), SelectInfo) and list(by["noise_reduction"].options) == ["Off", "Low", "Medium", "High"],
              "settings entities listed")
        tok = by.get("sendspin_pairing_token")
        check(isinstance(tok, TextSensorInfo) and tok.disabled_by_default and int(tok.entity_category) == 2, "Sendspin pairing token entity: diagnostic, disabled by default")
        temp, cpu = by.get("soc_temperature"), by.get("cpu_usage")
        check(isinstance(temp, SensorInfo) and temp.disabled_by_default and int(temp.entity_category) == 2 and temp.device_class == "temperature"
              and temp.unit_of_measurement == "\u00b0C" and isinstance(cpu, SensorInfo) and cpu.disabled_by_default and cpu.unit_of_measurement == "%"
              and int(cpu.state_class) == 1, "diagnostic sensors: SoC temperature and CPU usage, disabled by default")
        await asyncio.sleep(0.3)
        level0 = [x.state for x in states if isinstance(x, NumberState) and x.key == by["mic_level"].key]
        check(level0 == [-26], f"mic level at its default after a settings file from before it: {level0}")
        eqs = [by.get(k) for k in ("equalizer_bass", "equalizer_mid", "equalizer_treble")]
        await asyncio.sleep(0.3)
        check(all(isinstance(e, NumberInfo) and e.min_value == -6 and e.max_value == 6 and e.step == 1 and e.unit_of_measurement == "dB" for e in eqs)
              and any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 0 for x in states), "equalizer entities listed, flat on PC")
        cli.number_command(eqs[0].key, 4); cli.number_command(eqs[2].key, -9)
        await asyncio.sleep(0.5)
        check(any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 4 for x in states)
              and any(isinstance(x, NumberState) and x.key == eqs[2].key and x.state == -6 for x in states), "equalizer commands reflected, clamped to -6..+6")
        check(any(isinstance(x, SelectState) and x.key == by["noise_reduction"].key and x.state == "Off" for x in states), "noise reduction off by default")
        lux, lauto, lbright = by.get("illuminance"), by.get("led_auto_brightness"), by.get("led_brightness")
        check(isinstance(lux, SensorInfo) and lux.device_class == "illuminance" and lux.unit_of_measurement == "lx" and int(lux.state_class) == 1
              and isinstance(lauto, SwitchInfo) and isinstance(lbright, NumberInfo) and (lbright.min_value, lbright.max_value) == (0, 100),
              "light sensor and LED brightness entities listed")
        last = lambda k, t: ([x.state for x in states if isinstance(x, t) and x.key == k] or [None])[-1]
        check(last(lux.key, SensorState) == 67 and last(lauto.key, SwitchState) is True and last(lbright.key, NumberState) == 80,
              f"illuminance from the sensor file, auto brightness on as in stock: {last(lux.key, SensorState)} lx, "
              f"auto {last(lauto.key, SwitchState)}, level {last(lbright.key, NumberState)}")
        cli.number_command(by["mic_level"].key, -20)
        await asyncio.sleep(0.5)
        check(any(isinstance(x, NumberState) and x.key == by["mic_level"].key and x.state == -20 for x in states),
              "setting commands reflected in state")
        want = subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip()
        got = [x.state for x in states if isinstance(x, TextSensorState) and x.key == tok.key]
        check(got == [want] and want.startswith("SP:0") and len(want) > 100, f"token state equals `hassmic -T`: {want[:16]}…")
        ajoin, apeers = by.get("join_arbitration_network"), by.get("arbitration_peers")
        check("arbitration_id" not in by and isinstance(ajoin, SwitchInfo) and int(ajoin.entity_category) == 1
              and any(isinstance(x, SwitchState) and x.key == ajoin.key and x.state for x in states) and isinstance(apeers, SensorInfo),
              "\"Join arbitration network\" switch on by default, peers sensor, no ID entity")
        svcs = (await cli.list_entities_services())[1]
        check([(v.name, [(x.name, int(x.type)) for x in v.args]) for v in svcs] == [("arbitration_key", [("network", 3), ("key", 3)])],
              "action \"arbitration_key\" (network, key: strings) for other Echos to hand over their network")
        check(open(settings).read().split()[:2] == ["0", "-20"] and open(settings).read().split()[8:9] == ["2"], f"settings persisted: {open(settings).read().strip()!r}")
        # field 16 is another branch's (alarms), kept as a placeholder; 17: Wi-Fi setup over Bluetooth, on by default
        check(open(settings).read().split()[15:] == ["-", "1"], f"settings fields 16 and 17: {open(settings).read().strip()!r}")
        cfg = await cli.get_voice_assistant_configuration(5)
        avail = sorted((w.id, w.wake_word, list(w.trained_languages)) for w in cfg.available_wake_words)
        check(avail == [("alexa", "Alexa", ["en"]), ("computer-en-US", "Computer", ["en"]), ("echo-de", "Echo", ["de"])]
              and list(cfg.active_wake_words) == ["alexa"] and cfg.max_active_wake_words == 1, f"wake words: all installed ones offered, Alexa active: {avail}")

        started = asyncio.Event(); mic = bytearray(); stopped = []
        async def handle_start(conv_id, flags, settings, phrase):
            handle_start.args = (flags, phrase); handle_start.audio = (settings.noise_suppression_level, settings.auto_gain, round(settings.volume_multiplier, 2)); started.set(); return 0           # 0 = audio over the API connection
        async def handle_stop(abort): stopped.append(abort)
        async def handle_audio(data, *_): mic.extend(data)
        finished = []
        async def handle_finished(msg): finished.append(msg.success)
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                      handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3)

        proc.send_signal(signal.SIGUSR1)                                           # "wake word"
        await asyncio.wait_for(started.wait(), 5)
        check(handle_start.args == (1, "Alexa"), f"pipeline request: flags, phrase = {handle_start.args}")
        check(handle_start.audio == (0, 0, 1.0), f"neutral audio settings in the request (the gain is applied on the Echo): {handle_start.audio}")
        await asyncio.sleep(1.0)
        peak = max(abs(v) for v in struct.unpack(f"<{len(mic) // 2}h", mic[:len(mic) // 2 * 2])) if mic else 0
        check(len(mic) > 16000, f"mic audio streamed: {len(mic)} bytes in 1 s")
        check(8000 < peak <= 29100, f"mic audio brought up to speech level, peaks limited below full scale: peak {peak}")

        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
        await asyncio.sleep(0.3); n = len(mic); await asyncio.sleep(0.4)
        check(len(mic) == n, "mic stream stops after STT_VAD_END")
        wh = by.get("last_request_whispered")
        whs = [x for x in states if isinstance(x, BinarySensorState) and wh and x.key == wh.key]
        check(isinstance(wh, BinarySensorInfo) and whs and whs[0].missing_state and whs[-1].state is True and not whs[-1].missing_state,
              f"\"Last request whispered\": unknown until a request, on at its VAD end, before the transcript: {[(x.state, x.missing_state) for x in whs]}")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Done"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) == 48000 and finished == [True], f"reply fetched from the TTS_END url and reported: {os.path.getsize(play)} bytes, finished={finished}")

        # another wake word, picked in Home Assistant: kept, and named in the pipeline request (HA's duplicate check keys on it)
        await cli.set_voice_assistant_configuration(["echo-de"]); await asyncio.sleep(0.5)
        cfg = await cli.get_voice_assistant_configuration(5)
        saved = open(os.path.join(state, "wake_word")).read().strip()
        check(list(cfg.active_wake_words) == ["echo-de"] and saved == "echo-de", f"wake word switched from Home Assistant and kept: {list(cfg.active_wake_words)}, {saved}")
        started.clear(); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5)
        check(handle_start.args == (1, "Echo"), f"pipeline request names the new wake word: {handle_start.args}")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.5)
        await cli.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.3)

        # A second client beside "Home Assistant": gets answers, sees and causes state changes, cannot take the voice assistant.
        cli2 = APIClient("127.0.0.1", PORT, None)
        await asyncio.wait_for(cli2.connect(login=True), 5)
        info2 = await asyncio.wait_for(cli2.device_info(), 5)
        ent2, _ = await cli2.list_entities_services()
        check(info2.name == "echo-dot" and len(ent2) == len(entities), "second client is served while the first stays connected")
        states2 = []; cli2.subscribe_states(states2.append)
        started2 = asyncio.Event()
        async def start2(*a): started2.set(); return 0
        async def stop2(*a): pass
        cli2.subscribe_voice_assistant(handle_start=start2, handle_stop=stop2)
        await asyncio.sleep(0.3); n1 = len(states)
        cli2.number_command(by["mic_level"].key, -30)
        await asyncio.sleep(0.5)
        check(any(isinstance(x, NumberState) and x.state == -30 for x in states[n1:]) and any(isinstance(x, NumberState) and x.state == -30 for x in states2),
              "a change made by one client reaches both")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(not started2.is_set(), "the voice assistant stays with the first subscriber")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await cli2.disconnect(); await asyncio.sleep(0.5)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "first client unaffected when the second one leaves")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # A client that stops reading (its Wi-Fi stalls) while it is sent a lot: here 2000 entity lists asked for at once,
        # more than the kernel buffers.  What it is sent waits in its own queue, so the wake word and the other clients go
        # on; past the queue's limit it is let go (rather than lose a state).  Every client used to be written to under
        # the core's lock: one stalled write held up everything for its send timeout, 5 s.
        def frame(t, body=b""):
            def varint(v):
                out = b""
                while True:
                    c, v = v & 0x7f, v >> 7
                    out += bytes([c | (0x80 if v else 0)])
                    if not v: return out
            return b"\0" + varint(len(body)) + varint(t) + body
        stuck = socket.socket(); stuck.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stuck.connect(("127.0.0.1", PORT))
        stuck.sendall(frame(1, b"\x0a\x05stuck") + frame(20) + b"".join(frame(11) for _ in range(2000)))   # hello, states, lists
        await asyncio.sleep(0.5)
        loop = asyncio.get_running_loop(); mid = by["equalizer_mid"].key; n1 = len(states); t0 = loop.time()
        cli.number_command(mid, 3)
        while not any(isinstance(x, NumberState) and x.key == mid and x.state == 3 for x in states[n1:]) and loop.time() - t0 < 10:
            await asyncio.sleep(0.02)
        dt_state = loop.time() - t0
        started.clear(); t0 = loop.time(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 10)
        dt_wake = loop.time() - t0
        check(dt_state < 1.0 and dt_wake < 1.0,
              f"a client that stops reading holds up no one: another client's state after {dt_state:.2f} s, the wake word's pipeline after {dt_wake:.2f} s")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        stuck.settimeout(10)
        try:
            while stuck.recv(1 << 16): pass
            gone = True
        except socket.timeout: gone = False
        except OSError: gone = True
        stuck.close()
        check(gone, "the client that does not catch up is let go")
        await asyncio.sleep(0.5)

        # A reply that asks a follow-up question (continue_conversation) must still be interruptible by the wake word:
        # the reply is cut and the new pipeline starts at once, not after the full second of audio.
        before = os.path.getsize(play); finished.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(0.6); started.clear()
        t0 = asyncio.get_running_loop().time(); proc.send_signal(signal.SIGUSR1)       # "Alexa" while it talks
        await asyncio.wait_for(started.wait(), 5); dt = asyncio.get_running_loop().time() - t0
        played = (os.path.getsize(play) - before) / 96000
        check(dt < 1.0 and played < 2.0, f"wake word interrupts a continue-conversation reply: new pipeline after {dt:.2f} s, {played:.2f} s of 3 s played")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # "Stop" (SIGHUP here), the second keyword of the wake word model: ends what makes noise, never starts a pipeline.
        started.clear(); proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.6)
        check(not started.is_set(), '"stop" out of silence starts nothing')

        async def reply_3s():               # a 3 s reply that would listen again afterwards
            proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
            await asyncio.sleep(0.6); started.clear()

        before = os.path.getsize(play); started.clear(); await reply_3s()
        proc.send_signal(signal.SIGHUP); await asyncio.sleep(1.5)
        played = (os.path.getsize(play) - before) / 96000
        check(not started.is_set() and played < 2.0, f'"stop" cuts a reply and nothing listens afterwards: {played:.2f} s of 3 s played')

        before = os.path.getsize(play); started.clear(); await reply_3s()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)    # "<wake word>, stop": wake word cuts and listens,
        stopped.clear(); started.clear(); proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.8)  # "stop" drops that pipeline
        played = (os.path.getsize(play) - before) / 96000
        check(stopped and not started.is_set() and played < 2.0, f'"<wake word>, stop" during a reply: pipeline dropped, {played:.2f} s of 3 s played (stops {stopped}, restarted {started.is_set()})')
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "dropped"})
        await asyncio.sleep(0.4)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "wake word works again after a dropped pipeline")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # streaming TTS: URL arrives with RUN_START, playback may begin at INTENT_PROGRESS, long before TTS_END
        before = os.path.getsize(play); finished.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "tell me a story"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
        await asyncio.sleep(0.4)
        early = os.path.getsize(play) - before
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(1.5)
        check(early > 0 and os.path.getsize(play) - before == 48000 and finished == [True],
              f"streaming reply starts at INTENT_PROGRESS and plays once: early={early}, total={os.path.getsize(play) - before}, finished={finished}")

        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "hello",
                                                                         f"http://127.0.0.1:{HTTP_PORT}/chime.wav")
        check(res.success, "announcement finished with success")
        check(os.path.getsize(play) - before == 2 * 48000, f"chime + announcement played: {os.path.getsize(play) - before} bytes")
        check(any(isinstance(s, MediaPlayerEntityState) and int(s.state) == 2 for s in states), "media player reported PLAYING")

        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/tts.mp3", 15, "mp3")
        got = os.path.getsize(play) - before
        check(res.success and 44000 <= got <= 52000, f"MP3 announcement decoded and played: {got} bytes (1 s at 24 kHz = 48000)")

        res = await cli.send_voice_assistant_announcement_await_response("http://127.0.0.1:1/none.wav", 15, "x")
        check(not res.success, "unreachable announcement URL reports failure")

        dnd = by.get("do_not_disturb")
        check(isinstance(dnd, SwitchInfo), "do not disturb switch listed")
        cli.switch_command(dnd.key, True); await asyncio.sleep(0.3)
        check(any(isinstance(s, SwitchState) and s.key == dnd.key and s.state for s in states)
              and open(settings).read().split()[6:7] == ["1"], f"do not disturb on and persisted: {open(settings).read().strip()!r}")
        check(open(settings).read().split()[7:8] == ["en"], f"Bluetooth announcement language persisted as its code: {open(settings).read().strip()!r}")
        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(not res.success and os.path.getsize(play) == before, "do not disturb drops announcements")
        cli.switch_command(dnd.key, False); await asyncio.sleep(0.3)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(res.success and os.path.getsize(play) - before == 48000, "announcements play again once it is off")

        cli.media_player_command(mp[0].key, volume=0.3)
        await asyncio.sleep(0.5)
        check(any(isinstance(s, MediaPlayerEntityState) and abs(s.volume - 0.3) < 0.01 for s in states), "volume command reflected in state")

        cli.switch_command(by["mute"].key, True); await asyncio.sleep(0.3)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.sleep(0.6)
        check(not started.is_set(), "mute switch blocks triggers")
        cli.switch_command(by["mute"].key, False); await asyncio.sleep(0.3)

        # Debug access: no key set yet, so this connection could be anyone's.  It must not open adb.
        adb = by.get("debug_access_adb"); adb_req = os.path.join(state, "adb-request")
        def adb_state(sts): return [x.state for x in sts if isinstance(x, SwitchState) and x.key == adb.key][-1:]
        check(isinstance(adb, SwitchInfo) and int(adb.entity_category) == 1 and adb_state(states) == [False], "debug access (adb over Wi-Fi) switch listed, off")
        cli.switch_command(adb.key, True); await asyncio.sleep(0.5)
        check(not os.path.exists(adb_req) and adb_state(states) == [False], "debug access refused without the key: nothing asked, still off")

        cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
        await asyncio.sleep(0.5)
        proc.send_signal(signal.SIGUSR1)                                           # button press silences the alarm, no pipeline
        started.clear(); await asyncio.sleep(0.5)
        check(not started.is_set(), "trigger during alarm only stops the alarm")

        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "nothing heard"})
        await asyncio.sleep(0.5)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "new pipeline after an error")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)

        # Encryption, in the order Home Assistant runs it: plaintext connection, DeviceInfo says "supported, provisionable",
        # key sent over a zero-PSK Noise connection, then only that key gets in.
        info = await cli.device_info()
        check(info.api_encryption_supported and info.api_encryption_provisionable, "device info: encryption supported and provisionable")
        check("api_encryption_supported=Noise_NNpsk0" in open(mdns).read() and "api_encryption=" not in open(mdns).read(), "mDNS without a key: api_encryption_supported only")
        key = base64.b64encode(os.urandom(32))
        check(await asyncio.wait_for(cli.noise_encryption_set_key(key), 5) is False, "key refused over plaintext")
        try:
            bad = APIClient("127.0.0.1", PORT, None, noise_psk=base64.b64encode(os.urandom(32)).decode()); await asyncio.wait_for(bad.connect(), 5); ok = False
        except InvalidEncryptionKeyAPIError as e: ok = e.received_name == "echo-dot"
        check(ok, "a random key before provisioning: invalid key, server hello names the device")
        prov = APIClient("127.0.0.1", PORT, None, noise_psk=ZERO_NOISE_PSK)
        await asyncio.wait_for(prov.connect(), 5)
        check(await asyncio.wait_for(prov.noise_encryption_set_key(key), 5) is True, "key accepted over the zero-PSK connection")
        await prov.disconnect()
        check(open(os.path.join(state, "api_key")).read().strip() == key.decode() and oct(os.stat(os.path.join(state, "api_key")).st_mode & 0o777) == "0o600",
              "key stored in state/api_key, mode 600")
        check("api_encryption=Noise_NNpsk0" in open(mdns).read(), "mDNS service file rewritten: api_encryption")
        try: await asyncio.wait_for(cli.device_info(), 3); ok = False
        except Exception: ok = True
        check(ok, "the plaintext connection from before is closed on its next request")
        for psk, err, what in ((None, RequiresEncryptionAPIError, "plaintext"), (ZERO_NOISE_PSK, InvalidEncryptionKeyAPIError, "zero PSK")):
            try: c = APIClient("127.0.0.1", PORT, None, noise_psk=psk); await asyncio.wait_for(c.connect(), 5); ok = False
            except err: ok = True
            check(ok, f"with a key: {what} connection refused ({err.__name__})")

        enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode())
        await asyncio.wait_for(enc.connect(login=True), 5)
        info = await enc.device_info()
        check(info.name == "echo-dot" and info.api_encryption_supported and not info.api_encryption_provisionable, "encrypted: device info, no longer provisionable")
        ent3, _ = await enc.list_entities_services()
        check(len(ent3) == len(entities), "encrypted: entities listed")
        started.clear(); mic.clear()
        enc.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio, handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5); await asyncio.sleep(1.0)
        check(len(mic) > 16000, f"encrypted: mic audio streamed: {len(mic)} bytes in 1 s")
        enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)
        enc.select_command(by["noise_reduction"].key, "Medium"); await asyncio.sleep(0.3)
        check(open(settings).read().split()[:1] == ["2"], f"noise reduction set to medium and persisted: {open(settings).read().strip()!r}")
        started.clear(); mic.clear(); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5); await asyncio.sleep(1.0)
        peak = max(abs(v) for v in struct.unpack(f"<{len(mic) // 2}h", mic[:len(mic) // 2 * 2])) if mic else 0
        check(len(mic) > 16000 and len(mic) % 320 == 0 and 8000 < peak <= 29100,
              f"with noise reduction: mic audio streamed in whole 10 ms frames at speech level: {len(mic)} bytes in 1 s, peak {peak}")
        enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)
        enc.select_command(by["noise_reduction"].key, "Off"); await asyncio.sleep(0.3)
        before = os.path.getsize(play)
        res = await enc.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(res.success and os.path.getsize(play) - before == 48000, "encrypted: announcement played")

        # Debug access over the keyed connection.  This test is the root side (lockdown.sh): it takes the request and
        # answers with adb-open for as long as the port is open.
        st2 = []; enc.subscribe_states(st2.append); await asyncio.sleep(0.3)
        enc.switch_command(adb.key, True); await asyncio.sleep(0.5)
        check(os.path.exists(adb_req) and open(adb_req).read() == "1\n" and adb_state(st2) == [True], "debug access with the key: request written, switch on")
        open(env["HASSMIC_ADB_OPEN"], "w").close(); os.unlink(adb_req); await asyncio.sleep(3)
        check(adb_state(st2) == [True], "stays on once the firewall service has opened it")
        os.unlink(env["HASSMIC_ADB_OPEN"]); await asyncio.sleep(3)
        check(adb_state(st2) == [False], "switch goes off when the window has run out")
        enc.switch_command(adb.key, True); await asyncio.sleep(19)
        check(adb_state(st2) == [False] and not os.path.exists(adb_req), "no answer within 15 s: request withdrawn, switch off again")
        check(await asyncio.wait_for(enc.noise_encryption_set_key(b""), 5) is True and not os.path.exists(os.path.join(state, "api_key")),
              "empty key from the keyed connection clears it (Home Assistant deleting the device)")
        check("api_encryption_supported=" in open(mdns).read(), "mDNS back to api_encryption_supported")
        await enc.disconnect()
        c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
        check((await c.device_info()).api_encryption_provisionable, "plaintext accepted again, provisionable again")
        st3 = []; c.subscribe_states(st3.append); await asyncio.sleep(0.3)    # here: the sleeps would move the mic checks' place in the capture loop
        last3 = lambda k, t: ([x.state for x in st3 if isinstance(x, t) and x.key == k] or [None])[-1]
        with open(env["HASSMIC_LUX"], "w") as f: f.write("68\n")                # flicker: not sent
        await asyncio.sleep(1.5)
        with open(env["HASSMIC_LUX"], "w") as f: f.write("150\n")
        await asyncio.sleep(1.5)
        sent = [x.state for x in st3 if isinstance(x, SensorState) and x.key == lux.key]
        check(sent == [67, 150], f"illuminance sent on a real change, not on flicker: {sent}")
        c.number_command(lbright.key, 30)
        await asyncio.sleep(0.5)
        check(last3(lbright.key, NumberState) == 30 and last3(lauto.key, SwitchState) is False, "a fixed LED level switches auto brightness off")
        check(open(settings).read().split()[9:11] == ["0", "30"], f"LED brightness kept in the settings file: {open(settings).read().strip()!r}")
        c.switch_command(lauto.key, True)
        await asyncio.sleep(0.5)
        check(last3(lauto.key, SwitchState) is True and open(settings).read().split()[9] == "1", "auto brightness switched on again")
        # sound detection: off by default, one event entity; on, it reports what the detector hears (here sound_none.c's
        # dog, once per ~10 s window of the capture), kept in the settings file
        sw, ev = by.get("sound_detection"), by.get("sound")
        check(isinstance(sw, SwitchInfo) and int(sw.entity_category) == 1 and isinstance(ev, EventInfo)
              and list(ev.event_types) == ["smoke_or_co_alarm", "glass_break", "dog_bark", "baby_cry", "snoring", "cough", "water", "beeping_appliance"]
              and last3(sw.key, SwitchState) is False, f"sound detection switch (off) and event entity listed: {list(ev.event_types) if ev else None}")
        await asyncio.sleep(11)
        check(not [x for x in st3 if isinstance(x, Event)], "no sound event while sound detection is off")
        c.switch_command(sw.key, True)
        await asyncio.sleep(0.5)
        check(last3(sw.key, SwitchState) is True and open(settings).read().split()[11:12] == ["1"],
              f"sound detection switched on and kept in the settings file: {open(settings).read().strip()!r}")
        for _ in range(26):
            if [x for x in st3 if isinstance(x, Event)]: break
            await asyncio.sleep(0.5)
        evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
        check(evs[:1] == [(ev.key, "dog_bark")], f"the detector's dogBark arrives as event dog_bark within a window: {evs}")
        # a window in which the Echo played something itself is dropped: here an announcement
        n0 = len(evs)
        async def no_pipeline(*a): return 0
        async def nothing(*a): pass
        unsub = c.subscribe_voice_assistant(handle_start=no_pipeline, handle_stop=nothing, handle_audio=nothing,
                                    handle_announcement_finished=nothing)      # announcements answer the assistant's client
        await asyncio.sleep(0.3)
        res = await c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        await asyncio.sleep(9)
        evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
        check(res.success and len(evs) == n0, f"no sound event for the window with the announcement in it: {evs[n0:]}")
        for _ in range(50):
            if len([x for x in st3 if isinstance(x, Event)]) > n0: break
            await asyncio.sleep(0.5)
        check(len([x for x in st3 if isinstance(x, Event)]) > n0, "sound events again once the Echo has been quiet for a window")
        c.switch_command(sw.key, False)
        await asyncio.sleep(0.5)
        check(last3(sw.key, SwitchState) is False and open(settings).read().split()[11:12] == ["0"], "sound detection switched off again")
        # Wi-Fi motion (experimental): off by default, the sensor unknown while off; on, a still level is no motion, a
        # wobbling one is, and it clears after the hold (3 s in the PC build, 30 s on the Echo)
        wsw, wbs, wsn = by.get("wifi_motion_detection"), by.get("wifi_motion"), by.get("wifi_motion_sensitivity")
        wstate = lambda: ([x for x in st3 if isinstance(x, BinarySensorState) and x.key == wbs.key] or [None])[-1]
        check(isinstance(wsw, SwitchInfo) and "experimental" in wsw.name and int(wsw.entity_category) == 1
              and isinstance(wbs, BinarySensorInfo) and wbs.device_class == "motion" and "experimental" in wbs.name
              and isinstance(wsn, NumberInfo) and (wsn.min_value, wsn.max_value) == (1, 10) and int(wsn.entity_category) == 1,
              "Wi-Fi motion entities listed, named experimental")
        check(last3(wsw.key, SwitchState) is False and last3(wsn.key, NumberState) == 5 and wstate() and wstate().missing_state,
              "Wi-Fi motion off by default, sensitivity 5, the sensor unknown")
        c.switch_command(wsw.key, True)
        await asyncio.sleep(3)
        check(last3(wsw.key, SwitchState) is True and wstate() and not wstate().missing_state and wstate().state is False
              and open(settings).read().split()[12:14] == ["1", "5"],
              f"switched on: a steady level is no motion, kept in the settings file: {open(settings).read().strip()!r}")
        for _ in range(60):                                 # someone walking through the path: 6 s
            rx_stat(random.randint(106, 118)); await asyncio.sleep(0.1)
            if wstate().state: break
        check(wstate().state is True, "a wobbling level is motion")
        rx_stat(112)
        await asyncio.sleep(7)
        check(wstate().state is False, "motion clears after the hold")
        c.number_command(wsn.key, 8)
        await asyncio.sleep(0.5)
        check(last3(wsn.key, NumberState) == 8 and open(settings).read().split()[13] == "8", "sensitivity set and kept")
        c.switch_command(wsw.key, False)
        await asyncio.sleep(1)
        check(last3(wsw.key, SwitchState) is False and wstate().missing_state and open(settings).read().split()[12] == "0",
              "switched off again: the sensor unknown")
        unsub()                                             # the real assistant again, with mic and replies
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                    handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3)
        # (at the end: these sleeps would move the mic checks' place in the capture loop)
        # The action button (SIGUSR2) while Home Assistant thinks: the run is aborted (as a Voice PE's button does), and what
        # Home Assistant still sends for it plays nothing.  The wake word instead aborts and listens again.
        async def thinking():
            proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
            await asyncio.sleep(0.3); stopped.clear(); started.clear()
        def late_reply():                    # the aborted run's reply, already on the way
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        before = os.path.getsize(play); started.clear(); await thinking()
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        late_reply(); await asyncio.sleep(1.0)
        check(stopped == [True] and not started.is_set() and os.path.getsize(play) == before,
              f"button while thinking: run aborted, nothing listens, its late reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "wake word works again after a cancelled run")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        before = os.path.getsize(play); started.clear(); await thinking()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        late_reply(); await asyncio.sleep(1.0)
        check(stopped == [True] and os.path.getsize(play) == before,
              f"wake word while thinking: run aborted, new one started, the old reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) - before == 48000, f"the new run's reply plays: {os.path.getsize(play) - before} bytes")

        # Streamed TTS: the reply's fetch already waits on Home Assistant (first words out, tool calls still running) when
        # the wake word cancels.  The fetch is cut: the new run's reply is not refused as busy, the old one never plays.
        before = os.path.getsize(play); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/late.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
        await asyncio.sleep(0.5); stopped.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(stopped == [True] and os.path.getsize(play) - before == 48000,
              f"wake word while a streamed reply is fetched: fetch cut, the new run's reply plays (stops {stopped}, {os.path.getsize(play) - before} bytes)")
        await asyncio.sleep(6)
        check(os.path.getsize(play) - before == 48000, f"the cancelled run's reply never plays: {os.path.getsize(play) - before} bytes")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "and the wake word works after it")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # Home Assistant's media stop while the audio is still being fetched: that one is dropped, and only that one (the
        # stop used to outlive it, and every reply after it stayed silent until a restart)
        before = os.path.getsize(play); finished.clear()
        slow = asyncio.ensure_future(c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/tts-slow.mp3", 15, "x"))
        await asyncio.sleep(0.5)
        c.media_player_command(mp[0].key, command=MediaPlayerCommand.STOP)
        res = await slow
        check(not res.success and os.path.getsize(play) == before, f"media stop during the fetch drops that announcement: success={res.success}")
        res = await c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(res.success and os.path.getsize(play) - before == 48000, f"the next announcement plays: {os.path.getsize(play) - before} bytes")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) - before == 2 * 48000, f"and so does the next reply: {os.path.getsize(play) - before - 48000} bytes")
        await c.disconnect()

        # connections that never say anything held all four slots and kept Home Assistant out until they closed
        idle = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(4)]
        await asyncio.sleep(0.3)
        c2 = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c2.connect(login=True), 5)
        check((await asyncio.wait_for(c2.device_info(), 5)).name == "echo-dot", "four silent connections do not keep a new client out")
        await asyncio.sleep(10.5)
        def closed(s):
            s.settimeout(0.5)
            try: return s.recv(1) == b""
            except socket.timeout: return False
            except OSError: return True     # reset
        gone = [closed(s) for s in idle]
        for s in idle: s.close()
        check(all(gone), f"silent connections let go after 10 s: {gone}")
        check((await asyncio.wait_for(c2.device_info(), 5)).name == "echo-dot", "the client that talks stays connected")
        await c2.disconnect()
        await alarm_clock(env, state, log, start_hassmic, lambda: proc)
    finally:
        (alarm_clock.proc or proc).terminate(); httpd.shutdown()
        for f in (play, settings):
            if os.path.exists(f): os.unlink(f)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

async def wait_for(cond, secs):
    for _ in range(int(secs * 20)):
        if cond(): return True
        await asyncio.sleep(0.05)
    return cond()


async def alarm_clock(env, state, log, start_hassmic, cur):
    """Alarms that ring on the Echo itself.  Home Assistant here is in Berlin (aioesphomeapi answers GetTimeRequest with
    the time and the zone's POSIX TZ string, as Home Assistant does), and sets the alarms through their entities; the
    alarm then rings without it too, and after a restart of hassmic, but not on a clock it cannot trust."""
    proc = alarm_clock.proc = cur()
    berlin = ZoneInfo("Europe/Berlin")
    async def nothing(*a): pass
    async def no_pipeline(*a): return 0
    async def connect():
        ha = APIClient("127.0.0.1", PORT, None, timezone="Europe/Berlin")
        await asyncio.wait_for(ha.connect(login=True), 5)
        ents = {e.object_id: e for e in (await ha.list_entities_services())[0]}
        st = []; ha.subscribe_states(st.append)
        ha.subscribe_voice_assistant(handle_start=no_pipeline, handle_stop=nothing, handle_audio=nothing, handle_announcement_finished=nothing)
        return ha, ents, st
    def ahead(s):                                       # Berlin's wall clock s seconds from now, and that moment in UTC
        t = datetime.datetime.now(berlin).replace(microsecond=0) + datetime.timedelta(seconds=s)
        return t, t.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
    def last_obj(st, k, t): return ([x for x in st if isinstance(x, t) and x.key == k] or [None])[-1]
    def last(st, k, t): x = last_obj(st, k, t); return x.state if x else None
    def logged(text, since): return any(text in l for l in log[since:])

    ha, by, st = await connect()
    n = len(log)
    check(await wait_for(lambda: os.path.exists(os.path.join(state, "clock")) and "CET-1CEST,M3.5.0,M10.5.0/3" in open(os.path.join(state, "clock")).read(), 3),
          "asks Home Assistant for the time when it subscribes; the time and Berlin's TZ string kept in state/clock")
    sw = [by.get(f"alarm_{i}") for i in (1, 2, 3)]; tm = [by.get(f"alarm_{i}_time") for i in (1, 2, 3)]; rp = [by.get(f"alarm_{i}_repeat") for i in (1, 2, 3)]
    stop, snooze, ringing, ev, nxt = by.get("stop_alarm"), by.get("snooze_alarm"), by.get("alarm_ringing"), by.get("alarm"), by.get("next_alarm")
    check(all(isinstance(x, SwitchInfo) for x in sw) and all(isinstance(x, TimeInfo) for x in tm) and all(isinstance(x, SelectInfo) for x in rp)
          and list(rp[0].options)[:4] == ["Once", "Every day", "Weekdays", "Weekends"] and len(rp[0].options) == 11
          and isinstance(stop, ButtonInfo) and isinstance(snooze, ButtonInfo) and isinstance(ringing, BinarySensorInfo)
          and isinstance(ev, EventInfo) and list(ev.event_types) == ["alarm_1", "alarm_2", "alarm_3"]
          and isinstance(nxt, TextSensorInfo) and nxt.device_class == "timestamp", "alarm clock entities listed")
    await asyncio.sleep(0.3)
    t1 = last_obj(st, tm[0].key, TimeState)
    check(last(st, sw[0].key, SwitchState) is False and t1 and (t1.hour, t1.minute) == (7, 0) and last(st, rp[0].key, SelectState) == "Once"
          and last(st, nxt.key, TextSensorState) == "" and last(st, ringing.key, BinarySensorState) is False,
          "alarms off at 07:00 once by default; next alarm unknown, not ringing")

    # one alarm a few seconds ahead: it rings, says so, and the stop button ends it; once, so it switches itself off
    t, iso = ahead(4)
    ha.time_command(tm[0].key, t.hour, t.minute, t.second); ha.switch_command(sw[0].key, True)
    await asyncio.sleep(0.5)
    check(last(st, nxt.key, TextSensorState) == iso and last(st, sw[0].key, SwitchState) is True, f"alarm 1 set {t:%H:%M:%S} Berlin: next alarm {iso}")
    ok = await wait_for(lambda: last(st, ringing.key, BinarySensorState) is True, 7)
    evs = [x.event_type for x in st if isinstance(x, Event) and x.key == ev.key]
    check(ok and evs == ["alarm_1"] and logged("alarm: ringing", n), f"it rings on time, event {evs}")
    ha.button_command(stop.key)
    ok = await wait_for(lambda: last(st, ringing.key, BinarySensorState) is False, 3)
    check(ok and logged("alarm: off", n) and last(st, sw[0].key, SwitchState) is False and last(st, nxt.key, TextSensorState) == "",
          "the stop button ends it; once: switched off, no next alarm")
    check(open(os.path.join(state, "alarms")).read().split("\n")[0].split()[:4] == ["0", "0", f"{t:%H:%M:%S}", "0"],
          f"kept in state/alarms: {open(os.path.join(state, 'alarms')).read().splitlines()[0]!r}")

    # snooze: rings again in 9 minutes; the stop button then drops that too, and the daily alarm is tomorrow's
    t, iso = ahead(3)
    ha.time_command(tm[1].key, t.hour, t.minute, t.second); ha.select_command(rp[1].key, "Every day"); ha.switch_command(sw[1].key, True)
    ok = await wait_for(lambda: last(st, ringing.key, BinarySensorState) is True, 6)
    ha.button_command(snooze.key)
    await wait_for(lambda: last(st, ringing.key, BinarySensorState) is False, 3)
    want = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=540))
    got = last(st, nxt.key, TextSensorState)
    off = abs((datetime.datetime.fromisoformat(got) - want).total_seconds()) if got else 999
    check(ok and last(st, ringing.key, BinarySensorState) is False and off < 3 and last(st, sw[1].key, SwitchState) is True,
          f"daily alarm 2 rang; snoozed: quiet, next alarm in 9 min ({got})")
    ha.button_command(stop.key); await asyncio.sleep(0.5)
    tomorrow = (t + datetime.timedelta(days=1)).astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
    check(last(st, nxt.key, TextSensorState) == tomorrow, f"stop drops the snooze: next is tomorrow's {tomorrow}")
    ha.switch_command(sw[1].key, False); await asyncio.sleep(0.3)

    # Home Assistant gone: the alarm rings all the same, and "<wake word>, stop" (SIGHUP) ends it
    t, iso = ahead(5)
    ha.time_command(tm[2].key, t.hour, t.minute, t.second); ha.switch_command(sw[2].key, True); await asyncio.sleep(0.5)
    n = len(log); await ha.disconnect()
    ok = await wait_for(lambda: logged("alarm clock: alarm 3 rings", n) and logged("alarm: ringing", n), 8)
    check(ok and logged("client disconnected (0 left)", n), "without Home Assistant connected: alarm 3 rings")
    proc.send_signal(signal.SIGHUP)
    check(await wait_for(lambda: logged("alarm: off", n), 3), '"stop" ends it')

    # hassmic restarting (an update) keeps the clock: an alarm rings with no client ever connected
    def restart(boot=None, ring_in=5):
        nonlocal proc
        proc.terminate(); proc.wait()
        t = datetime.datetime.now(berlin).replace(microsecond=0) + datetime.timedelta(seconds=ring_in)
        with open(os.path.join(state, "alarms"), "w") as f:
            f.write(f"0 1 {t:%H:%M:%S} 0 {int(time.time())}\n1 0 07:00:00 0 -1\n2 0 07:00:00 0 -1\n")
        if boot:
            path = os.path.join(state, "clock"); lines = open(path).read().splitlines()
            open(path, "w").write("\n".join(f"boot {boot}" if l.startswith("boot ") else l for l in lines) + "\n")
        n = len(log); proc = alarm_clock.proc = start_hassmic()
        return n
    n = restart()
    ok = await wait_for(lambda: logged("alarm clock: alarm 1 rings", n), 9)
    check(ok and logged("kept from before the restart", n), "after a restart of hassmic (same boot): the clock is kept, the alarm rings without Home Assistant")
    proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.5)

    # after a reboot (another kernel boot id) there is no clock to trust: nothing rings until Home Assistant has said the
    # time; then an alarm missed by a little rings late
    n = restart(boot="00000000-0000-0000-0000-000000000000", ring_in=2)
    await asyncio.sleep(5)
    check(logged("no alarm rings until Home Assistant has told it", n) and not logged("alarm: ringing", n), "after a reboot: no clock, the alarm does not ring")
    ha, by, st = await connect()
    ok = await wait_for(lambda: logged("alarm clock: alarm 1 rings", n), 4)
    check(ok and last(st, ringing.key, BinarySensorState) is True, "Home Assistant tells the time: the alarm missed by a few seconds rings late")
    ha.button_command(stop.key)
    check(await wait_for(lambda: last(st, ringing.key, BinarySensorState) is False, 3), "and stops")
    await ha.disconnect()
alarm_clock.proc = None

asyncio.run(main())
