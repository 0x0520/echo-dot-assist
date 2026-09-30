/* Noise reduction of the pipeline's mic audio (micdenoise.c) on espeak speech brought down to the level and the noise
 * floor the Echo delivers: speech at -57 dBFS over white noise at -65 dBFS, as in the captures of 2026-09-30.
 * usage: micdenoise_test testdata/alexa_espeak.raw */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "micdenoise.h"

#define RATE 16000
#define MAXN (RATE * 10)
static int bad;
static void check(int ok, const char *what, double v) { printf("%s %s: %.1f\n", ok ? "ok  " : "FAIL", what, v); if (!ok) bad = 1; }

static double level(const int16_t *s, size_t a, size_t b)
{
    double sum = 0;
    for (size_t i = a; i < b; i++) sum += (double)s[i] * s[i];
    return 10 * log10(sum / (b - a) / (32768.0 * 32768.0) + 1e-12);
}

static int16_t clean[MAXN], in[MAXN], out[MAXN + MICDENOISE_FRAME];

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "testdata/alexa_espeak.raw", "rb");
    if (!f) { printf("FAIL cannot open the speech file\n"); return 1; }
    size_t n = fread(clean, 2, MAXN, f), made = 0;
    fclose(f);
    /* the file: silence to 1.5 s, four phrases to 6.0 s (-21 dBFS), silence.  Speech to -57 dBFS, noise at -65 dBFS */
    double g = pow(10, (-57 - level(clean, RATE * 3 / 2, RATE * 6)) / 20), na = 32768 * pow(10, -65 / 20.0) * sqrt(3);
    unsigned seed = 1;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        in[i] = (int16_t)lrint(clean[i] * g + na * (((seed >> 8) & 0xffff) / 32768.0 - 1.0));
        clean[i] = (int16_t)lrint(clean[i] * g);
    }
    micdenoise_reset(12);
    for (size_t i = 0; i < n; i += 250) made += micdenoise_run(in + i, n - i < 250 ? n - i : 250, out + made);   /* odd blocks */
    check(made == n / MICDENOISE_FRAME * MICDENOISE_FRAME, "whole frames out, none lost", (double)made);

    enum { D = 192 };                     /* delay: 12 ms */
    /* from cold RNNoise needs a second or two to settle on white noise (0.2 s on the Echo's room noise): hassmic lets
     * it hear the second before the command first */
    double noise_in = level(in, RATE, RATE * 3 / 2), noise_out = level(out, RATE + D, RATE * 3 / 2 + D);
    check(noise_in - noise_out > 5 && noise_in - noise_out < 12.5, "noise 1 to 1.5 s after a cold start, dB down", noise_in - noise_out);
    double tail = level(in, RATE * 7, RATE * 8) - level(out, RATE * 7 + D, RATE * 8 + D);
    check(tail > 10 && tail < 12.5, "noise after the speech: down by the cap of 12 dB, no more", tail);
    double sp_clean = level(clean, RATE * 3 / 2, RATE * 6), sp_out = level(out, RATE * 3 / 2 + D, RATE * 6 + D);
    check(fabs(sp_out - sp_clean) < 3, "speech keeps its level, dB off the clean speech", sp_out - sp_clean);
    double err = 0, ref = 0;              /* what is left besides the clean speech, over the phrases */
    for (size_t i = RATE * 3 / 2; i < (size_t)RATE * 6; i++) { double e = out[i + D] - clean[i]; err += e * e; ref += (double)clean[i] * clean[i]; }
    double in_err = 0;
    for (size_t i = RATE * 3 / 2; i < (size_t)RATE * 6; i++) { double e = in[i] - clean[i]; in_err += e * e; }
    check(10 * log10(ref / err) > 10 * log10(ref / in_err) + 2, "speech to everything else better than before, dB", 10 * log10(ref / err) - 10 * log10(ref / in_err));
    check(micdenoise_speech() < 0.5, "no speech seen in the noise at the end, probability x 100", micdenoise_speech() * 100);

    micdenoise_reset(6);                  /* the lowest strength */
    made = 0;
    for (size_t i = 0; i < n; i += 250) made += micdenoise_run(in + i, n - i < 250 ? n - i : 250, out + made);
    tail = level(in, RATE * 7, RATE * 8) - level(out, RATE * 7 + D, RATE * 8 + D);
    check(tail > 5 && tail < 6.5, "cap of 6 dB", tail);

    micdenoise_reset(12);                 /* warming up on the room (out == NULL) gives nothing back */
    check(micdenoise_run(in, RATE, NULL) == 0, "listening only returns no samples", 0);
    return bad;
}
