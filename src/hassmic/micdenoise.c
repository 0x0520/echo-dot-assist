/* RNNoise on the 16 kHz mic stream: see micdenoise.h.
 *
 * RNNoise runs at 48 kHz in frames of 480, so every 160 samples go up by 3 (zero stuffing + low-pass), through
 * rnnoise_process_frame, and down by 3 again (the same low-pass, every third sample).  Its bands above 8 kHz stay empty,
 * which it copes with: the gains are per band.
 *
 * Level: RNNoise was trained on speech between full scale and 40 dB below it and ours arrives at -55 dBFS, but raising
 * the stream ahead of it (by 12, 24, 36 dB, and back down behind it) changed nothing measurable, so it gets it as it is.
 *
 * Left alone RNNoise takes the floor between words to digital silence (-65 to below -108 dBFS on the captures), and it
 * cuts into the weak sounds of quiet speech.  So the reduction is capped: the output is RNNoise's plus the input of the
 * same moment (RNNoise delays by one frame) at minus the cap.  By ear (user, a quiet sentence over the Echo's room
 * noise, 2026-09-30): artefacts at 18 dB, fine at 6, 9 and 12, which became the three strengths. */
#include "micdenoise.h"
#include <math.h>
#include <string.h>
#include "../third_party/rnnoise/rnnoise.h"

#define UP 3
#define F48 (MICDENOISE_FRAME * UP)
#define TAPS 96                         /* low-pass at 48 kHz: pass to 7 kHz, -60 dB from 9 kHz (Blackman) */

static float lp[TAPS];
static float up_hist[TAPS / UP - 1];    /* the last input samples: the zero-stuffed signal only has every third */
static float down_hist[TAPS - 1];
static float dry[F48];                  /* the frame RNNoise was given before this one: what its output belongs to */
static int16_t pend[MICDENOISE_FRAME]; static size_t npend;
static DenoiseState *st;
static float speech, keep;              /* keep: how much of the input stays, 10^(-cap / 20) */

static void init(void)
{
    double sum = 0;
    for (int i = 0; i < TAPS; i++) {
        double t = i - (TAPS - 1) / 2.0, x = M_PI * t / UP;                     /* cutoff 8 kHz = fs / (2 * UP) */
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (TAPS - 1));
        lp[i] = (float)((fabs(t) < 1e-9 ? 1.0 : sin(x) / x) * w);
        sum += lp[i];
    }
    for (int i = 0; i < TAPS; i++) lp[i] = (float)(lp[i] / sum);
}

void micdenoise_reset(float max_db)
{
    keep = powf(10.0f, -max_db / 20.0f);
    if (!lp[TAPS / 2]) init();
    if (st) rnnoise_destroy(st);        /* rnnoise_init on a used state would leak its three buffers */
    st = rnnoise_create(NULL);
    memset(up_hist, 0, sizeof up_hist); memset(down_hist, 0, sizeof down_hist); memset(dry, 0, sizeof dry);
    npend = 0; speech = 0;
}

float micdenoise_speech(void) { return speech; }

static void frame(const int16_t *in, int16_t *out)
{
    enum { H = TAPS / UP - 1 };
    float x[H + MICDENOISE_FRAME], a[F48], b[TAPS - 1 + F48];
    memcpy(x, up_hist, sizeof up_hist);
    for (int i = 0; i < MICDENOISE_FRAME; i++) x[H + i] = in[i];
    memcpy(up_hist, x + MICDENOISE_FRAME, sizeof up_hist);
    /* up: output sample 3 i + p is the sum over the input samples that fall under the filter, taps p, p + 3, ... */
    for (int i = 0; i < MICDENOISE_FRAME; i++)
        for (int p = 0; p < UP; p++) {
            float s = 0;
            for (int k = 0; k <= H; k++) s += lp[p + UP * k] * x[H + i - k];
            a[UP * i + p] = s * UP;
        }
    memcpy(b, down_hist, sizeof down_hist);
    speech = rnnoise_process_frame(st, b + TAPS - 1, a);
    for (int i = 0; i < F48; i++) b[TAPS - 1 + i] = b[TAPS - 1 + i] * (1 - keep) + dry[i] * keep;
    memcpy(dry, a, sizeof dry);
    memcpy(down_hist, b + F48, sizeof down_hist);
    if (!out) return;
    for (int i = 0; i < MICDENOISE_FRAME; i++) {
        float s = 0;
        for (int k = 0; k < TAPS; k++) s += lp[k] * b[UP * i + TAPS - 1 - k];
        out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)lrintf(s);
    }
}

size_t micdenoise_run(const int16_t *in, size_t n, int16_t *out)
{
    size_t made = 0;
    if (!st) micdenoise_reset(12);
    while (n) {
        size_t k = MICDENOISE_FRAME - npend; if (k > n) k = n;
        memcpy(pend + npend, in, k * sizeof *in);
        npend += k; in += k; n -= k;
        if (npend < MICDENOISE_FRAME) break;
        frame(pend, out ? out + made : NULL);
        made += MICDENOISE_FRAME; npend = 0;
    }
    return out ? made : 0;
}
