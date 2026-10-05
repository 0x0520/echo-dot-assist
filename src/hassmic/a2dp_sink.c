/*
 * A phone playing to the Echo (bt_int.h): the A2DP sink.
 *
 *   AVDTP     one sink endpoint per codec (a2dp_codecs.c: SBC, AAC, aptX HD, aptX; Opus not, see there), the source
 *             picks; delay reporting so video stays in sync.  One stream at a time: the others show as in use meanwhile.
 *             AAC only while the "Bluetooth AAC" switch is on (a2dp_aac(), off by default): see there.
 *   audio     decoded into a jitter buffer, a player thread feeds the mixer's music stream.  The phone's clock and the
 *             mixer's drift apart: a frame is dropped or repeated now and then to hold the buffer at its target.
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "a2dp.h"
#include "a2dp_codec.h"
#include "audio.h"
#include "bt_int.h"
#include "core.h"
#include "hci.h"
#include "threadname.h"

#define PREBUF_MS 150                   /* jitter buffer while playing: Wi-Fi shares the antenna and delays packets */
#define MIXER_US 60000                  /* kept queued in the mixer beyond that */
#define OUTPUT_MS 70                    /* mixer, DAC, amplifier (measured for Sendspin, see sendspin.c) */
#define CHUNK_MS 10

/* ---------------------------------------------------------------- audio: jitter buffer and player */

#define RING_MAX_RATE 48000                             /* the most any endpoint of ours negotiates */
#define RING_FRAMES RING_MAX_RATE                       /* 1 s of stereo at 48 kHz */
static int16_t ring[RING_FRAMES * 2];
static size_t r_head, r_count; static unsigned r_rate;  /* r_lock */
static long r_dropped;
static pthread_mutex_t r_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_cond = PTHREAD_COND_INITIALIZER;
static atomic_int streaming;                            /* AVDTP says the source plays */
static atomic_int gap_max, gaps_long, dry_count;       /* per log interval: longest pause between media packets (ms),
                                                           pauses over 100 ms, times the buffer ran dry */

static atomic_int yielded;                              /* another source took over and the device cannot be paused */
static atomic_llong button_paused_at;                   /* the action button paused the device (AVRCP) */

static void ring_push(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    /* The player's buffers are sized for 48 kHz.  The codecs hold to what was negotiated (AAC checks what it decodes,
     * a2dp_codecs.c), this is the last line: a higher rate from the radio never reaches the player. */
    if (rate < 8000 || rate > RING_MAX_RATE || !ch) return;
    pthread_mutex_lock(&r_lock);
    if (rate != r_rate) { r_rate = rate; r_count = 0; }
    for (unsigned i = 0; i < frames; i++) {
        if (r_count == RING_FRAMES) { r_dropped++; continue; }
        size_t at = (r_head + r_count) % RING_FRAMES * 2;
        ring[at] = pcm[i * ch]; ring[at + 1] = pcm[i * ch + ch - 1];      /* mono: both sides */
        r_count++;
    }
    pthread_cond_signal(&r_cond);
    pthread_mutex_unlock(&r_lock);
}

static void *player(void *arg)
{
    thread_name("a2dp player");
    enum { MAXCHUNK = 48000 * CHUNK_MS / 1000 };
    int16_t buf[MAXCHUNK * 2], in[(MAXCHUNK + 8) * 2], prev[2] = { 0, 0 };
    unsigned rate = 0; int open = 0, primed = 0, peak = 0; long long quiet = 0, stat = 0;
    double avg = 0, base = 1, acc = 0;                  /* buffer level (ms); drift estimate; fractional input frames */
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&r_lock);
        long wait_ns = open || atomic_load(&streaming) ? 20000000 : 500000000;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += wait_ns; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        /* start once the jitter buffer and the mixer's share are there: the mixer takes its share at once */
        unsigned rr = r_rate; size_t prime = (size_t)rr * (PREBUF_MS + MIXER_US / 1000) / 1000, want = primed ? 1 : prime;
        if (r_count < want || !rr) pthread_cond_timedwait(&r_cond, &r_lock, &ts);
        rr = r_rate;
        size_t chunk = (size_t)rr * CHUNK_MS / 1000, have = r_count;
        prime = (size_t)rr * (PREBUF_MS + MIXER_US / 1000) / 1000;
        if (!rr || (!primed && have < prime) || !have) {
            pthread_mutex_unlock(&r_lock);
            if (primed && !have) { primed = 0; if (atomic_load(&streaming)) { atomic_fetch_add(&dry_count, 1); fprintf(stderr, "a2dp: buffer ran dry\n"); } }
            if (open && !atomic_load(&streaming) && (quiet ? ms() - quiet > 3000 : (quiet = ms(), 0))) {
                bt_close(); open = 0; fprintf(stderr, "a2dp: output closed\n");
            }
            continue;
        }
        if (!primed) { primed = 1; avg = PREBUF_MS; acc = 0; prev[0] = ring[r_head * 2]; prev[1] = ring[r_head * 2 + 1]; }
        quiet = 0;
        /* The source's clock against the mixer's (a phone was 0.2 % fast): play the input slightly faster or slower,
         * resampled linearly, so the buffer stays near PREBUF_MS.  base follows the long-term drift, the error term the
         * rest; both stay far below audible pitch changes. */
        avg = 0.98 * avg + 0.02 * ((double)have * 1000 / rr);
        double err = avg - PREBUF_MS, ratio;
        base += err * 1e-7; base = base < 0.99 ? 0.99 : base > 1.01 ? 1.01 : base;
        ratio = base + (err < -100 ? -100 : err > 100 ? 100 : err) * 2e-5;
        if (chunk > MAXCHUNK) chunk = MAXCHUNK;             /* ring_push keeps rates <= 48 kHz; buf must hold whatever */
        acc += chunk * ratio;
        size_t n = (size_t)acc, out = chunk;
        if (n > MAXCHUNK + 7) n = MAXCHUNK + 7;
        if (n > have) { n = have; out = (size_t)(n / ratio); acc = 0; if (!out) out = 1; } else acc -= n;
        if (out > MAXCHUNK) out = MAXCHUNK;                 /* n / ratio with ratio < 1: a few frames over */
        for (size_t i = 0; i < n; i++) { size_t at = (r_head + i) % RING_FRAMES * 2; in[2 * i] = ring[at]; in[2 * i + 1] = ring[at + 1]; }
        r_head = (r_head + n) % RING_FRAMES; r_count -= n;
        long dropped = r_dropped; r_dropped = 0;
        pthread_mutex_unlock(&r_lock);                      /* resampled outside: the HCI thread pushes meanwhile */
        for (size_t j = 0; j < out; j++) {                  /* position in the input, prev being index -1 */
            double pos = -1 + (j + 1) * (double)n / out; long i0 = (long)floor(pos); double f = pos - i0;
            for (int c = 0; c < 2; c++) {
                double x0 = i0 < 0 ? prev[c] : in[2 * i0 + c], x1 = in[2 * (i0 + 1 < (long)n ? i0 + 1 : (long)n - 1) + c];
                buf[2 * j + c] = (int16_t)lrint(x0 + (x1 - x0) * f);
            }
        }
        if (n) { prev[0] = in[2 * n - 2]; prev[1] = in[2 * n - 1]; }
        if (dropped) fprintf(stderr, "a2dp: buffer full, %ld frames dropped\n", dropped);

        if (open && rate != rr) { bt_close(); open = 0; }
        if (!open) {
            if (bt_open(rr, 2)) { usleep(200000); continue; }
            open = 1; rate = rr; fprintf(stderr, "a2dp: output open, %u Hz\n", rr);
        }
        if (core_state() != IDLE) for (size_t i = 0; i < 2 * out; i++) buf[i] /= 6;     /* duck under the voice assistant
                                                                                       (a snapshot without core_lock) */
        while (bt_queued_us() > MIXER_US) usleep(5000);
        for (size_t i = 0; i < 2 * out; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > peak) peak = v; }
        if (bt_write(buf, out * 4) < 0) fprintf(stderr, "a2dp: mixer write failed\n");
        if (ms() - stat > 60000 && atomic_load(&streaming)) {     /* peak: whether the source sends more than silence */
            stat = ms();
            fprintf(stderr, "a2dp: buffer %.0f ms, source clock %+.0f ppm, peak %.0f dBFS; packets: longest gap %d ms, %d gaps > 100 ms; ran dry %d times\n",
                    avg, (base - 1) * 1e6, 20 * log10((peak + 1) / 32768.0), atomic_exchange(&gap_max, 0), atomic_exchange(&gaps_long, 0), atomic_exchange(&dry_count, 0));
            peak = 0;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- AVDTP */

enum { ST_IDLE, ST_CONFIGURED, ST_OPEN, ST_STREAMING };

/* stream endpoints: one per codec the device can decode, SEID = index + 1 */
static int seps[8], nseps;

static struct { struct link *l; int state, media, rseid, delay, label, sep; unsigned char cfg[16]; unsigned ncfg; } av;

static const struct a2dp_codec *sep_codec(int seid) { return seid >= 1 && seid <= nseps ? &a2dp_codecs[seps[seid - 1]] : NULL; }

/* AAC goes into the firmware's FFmpeg 4 decoder, years of fixes behind, fed what any phone in radio range sends once it
 * is paired (a2dp_codecs.c checks the LATM configuration, not the rest of the bitstream).  SBC and aptX are decoded by
 * code of ours that is small and checked.  So the AAC endpoint is only offered while the user wants it; its SEID stays
 * the same either way.  A source discovers the endpoints when it connects: a change counts from the next connection. */
static atomic_int aac_on;
int a2dp_aac(int set) { if (set >= 0) atomic_store(&aac_on, set != 0); return atomic_load(&aac_on); }
static int sep_offered(int seid) { const struct a2dp_codec *c = sep_codec(seid); return c && (c->type != A2DP_AAC || atomic_load(&aac_on)); }

static void set_streaming(int on)
{
    if (atomic_exchange(&streaming, on) == on) return;
    fprintf(stderr, "a2dp: %s\n", on ? "streaming" : "stream stopped");
    core_music(MUSIC_BLUETOOTH, on);
}

static void av_reset(void)
{
    set_streaming(0);
    if (av.l) sep_codec(av.sep)->close();
    av.l = NULL; av.state = ST_IDLE; av.media = 0; av.delay = 0; av.sep = 0;
}

void av_send(struct link *l, const void *p, size_t n) { struct chan *c = chan_by(l, l->av_sig); if (c) l2_send(l, c->rcid, p, n); }

void av_reply(struct link *l, unsigned label, unsigned sig, int accept, const unsigned char *d, size_t n)
{
    unsigned char r[48] = { label << 4 | (accept ? 2 : 3), sig };
    if (n > sizeof r - 2) n = sizeof r - 2;
    memcpy(r + 2, d, n); av_send(l, r, 2 + n);
}

static void delay_report(void)                          /* how long our audio takes, 1/10 ms: sources delay video by it */
{
    unsigned d = (PREBUF_MS + MIXER_US / 1000 + OUTPUT_MS) * 10;
    unsigned char p[5] = { (unsigned char)((++av.label & 15) << 4), AV_DELAY, av.rseid << 2, d >> 8, d };
    av_send(av.l, p, 5);
}

/* service capabilities: media transport, media codec (audio, the codec's type and information), delay reporting */
static size_t caps_of(const struct a2dp_codec *c, const unsigned char *info, unsigned ninfo, int delay, unsigned char *o)
{
    size_t n = 0;
    o[n++] = 1; o[n++] = 0;
    o[n++] = 7; o[n++] = 2 + ninfo; o[n++] = 0x00; o[n++] = c->type; memcpy(o + n, info, ninfo); n += ninfo;
    if (delay) { o[n++] = 8; o[n++] = 0; }
    return n;
}

/* Set Configuration / Reconfigure for endpoint seid: the categories asked for.  Returns 0 or the error, *cat the category
 * at fault; the codec configuration lands in av.cfg */
static int av_config(int seid, const unsigned char *d, size_t n, unsigned *cat, int reconf)
{
    const struct a2dp_codec *codec = sep_codec(seid); int have = 0, delay = 0;
    for (size_t i = 0; i + 2 <= n; i += 2 + d[i + 1]) {
        unsigned c = d[i], len = d[i + 1];
        *cat = c;
        if (i + 2 + len > n) return E_BAD_SERV_CATEGORY;
        if (c == 1 && !reconf) continue;                    /* media transport */
        if (c == 8 && !reconf) { delay = 1; continue; }     /* delay reporting */
        if (c != 7) return reconf ? 0x1e : E_BAD_SERV_CATEGORY;     /* invalid capabilities (reconfigure) / bad category */
        const unsigned char *s = d + i + 2;
        if (len < 2 || len - 2 > sizeof av.cfg || s[0] >> 4 != 0 || s[1] != codec->type || !codec->check(s + 2, len - 2))
            return E_UNSUPPORTED_CONF;
        memcpy(av.cfg, s + 2, len - 2); av.ncfg = len - 2; have = 1;
    }
    if (!have && !reconf) { *cat = 7; return E_UNSUPPORTED_CONF; }
    if (!reconf) av.delay = delay;
    return 0;
}

static void log_config(struct link *l)
{
    char hex[40] = ""; for (unsigned i = 0; i < av.ncfg && i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", av.cfg[i]);
    fprintf(stderr, "a2dp: %012llx configured %s (%s)%s\n", (unsigned long long)l->addr, sep_codec(av.sep)->name, hex,
            av.delay ? ", delay reporting" : "");
}

void av_rx(struct link *l, const unsigned char *p, size_t n)
{
    if (n < 2) return;
    unsigned label = p[0] >> 4, type = p[0] >> 2 & 3, msg = p[0] & 3, sig = p[1] & 0x3f;
    const unsigned char *d = p + 2; size_t dn = n - 2; unsigned char r[48]; unsigned cat = 0; int e;
    if (type != 0 || msg != 0) return;                     /* fragments (never this small) and answers to our delay reports */
    int mine = av.l == l, seid = dn >= 1 ? d[0] >> 2 : 0; const struct a2dp_codec *codec = sep_codec(seid);
    int ours = mine && seid == av.sep;                      /* the endpoint this link has configured */
    if (!ours && !sep_offered(seid)) codec = NULL;          /* one configured before the switch went off plays on */
    switch (sig) {
    case AV_DISCOVER: {
        int k = 0;
        for (int i = 0; i < nseps; i++) {                   /* in use: taken by the stream, or by another source */
            if (!sep_offered(i + 1) && !(mine && av.sep == i + 1)) continue;
            r[2 * k] = (i + 1) << 2 | (av.l && (!mine || av.sep == i + 1) ? 2 : 0);
            r[2 * k + 1] = 0 << 4 | 1 << 3;                 /* audio, sink */
            k++;
        }
        av_reply(l, label, sig, 1, r, 2 * k);
        break; }
    case AV_GET_CAP: case AV_GET_ALL_CAP:
        if (!codec) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, r, caps_of(codec, codec->caps, codec->ncaps, sig == AV_GET_ALL_CAP, r));
        break;
    case AV_SET_CONF:
        if (dn < 2 || !codec) { r[0] = 0; r[1] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); break; }
        if (av.l) { r[0] = 0; r[1] = E_SEP_IN_USE; av_reply(l, label, sig, 0, r, 2); break; }
        if ((e = av_config(seid, d + 2, dn - 2, &cat, 0)) || (codec->open(av.cfg, av.ncfg) && (cat = 7, e = E_UNSUPPORTED_CONF))) {
            r[0] = cat; r[1] = e; av_reply(l, label, sig, 0, r, 2); break;
        }
        av.l = l; av.sep = seid; av.state = ST_CONFIGURED; av.rseid = d[1] >> 2;
        log_config(l);
        av_reply(l, label, sig, 1, NULL, 0);
        if (av.delay) delay_report();
        break;
    case AV_RECONF:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        if (av.state != ST_OPEN) { r[0] = 0; r[1] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 2); break; }
        if ((e = av_config(seid, d + 1, dn - 1, &cat, 1))) { r[0] = cat; r[1] = e; av_reply(l, label, sig, 0, r, 2); break; }
        codec->close();
        if (codec->open(av.cfg, av.ncfg)) { r[0] = 7; r[1] = E_UNSUPPORTED_CONF; av_reply(l, label, sig, 0, r, 2); av_reset(); break; }
        log_config(l);
        av_reply(l, label, sig, 1, NULL, 0);
        break;
    case AV_GET_CONF:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, r, caps_of(codec, av.cfg, av.ncfg, av.delay, r));
        break;
    case AV_OPEN:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        if (av.state != ST_CONFIGURED) { r[0] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 1); break; }
        av.state = ST_OPEN; av_reply(l, label, sig, 1, NULL, 0);        /* the media channel comes next */
        break;
    case AV_START: case AV_SUSPEND:
        if (!ours) { r[0] = dn ? d[0] : 0; r[1] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); break; }
        if (sig == AV_START ? av.state != ST_OPEN : av.state != ST_STREAMING) {
            if ((sig == AV_START && av.state == ST_STREAMING) || (sig == AV_SUSPEND && av.state == ST_OPEN)) { av_reply(l, label, sig, 1, NULL, 0); break; }
            r[0] = d[0]; r[1] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 2); break;
        }
        av.state = sig == AV_START ? ST_STREAMING : ST_OPEN;
        if (sig == AV_START) { atomic_store(&yielded, 0); atomic_store(&button_paused_at, 0); }  /* started again: wanted */
        av_reply(l, label, sig, 1, NULL, 0);
        set_streaming(sig == AV_START);
        break;
    case AV_CLOSE: case AV_ABORT:
        if (sig == AV_CLOSE && !ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, NULL, 0);
        if (mine) av_reset();                               /* the source closes the media channel itself */
        break;
    case AV_DELAY: r[0] = E_NOT_SUPPORTED; av_reply(l, label, sig, 0, r, 1); break;   /* we are the sink */
    case AV_SECURITY: r[0] = E_NOT_SUPPORTED; av_reply(l, label, sig, 0, r, 1); break;
    default: { unsigned char g[2] = { label << 4 | 1, sig }; av_send(l, g, 2); break; }     /* general reject */
    }
}

static void media_rx(const unsigned char *p, size_t n)
{
    static long long last;
    long long t = ms();
    if (last && t - last < 5000) { int g = (int)(t - last); if (g > atomic_load(&gap_max)) atomic_store(&gap_max, g); if (g > 100) atomic_fetch_add(&gaps_long, 1); }
    last = t;
    if (atomic_load(&streaming) && !atomic_load(&yielded)) sep_codec(av.sep)->packet(p, n, ring_push);
}

/* ---------------------------------------------------------------- hooks for bt_link.c */

struct link *sink_link(void) { return av.l; }
int  sink_streaming(void) { return atomic_load(&streaming); }

void sink_media_opened(struct link *l, struct chan *c) { if (av.l == l && av.state == ST_OPEN && !av.media) av.media = c->lcid; }

void sink_closed(struct link *l) { if (av.l == l) av_reset(); }

void sink_media_closed(struct link *l, int lcid)
{
    if (av.l == l && lcid == av.media) { av.media = 0; if (av.state >= ST_OPEN) { av.state = ST_CONFIGURED; set_streaming(0); } }
}

void sink_media_rx(struct link *l, int cid, const unsigned char *p, size_t n) { if (av.l == l && cid == av.media) media_rx(p, n); }

void sink_start(void)
{
    for (int i = 0; i < a2dp_ncodecs && nseps < (int)(sizeof seps / sizeof *seps); i++) if (a2dp_codecs[i].usable()) seps[nseps++] = i;
    { char names[80] = ""; for (int i = 0; i < nseps; i++) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "", a2dp_codecs[seps[i]].name);
      fprintf(stderr, "a2dp: codecs %s\n", names); }
    pthread_t t; pthread_create(&t, NULL, player, NULL); pthread_detach(t);
}

/* ---------------------------------------------------------------- API (a2dp.h), any thread */

int a2dp_button(int resume)
{
    if (!avrcp_present()) return 0;
    long long t = atomic_load(&button_paused_at);
    int paused = t && ms() - t < 30 * 60 * 1000LL;          /* by the button, and the device has not started since */
    if (!resume) {                                          /* while it streams: pause, or play if we just paused it (a
                                                               device may stream silence for a while after pausing) */
        if (!atomic_load(&streaming) || atomic_load(&yielded)) return 0;
        atomic_store(&button_paused_at, paused ? 0 : ms()); avrcp_press(paused ? KEY_PLAY : KEY_PAUSE);
        return 1;
    }
    if (!paused) return 0;
    atomic_store(&button_paused_at, 0); avrcp_press(KEY_PLAY);
    return 1;
}

void a2dp_pause(void)
{
    if (!atomic_load(&streaming)) return;
    atomic_store(&button_paused_at, 0);
    if (avrcp_present()) { avrcp_press(KEY_PAUSE); return; }
    if (!atomic_exchange(&yielded, 1)) fprintf(stderr, "a2dp: no remote control: silent while the other source plays\n");
}

void a2dp_unyield(void) { if (atomic_exchange(&yielded, 0)) fprintf(stderr, "a2dp: audible again\n"); }
