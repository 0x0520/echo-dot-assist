/* What the wake word engine detects, acted on by the capture thread: the ring buffer of what the engine heard, the
 * wake word's score, and wake word arbitration with other Echos (arb.c). */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "arb.h"
#include "audio.h"
#include "core_int.h"
#include "wake.h"

static atomic_int afe_asked;                        /* the front end was asked for the wake word's energies (afe_score) */

void afe_done(void)                                 /* any thread: no command follows, or it is over */
{
    if (!atomic_exchange(&afe_asked, 0)) return;
    char *argv[] = { "/system/bin/lipc-set-prop", "-i", "com.doppler.lasp", "LASP_CMD_NOTIFY_ASR_STREAM_STOPPED", "1", NULL };
    run_argv(argv);
}

/* ---------------------------------------------------------------- wake word arbitration (arb.c)
 * With other Echos in the arbitration network, a detection is scored and only acted on once the others' claims are in:
 * WINDOW_MS later, in the capture thread.  Until then nothing shows (no sound, no ring), so the Echos that lose stay
 * quiet.  The audio of the window is not lost: the winner sends it from the ring buffer ahead of the live stream. */

#define RING_SAMPLES (CAP_RATE * 4)
static int16_t ring[RING_SAMPLES];                  /* what the wake word engine was fed, by its sample index */
static uint64_t ring_n;                             /* capture thread: samples fed so far */
/* Detections go to the capture thread, which owns the ring and acts on them: the engine's thread only hands them over
 * (it used to read the ring while the capture thread wrote it, and to wait for core_lock behind a stalled client) */
static pthread_mutex_t det_lock = PTHREAD_MUTEX_INITIALIZER;
static int det_wake, det_stop;                      /* under det_lock */
static uint64_t det_begin, det_end;                 /* under det_lock */
static long long arb_due;                           /* capture thread: a round runs, decide then */
static uint64_t arb_from;                           /* capture thread: first sample after the detection */

void wakedet_feed(const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) ring[(ring_n + i) % RING_SAMPLES] = s[i];
    ring_n += n;
}

uint64_t ring_samples(void) { return ring_n; }

/* The longest piece of samples [*from, to) that lies in one run in the ring, as far as the ring still has them; *from
 * moves past it.  NULL: nothing (left) */
const int16_t *ring_chunk(uint64_t *from, uint64_t to, size_t *n)
{
    if (ring_n > RING_SAMPLES && *from < ring_n - RING_SAMPLES) *from = ring_n - RING_SAMPLES;
    if (*from >= to) return NULL;
    uint64_t at = *from % RING_SAMPLES, k = to - *from;
    if (k > RING_SAMPLES - at) k = RING_SAMPLES - at;
    *from += k; *n = (size_t)k;
    return ring + at;
}

static double ring_power(uint64_t a, uint64_t b)    /* mean square over samples [a, b), as far as the ring still has them */
{
    double sum = 0; uint64_t n = 0;
    if (ring_n > RING_SAMPLES && a < ring_n - RING_SAMPLES) a = ring_n - RING_SAMPLES;
    if (b > ring_n) b = ring_n;
    for (uint64_t i = a; i < b; i++, n++) { double v = ring[i % RING_SAMPLES]; sum += v * v; }
    return n ? sum / n : 0;
}

/* Signal to noise of the wake word in dB x 100: the keyword against the half second before it (ending 100 ms ahead, so
 * that an early "begin" does not count the word as noise).  On the processed stream after beamforming, AEC and gain
 * control, the absolute level says less than how far the voice stands out of the room: the Echo the talker is close to
 * and facing hears it clearest.  HASSMIC_TEST_SCORE stands in for it on the PC, where SIGUSR1 plays the detection. */
/* Amazon's own measure of the wake word, which its cloud used to pick the Echo that answers ("ESP"): the energy of the
 * keyword and of the room before it as the front end measures them ("1-mic ESP" in its log).  As stock does it: hand the front end the keyword's place on its clock
 * (wake_afe_times), then read LASP_CMD_REQUEST_ARBITRATION_JSON: {"voiceEnergy":..,"ambientEnergy":..,..}.  Both
 * through lipc's tools: 150 ms (measured), inside the arbitration window of the others, who wait 200 ms and count
 * claims up to a second old.  Their ratio in dB x 100 is a signal to noise like our own score below, so Echos without
 * it (older builds, other front ends) still compare.  Reading it also puts the front end into its utterance state and
 * starts its diagnostics (FINDINGS.md "Listening mode"): afe_done() ends both.  0: not available. */
static int afe_score(int *score)                    /* capture thread */
{
    long ts, te; char cmd[400], buf[512]; const char *v, *a;
    if (!wake_afe_times(&ts, &te)) return 0;
    snprintf(cmd, sizeof cmd, "/system/bin/lipc-set-prop -s com.doppler.lasp LASP_CMD_SET_WAKEWORD_METADATA "
             "'{\"timestamp_before_ww_start\":%ld,\"timestamp_before_ww_end\":%ld}' && "
             "/system/bin/lipc-get-prop -s com.doppler.lasp LASP_CMD_REQUEST_ARBITRATION_JSON", ts, te);
    char *argv[] = { "/system/bin/sh", "-c", cmd, NULL };
    run_output(argv, buf, sizeof buf);
    if (!(v = strstr(buf, "\"voiceEnergy\":")) || !(a = strstr(buf, "\"ambientEnergy\":"))) return 0;
    atomic_store(&afe_asked, 1);
    double voice = atof(strchr(v, ':') + 1), ambient = atof(strchr(a, ':') + 1);
    *score = (int)lround(1000 * log10((voice + 1) / (ambient + 1)));
    fprintf(stderr, "wake: front end: voice energy %.0f, ambient %.0f\n", voice, ambient);
    return 1;
}

static int wake_score(uint64_t begin, uint64_t end, int simulated)
{
    const char *t = getenv("HASSMIC_TEST_SCORE");
    if (simulated && t) return atoi(t);
    uint64_t gap = CAP_RATE / 10, len = CAP_RATE / 2;
    uint64_t ne = begin > gap ? begin - gap : 0, nb = ne > len ? ne - len : 0;
    double w = ring_power(begin, end), n = ring_power(nb, ne), fs = 32768.0 * 32768.0;
    int own = (int)lround(1000 * log10((w + 1) / (n + 1))), afe;
    fprintf(stderr, "wake: level %.1f dBFS over noise %.1f dBFS\n", 10 * log10((w + 1) / fs), 10 * log10((n + 1) / fs));
    if (simulated || !afe_score(&afe)) return own;
    fprintf(stderr, "wake: score %d from the front end (%d from the mic stream)\n", afe, own);
    return afe;
}

static void answer(uint64_t from)                   /* capture thread: act on the wake word; audio after it from the ring */
{
    int was = mic_streaming();
    trigger(0);
    if (from && !was && mic_streaming()) mic_queue_ring(from);
    if (!mic_streaming()) afe_done();               /* it stopped an alarm, or cut a reply: the pipeline's end follows */
}

static void wake_heard(uint64_t begin, uint64_t end, int simulated)     /* capture thread */
{
    if (!arb_running()) { trigger(0); return; }
    if (arb_due) return;                            /* the same wake word once more while its round runs */
    pthread_mutex_lock(&core_lock);
    int alarm = alarm_ringing(), can = alarm || (satellite_ready() && !core_muted()), prio = alarm || core_state() != IDLE ? 2 : 0;
    pthread_mutex_unlock(&core_lock);
    if (!can) { trigger(0); return; }               /* could not answer: a claim would only silence the Echos that can */
    pthread_mutex_lock(&core_lock); char kw[64]; wake_word_active_name(kw, sizeof kw); pthread_mutex_unlock(&core_lock);
    long long due = arb_claim(kw, wake_score(begin, end, simulated), prio);
    if (!due) { trigger(0); if (!mic_streaming()) afe_done(); return; }
    arb_due = due; arb_from = ring_n;
}

void wakedet_simulate(void) { wake_heard(ring_n > CAP_RATE * 6 / 10 ? ring_n - CAP_RATE * 6 / 10 : 0, ring_n, 1); }

void on_wake(const char *keyword, uint64_t begin, uint64_t end)     /* detector thread */
{
    if (!core_local_wake) return;
    pthread_mutex_lock(&det_lock);
    if (!strcasecmp(keyword, "STOP")) det_stop = 1;
    else { det_wake = 1; det_begin = begin; det_end = end; }
    pthread_mutex_unlock(&det_lock);
}

static void wake_detected(uint64_t begin, uint64_t end)                    /* capture thread */
{
    float db = 10 * log10f((ring_power(begin, end) + 1) / (32768.0f * 32768.0f));     /* the talker's level, for the gain */
    pthread_mutex_lock(&core_lock); mic_keyword(db); pthread_mutex_unlock(&core_lock);
    wake_heard(begin, end, 0);
}

void wakedet_poll(void)
{
    pthread_mutex_lock(&det_lock);
    int w = det_wake, s = det_stop; uint64_t b = det_begin, e = det_end;
    det_wake = det_stop = 0;
    pthread_mutex_unlock(&det_lock);
    if (w) wake_detected(b, e);
    if (s) stop_word();
    if (arb_due && mono_ms() >= arb_due) { arb_due = 0; if (arb_decide()) answer(arb_from); else afe_done(); }    /* after this block went out live */
}

static int arb_send_key(const char *node, const char *network, const char *key)
{
    pthread_mutex_lock(&core_lock);
    const struct proto *p = core_client();
    int r = p && p->arb_send ? p->arb_send(node, network, key) : -1;
    pthread_mutex_unlock(&core_lock);
    return r;
}

static void arb_notify(void) { pthread_mutex_lock(&core_lock); if (core_proto()->arb_changed) core_proto()->arb_changed(); pthread_mutex_unlock(&core_lock); }

int wakedet_arb_start(int port)
{
    static const struct arb_hooks arb_hooks = { arb_send_key, arb_notify };
    return arb_start(port, core_node_name(), &arb_hooks);
}
