/* Mic audio to the pipeline: queued by the capture thread, made ready (noise reduction, gain) and sent by the mic
 * sender thread; and the front end's listening mode while a command is spoken. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "core_int.h"
#include "micdenoise.h"
#include "micgain.h"
#include "threadname.h"

static atomic_int streaming;                        /* mic audio goes to the pipeline; set under core_lock, read anywhere */
static struct micgain mic_gain;                     /* under lock: gain settings as Home Assistant set them (micgain.h); the
                                                      * mic sender works on its own copy, taken when mic_gain_gen moves */
static unsigned mic_gain_gen;                       /* under lock */
static int mic_fresh;                               /* under lock: a pipeline started, the gain has not seen it yet */
static float keyword_db = 1;                        /* under lock: rms of the last wake word, dBFS */
static long long last_wake_ms;                      /* under lock.  Amazon's models know "stop" only in the ~2 s after the wake
                                                      * word (op.cfg.json: awake state) */
static int mic_denoise;                             /* under lock: noise reduction ahead of the gain (micdenoise.h):
                                                      * 0 off, 1 low, 2 medium, 3 high */
static atomic_uint mic_seq;                         /* bumped by pipeline_start (lock held): mic blocks of an earlier
                                                      * pipeline still queued for the sender go nowhere */
static long long denoise_ns; static unsigned denoise_frames;   /* under lock: its cost in the running pipeline */

void mic_init(void) { micgain_init(&mic_gain, MICGAIN_LEVEL); }    /* until the protocol has its saved settings (Wyoming: always) */

int  mic_streaming(void) { return atomic_load(&streaming); }
int  mic_stream_end(void) { return atomic_exchange(&streaming, 0); }
void mic_keyword(float db) { keyword_db = db; last_wake_ms = mono_ms(); }
long long mic_keyword_ms(void) { return last_wake_ms; }

void mic_pipeline_new(void)
{
    mic_fresh = 1;
    atomic_fetch_add(&mic_seq, 1);                  /* before streaming: a block the capture thread queues under it is ours */
}

void mic_stream_on(void)
{
    atomic_store(&streaming, 1);
    listening(1);
}

void core_mic_level(int dbfs)
{
    if (dbfs != mic_gain.level) { micgain_init(&mic_gain, dbfs); mic_gain_gen++; mic_fresh = 1; }
}

int core_mic_denoise(int set)
{
    if (set >= 0 && set != mic_denoise) { mic_denoise = set > 3 ? 3 : set; mic_fresh = 1; }
    return mic_denoise;
}

/* lock held.  Tells Amazon's front end that a command is being spoken, as stock does after the wake word.  While its
 * "utterance" flag is set (libasp.so, FINDINGS.md "Listening mode") the echo canceller (AEC_V2) and the interference
 * canceller (ARA_V2) stop adapting and the beam merger keeps its beam group; without it they adapt to the talker and take
 * the voice for interference after ~1.5 s: in micAsr a quiet sentence then sinks to 0-3 dB over the floor while micRaw
 * still has it at 8-10 dB (6 captures, 2026-09-30); with it micAsr stays within 1 dB of micRaw for a 4 s sentence.
 * Stock PuffinApp sets the flag by reading LASP_CMD_REQUEST_ARBITRATION_JSON and clears it with
 * LASP_CMD_NOTIFY_ASR_STREAM_STOPPED; those also start and stop the front end's diagnostics with their metrics, so this
 * uses the plain switch.  No timeout in the front end, and the mixer keeps the state: cleared at start in case hassmic
 * died while listening.  Not with the wake word at the server (-w remote): the mic streams all the time then, and the
 * cancellers would never adapt. */
void listening(int on)
{
    static int is = -1;
    if (!core_local_wake) on = 0;
    if (on == is) return;
    is = on;
    char *argv[] = { "/system/bin/lipc-set-prop", "-i", "com.doppler.lasp", "LASP_CMD_SET_LISTENING_MODE", on ? "1" : "0", NULL };
    run_argv(argv);
}

void mic_stopped(void)
{
    listening(0);
    afe_done();
    if (denoise_frames) fprintf(stderr, "denoise: %.1f s of audio took %.0f ms of CPU\n", denoise_frames / 100.0, denoise_ns / 1e6);
    denoise_frames = 0; denoise_ns = 0;
}

void core_mic_off(void) { whisper_mark_eou(); atomic_store(&streaming, 0); mic_stopped(); }

/* ---------------------------------------------------------------- mic audio to the pipeline
 * The capture thread only queues the blocks; the mic sender thread makes them ready and sends them.  It used to send
 * them itself, under core_lock: a client whose Wi-Fi stalls blocks that write for up to 5 s (SO_SNDTIMEO), and the
 * wake word, sound detection and the capture loop stopped for that long, while every other thread waited for the lock.
 * Now a stall only fills the queue, which drops its oldest blocks: a pipeline that far behind is lost anyway.
 * Noise reduction and gain run on the sender without core_lock (RNNoise costs about 1 ms per 10 ms frame); it takes the
 * lock to read the settings and for each send (proto->audio's contract). */
#define MQ_BLOCK 512                                /* samples: 32 ms */
#define MQ_SLOTS 64                                 /* 2 s: the 1 s ahead of a command (below) and a second of it */
struct mblock { unsigned seq; int ahead; unsigned n; int16_t pcm[MQ_BLOCK]; };
static struct mblock mq[MQ_SLOTS];
static unsigned mq_head, mq_count, mq_dropped;      /* under mq_lock */
static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mq_cond = PTHREAD_COND_INITIALIZER;
static unsigned mq_seq;                             /* capture thread: the pipeline it queued for last */

static void mq_add(const int16_t *s, size_t n, unsigned seq, int ahead)
{
    pthread_mutex_lock(&mq_lock);
    while (n) {
        size_t k = n < MQ_BLOCK ? n : MQ_BLOCK;
        if (mq_count == MQ_SLOTS) { mq_head = (mq_head + 1) % MQ_SLOTS; mq_count--; mq_dropped++; }
        struct mblock *b = &mq[(mq_head + mq_count++) % MQ_SLOTS];
        b->seq = seq; b->ahead = ahead; b->n = (unsigned)k; memcpy(b->pcm, s, k * 2);
        s += k; n -= k;
    }
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
}

static void mq_add_ring(uint64_t from, uint64_t to, unsigned seq, int ahead)
{
    const int16_t *p; size_t n;
    while ((p = ring_chunk(&from, to, &n))) mq_add(p, n, seq, ahead);
}

/* Capture thread: mic audio for the running pipeline.  at: the ring's index of the first sample (0: not from the ring).
 * The first block of a pipeline brings the second ahead of it along, for RNNoise to settle on the room (mic_sender) */
static unsigned mic_queue_seq(uint64_t at)
{
    unsigned seq = atomic_load(&mic_seq);
    if (seq != mq_seq) { mq_seq = seq; if (at) mq_add_ring(at > CAP_RATE ? at - CAP_RATE : 0, at, seq, 1); }
    return seq;
}
void mic_queue(const int16_t *pcm, size_t n, uint64_t at) { mq_add(pcm, n, mic_queue_seq(at), 0); }
void mic_queue_ring(uint64_t from) { mq_add_ring(from, ring_samples(), mic_queue_seq(from), 0); }

/* Mic audio to the pipeline: noise reduction if switched on, then brought to speech level.
 * - The wake word just before it sets the gain to start with (from up to 3 s back), and RNNoise first hears the second
 *   of room and wake word ahead of the command, so that it does not start on the first word.
 * - Our own wake sound is still in the stream after the echo canceller: +21 dB over the floor in micRaw, +4 to +8 dB in
 *   micAsr (4 triggers, 2026-09-30), as loud as a quiet talker.  The gain took it for speech and came down for the
 *   command behind it (-32 instead of -26 dBFS in Home Assistant's recording), so it holds still while a sound plays. */
static int16_t mic_back[CAP_RATE];                  /* mic sender: the last second it got, for RNNoise to start on */
static size_t mic_back_n, mic_back_at;

static void mic_back_add(const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { mic_back[mic_back_at] = s[i]; mic_back_at = (mic_back_at + 1) % CAP_RATE; }
    mic_back_n = mic_back_n + n > CAP_RATE ? CAP_RATE : mic_back_n + n;
}

static void mic_start(struct micgain *g, float kdb, int denoise)       /* mic sender: a pipeline (or new settings) */
{
    static const int denoise_db[] = { 0, 6, 9, 12 };
    micgain_start(g, kdb);
    fprintf(stderr, "mic gain: talker %.1f dBFS, gain %+.1f dB", micgain_talker_db(g), g->gain_db);
    if (denoise) fprintf(stderr, ", noise reduction %d dB", denoise_db[denoise]);
    fprintf(stderr, "\n");
    if (!denoise) return;
    micdenoise_reset(denoise_db[denoise]);
    size_t from = (mic_back_at + CAP_RATE - mic_back_n) % CAP_RATE, k = CAP_RATE - from < mic_back_n ? CAP_RATE - from : mic_back_n;
    micdenoise_run(mic_back + from, k, NULL);
    if (k < mic_back_n) micdenoise_run(mic_back, mic_back_n - k, NULL);
}

void *mic_sender(void *arg)
{
    thread_name("mic sender");
    static struct mblock b;
    static int16_t out[MQ_BLOCK + MICDENOISE_FRAME];
    static struct micgain g;
    unsigned seq = 0, gen = 0; int denoise = 0;
    (void)arg;
    pthread_mutex_lock(&core_lock); g = mic_gain; gen = mic_gain_gen; pthread_mutex_unlock(&core_lock);
    for (;;) {
        pthread_mutex_lock(&mq_lock);
        while (!mq_count) pthread_cond_wait(&mq_cond, &mq_lock);
        b = mq[mq_head]; mq_head = (mq_head + 1) % MQ_SLOTS; mq_count--;
        unsigned dropped = mq_dropped; mq_dropped = 0;
        pthread_mutex_unlock(&mq_lock);
        if (dropped) fprintf(stderr, "mic: %u ms dropped, the client does not take the audio\n", dropped * MQ_BLOCK * 1000 / CAP_RATE);
        if (b.seq != seq) { seq = b.seq; mic_back_n = 0; }
        if (b.ahead) { mic_back_add(b.pcm, b.n); continue; }

        int fresh = 0; float kdb = 1;
        pthread_mutex_lock(&core_lock);
        int live = b.seq == atomic_load(&mic_seq) && atomic_load(&streaming) && core_client();
        if (live && mic_fresh) {
            mic_fresh = 0; fresh = 1;
            if (gen != mic_gain_gen) { gen = mic_gain_gen; g = mic_gain; }     /* else the talker's level carries over */
            kdb = mono_ms() - last_wake_ms < 3000 ? keyword_db : 1;
        }
        if (live) denoise = mic_denoise;
        pthread_mutex_unlock(&core_lock);
        if (!live) continue;

        if (fresh) mic_start(&g, kdb, denoise);
        g.hold = own_sound_hold();
        size_t m = b.n; const int16_t *src = b.pcm; long long ns = 0;
        if (denoise) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
            m = micdenoise_run(b.pcm, b.n, out); src = out;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
            ns = (t1.tv_sec - t0.tv_sec) * 1000000000LL + t1.tv_nsec - t0.tv_nsec;
        }
        mic_back_add(b.pcm, b.n);
        if (m) micgain_run(&g, src, out, m);

        pthread_mutex_lock(&core_lock);
        const struct proto *p = core_client();
        if (m && b.seq == atomic_load(&mic_seq) && atomic_load(&streaming) && p) p->audio(out, m * 2);
        if (denoise) { denoise_ns += ns; denoise_frames += (unsigned)(m / MICDENOISE_FRAME); }
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}
