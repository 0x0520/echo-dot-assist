#!/usr/bin/env python3
"""Speech against noise in micRaw and micAsr recorded together by scripts/mic-compare.sh (16 kHz mono s16le).
usage: mic-compare.py <dir with micRaw.raw and micAsr.raw> [speech_start_s speech_end_s]
Without the two times the sentence is looked for in micRaw: the longest stretch of 100 ms blocks 5 dB over its median,
gaps up to 0.4 s bridged.  Prints the level per 100 ms of both streams and the signal to noise of the sentence's first
1.5 s and of the rest: a front end that takes the talker for interference shows as micAsr falling behind micRaw there."""
import sys
import numpy as np

RATE, F = 16000, 160
d = sys.argv[1]
asr = np.fromfile(f"{d}/micAsr.raw", np.int16).astype(float)
raw = np.fromfile(f"{d}/micRaw.raw", np.int16).astype(float)

if not len(asr) or not len(raw): sys.exit("a recording is empty")
if np.abs(raw).max() < 4 or np.abs(asr).max() < 4: sys.exit("digital silence: the microphones are off (mute button?)")

def db(x, n):
    f = x[:len(x) // n * n].reshape(-1, n)
    return 10 * np.log10((f ** 2).mean(1) / 32768 ** 2 + 1e-12)

# micRaw started earlier and with a backlog block: find where micAsr lies in it by the 10 ms level curves
la, lr = db(asr, F), db(raw, F)
off = max(range(max(1, len(lr) - len(la) + 1)), key=lambda o: np.corrcoef(lr[o:o + len(la)], la[:len(lr) - o])[0, 1])
raw = raw[off * F:off * F + len(asr)]
n = min(len(raw), len(asr)); raw, asr = raw[:n], asr[:n]
corr = np.corrcoef(db(raw, F), db(asr, F))[0, 1]
print(f"{n / RATE:.1f} s, micRaw lined up at {off * F / RATE:.2f} s (level curves correlate {corr:.2f})")
if corr < 0.4: print("   weak match: little in the recording that both streams show, the numbers below may not mean much")
ba, br = db(asr, RATE // 10), db(raw, RATE // 10)
print("micAsr dBFS per 100 ms:", " ".join(f"{v:.0f}" for v in ba))
print("micRaw dBFS per 100 ms:", " ".join(f"{v:.0f}" for v in br))

if len(sys.argv) > 3:
    t0, t1 = float(sys.argv[2]), float(sys.argv[3])
else:
    loud = br > np.median(br) + 5
    best, start, last = (0, 0), None, None
    for i, v in enumerate(list(loud) + [False] * 5):
        if v:
            if start is None: start = i
            last = i
        elif start is not None and i - last > 4:
            if last + 1 - start > best[1] - best[0]: best = (start, last + 1)
            start = None
    t0, t1 = best[0] / 10, best[1] / 10
if t1 - t0 < 0.5: sys.exit("no sentence found: give its start and end in seconds")
print(f"sentence {t0:.1f} .. {t1:.1f} s")

def power(x, a, b):
    s = x[int(a * RATE):int(b * RATE)]
    return (s ** 2).mean() / 32768 ** 2 if len(s) else np.nan

def line(name, x):
    parts = [p for p in (power(x, 0.3, t0 - 0.5), power(x, t1 + 1, n / RATE - 0.2)) if not np.isnan(p)]   # silence before and after
    noise = 10 * np.log10(np.mean(parts))
    snr = lambda a, b: 10 * np.log10(power(x, a, b)) - noise
    mid = min(t0 + 1.5, t1)
    rest = f"{snr(mid, t1):5.1f}" if t1 - mid > 0.3 else "    -"
    print(f"{name}: noise {noise:6.1f} dBFS | signal to noise, first 1.5 s {snr(t0, mid):5.1f} dB | rest {rest} dB | last 1.5 s {snr(max(t0, t1 - 1.5), t1):5.1f} dB")

line("micRaw", raw); line("micAsr", asr)
