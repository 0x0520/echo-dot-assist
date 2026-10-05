/*
 * Playing to a Bluetooth speaker (A2DP source; bt_int.h).
 * The radio side; btout.c serves the mixer, which plays everything to the speaker once routed.  Home Assistant's
 * "Bluetooth speaker search" inquires for up to OUT_SEARCH_S for speakers in pairing mode (class of device audio), takes
 * the strongest, pairs as phones pair with us (Just Works, or PIN 0000) and remembers it in state/bt_speaker.  "Play on
 * Bluetooth speaker" keeps a link to it: paged while away (backing off), and accepted when it pages us (speakers call
 * their last source when switched on).  On the link we configure SBC ourselves as AVDTP initiator (discover,
 * capabilities, set configuration, open, media channel); a speaker that configures our source endpoint itself first is
 * followed instead.  START and SUSPEND follow the mixer's HAL (btout_want). */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "a2dp.h"
#include "bt_int.h"
#include "btout.h"
#include "core.h"
#include "hci.h"

#define OUT_SEID 0x30                   /* our source endpoint; the sink endpoints are 1..nseps */
#define OUT_BITPOOL 53                  /* SBC "high quality" for joint stereo 44.1 kHz: 328 kbit/s */
#define OUT_SEARCH_S 60
#define OUT_PAIR_S 120                  /* after the search found it: to page it and pair (speakers leave pairing mode too) */
#define OUT_DELAY_MS 250                /* default latency of a speaker for Sendspin (SBC sinks buffer 150..250 ms) */

enum { O_IDLE, O_DISCOVER, O_CAPS, O_SETCONF, O_OPEN, O_ACP, O_MEDIA, O_READY, O_START, O_STREAMING, O_SUSPEND };
enum { S_NONE, S_OFF, S_AWAY, S_CONNECTING, S_READY, S_PLAYING };      /* out_state, for Home Assistant */
enum { A_NO = -1, A_UNKNOWN, A_ASKED, A_ON };                            /* out.absvol */

static struct {
    uint64_t addr; char name[80]; int on;               /* the speaker (state/bt_speaker), whether to play on it */
    struct link *l;                                     /* its link */
    int phase, label, sig; long long sent_at;           /* our AVDTP command awaiting its answer (sig 0: none) */
    int seps[8], nseps, sep_i, rseid, bp_min, bitpool, media; unsigned mtu;
    int sig_lcid; long long sig_at, int_at;             /* AVDTP signalling we opened, when; when we configure */
    int absvol, sent_v; long long avol_at;              /* AVRCP absolute volume (A_*), the value we set last */
    long long next_page; int paging, failures, pairing, psrm, clock, delay_report;
    long long pair_until;                               /* pairing: the search's window to pair in ends */
} out;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;          /* out.name for other threads */
static atomic_int out_on_req = -1, out_search_req = -1, out_searching, out_enabled, out_delay = OUT_DELAY_MS, out_save_req,
                  out_state, out_live;
static long long search_until; static int inquiring;
static struct { uint64_t addr; int rssi, psrm, clock; char name[80]; } cand;

int is_speaker(const struct link *l) { return out.addr && l->addr == out.addr; }
/* Only while "Bluetooth speaker search" has just picked it.  Not merely because no key is there: one that a failed
 * authentication used to delete let a device with the speaker's address pair Just Works in its place. */
int out_may_pair(uint64_t a) { return out.addr && a == out.addr && out.pairing; }

static const char *speaker_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/bt_speaker", d ? d : "/data/local/hassmic/state");
    return p;
}

static void speaker_load(void)                  /* address, play on it, latency (ms), name */
{
    char l[160]; unsigned long long a; int on, d, n = 0; FILE *f = fopen(speaker_path(), "r");
    if (!f) return;
    if (fgets(l, sizeof l, f) && sscanf(l, "%llx %d %d %n", &a, &on, &d, &n) == 3 && n) {
        l[strcspn(l, "\n")] = 0;
        out.addr = a; out.on = on != 0; out.psrm = 1; atomic_store(&out_enabled, out.on);   /* page scan R1: most devices */
        atomic_store(&out_delay, d >= 0 && d <= 2000 ? d : OUT_DELAY_MS);
        snprintf(out.name, sizeof out.name, "%s", l + n);
        fprintf(stderr, "a2dp: speaker %012llx \"%s\"%s\n", a, out.name, out.on ? ", playing on it" : "");
    }
    fclose(f);
}

static void speaker_save(void)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", speaker_path());
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "a2dp: cannot write %s\n", tmp); return; }
    pthread_mutex_lock(&out_lock);
    fprintf(f, "%012llx %d %d %s\n", (unsigned long long)out.addr, out.on, atomic_load(&out_delay), out.name);
    pthread_mutex_unlock(&out_lock);
    if (fclose(f) || rename(tmp, speaker_path())) { unlink(tmp); fprintf(stderr, "a2dp: cannot write %s\n", speaker_path()); }
}

static void out_name(const char *name)
{
    pthread_mutex_lock(&out_lock); snprintf(out.name, sizeof out.name, "%s", name); pthread_mutex_unlock(&out_lock);
}

static void out_phase(int p)
{
    out.phase = p;
    atomic_store(&out_live, p == O_STREAMING);
}

static void out_stream_reset(void)
{
    if (out.phase >= O_READY) btout_gone();
    out.absvol = A_UNKNOWN;                             /* asked again once there is a stream: btout_gone forgot it */
    out_phase(O_IDLE); out.media = 0; out.sig = 0; out.nseps = 0;
}

static void out_disconnect(struct link *l)
{
    if (!l || l->dropping) return;
    unsigned char d[3]; put16(d, l->handle); d[2] = 0x13;      /* remote user terminated connection */
    l->dropping = 1; later(OP_DISCONNECT, d, 3);
}

/* the stream failed: drop the link, page again later */
static void out_fail(const char *why)
{
    fprintf(stderr, "a2dp: speaker %012llx: %s\n", (unsigned long long)out.addr, why);
    out_stream_reset();
    out_disconnect(out.l);
    out.next_page = ms() + 30000;
}

static void out_cmd(int sig, const unsigned char *d, size_t n)
{
    unsigned char p[24]; struct chan *c = out.l ? chan_by(out.l, out.l->av_sig) : NULL;
    if (!c || n > sizeof p - 2) return;
    out.label = (out.label + 1) & 15; out.sig = sig; out.sent_at = ms();
    p[0] = out.label << 4; p[1] = sig; memcpy(p + 2, d, n);
    l2_send(out.l, c->rcid, p, 2 + n);
}

static void out_seid_cmd(int sig) { unsigned char d = out.rseid << 2; out_cmd(sig, &d, 1); }

/* our endpoint's capabilities: what the encoder does (sbc.c), 44.1 kHz (the HAL's rate) joint stereo, 16 blocks, 8
 * subbands, loudness; bitpool 2..OUT_BITPOOL */
static const unsigned char out_caps[] = { 1, 0, 7, 6, 0x00, 0x00, 0x21, 0x15, 2, OUT_BITPOOL };

static int sbc_fits(const unsigned char *s)     /* a sink's SBC capabilities (4 bytes): can it take ours */
{
    return (s[0] & 0x20) && (s[0] & 0x01) && (s[1] & 0x10) && (s[1] & 0x04) && (s[1] & 0x01) && s[2] <= s[3] && s[2] <= OUT_BITPOOL;
}

static void out_media_open(struct link *l, struct chan *c)
{
    out.media = c->lcid; out.mtu = c->rmtu < 1024 ? c->rmtu : 1024; out_phase(O_READY);
    fprintf(stderr, "a2dp: speaker %012llx ready, SBC bitpool %d, MTU %u\n", (unsigned long long)l->addr, out.bitpool, c->rmtu);
    btout_ready(l->addr, out.name[0] ? out.name : l->name, out.bitpool, out.mtu);
}

/* answers to our commands */
static void out_answer(struct link *l, int ok, const unsigned char *d, size_t dn)
{
    switch (out.phase) {
    case O_DISCOVER:
        if (!ok) { out_fail("discover refused"); return; }
        out.nseps = 0;
        for (size_t i = 0; i + 1 < dn && out.nseps < 8; i += 2)        /* audio sinks not in use */
            if (!(d[i] & 2) && d[i + 1] >> 4 == 0 && d[i + 1] & 8) out.seps[out.nseps++] = d[i] >> 2;
        if (!out.nseps) { out_fail("no free audio sink endpoint"); return; }
        out.sep_i = 0; out.rseid = out.seps[0]; out_phase(O_CAPS); out_seid_cmd(AV_GET_CAP);
        return;
    case O_CAPS:
        for (size_t i = 0; ok && i + 2 <= dn && i + 2 + d[i + 1] <= dn; i += 2 + d[i + 1]) {
            const unsigned char *s = d + i + 2;
            if (d[i] != 7 || d[i + 1] < 6 || s[0] >> 4 != 0 || s[1] != 0 || !sbc_fits(s + 2)) continue;
            out.bp_min = s[4] < 2 ? 2 : s[4]; out.bitpool = s[5] < OUT_BITPOOL ? s[5] : OUT_BITPOOL;
            unsigned char c[12] = { out.rseid << 2, OUT_SEID << 2, 1, 0, 7, 6, 0x00, 0x00, 0x21, 0x15, out.bp_min, out.bitpool };
            out_phase(O_SETCONF); out_cmd(AV_SET_CONF, c, 12);
            return;
        }
        if (++out.sep_i < out.nseps) { out.rseid = out.seps[out.sep_i]; out_seid_cmd(AV_GET_CAP); return; }
        out_fail("no SBC endpoint that takes 44.1 kHz joint stereo");
        return;
    case O_SETCONF:
        if (!ok) { out_fail("configuration refused"); return; }
        out_phase(O_OPEN); out_seid_cmd(AV_OPEN);
        return;
    case O_OPEN:
        if (!ok) { out_fail("open refused"); return; }
        out_phase(O_MEDIA); out.sent_at = ms();
        if (!chan_open(l, PSM_AVDTP)) out_fail("no room for the media channel");
        return;
    case O_START:
        if (ok) { out_phase(O_STREAMING); btout_streaming(1); }
        else { out_phase(O_READY); btout_start_failed(); fprintf(stderr, "a2dp: speaker refused to start\n"); }
        return;
    case O_SUSPEND:
        if (ok) { out_phase(O_READY); btout_streaming(0); }
        else out_phase(O_STREAMING);
        return;
    }
}

/* commands of the speaker: when it configures our endpoint itself, starts, suspends, closes, reports its delay */
static void out_command(struct link *l, unsigned label, unsigned sig, const unsigned char *d, size_t dn)
{
    unsigned char r[16]; int seid = dn ? d[0] >> 2 : 0, mine = seid == OUT_SEID;
    switch (sig) {
    case AV_DISCOVER:
        r[0] = OUT_SEID << 2 | (out.phase >= O_SETCONF ? 2 : 0); r[1] = 0x00;      /* audio, source */
        av_reply(l, label, sig, 1, r, 2);
        return;
    case AV_GET_CAP: case AV_GET_ALL_CAP:
        if (!mine) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); return; }
        av_reply(l, label, sig, 1, out_caps, sizeof out_caps);
        return;
    case AV_SET_CONF: {
        if (dn < 2 || !mine) { r[0] = 0; r[1] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); return; }
        if (out.phase >= O_SETCONF) { r[0] = 0; r[1] = E_SEP_IN_USE; av_reply(l, label, sig, 0, r, 2); return; }
        int bp = 0;
        for (size_t i = 2; i + 2 <= dn; i += 2 + d[i + 1]) {
            const unsigned char *s = d + i + 2; unsigned cat = d[i], len = d[i + 1];
            if (i + 2 + len > dn) { r[0] = cat; r[1] = E_BAD_SERV_CATEGORY; av_reply(l, label, sig, 0, r, 2); return; }
            if (cat == 1 || cat == 8) continue;
            if (cat != 7) { r[0] = cat; r[1] = E_BAD_SERV_CATEGORY; av_reply(l, label, sig, 0, r, 2); return; }
            if (len < 6 || s[0] >> 4 != 0 || s[1] != 0 || s[2] != 0x21 || s[3] != 0x15 || s[4] < 2 || s[4] > s[5] || s[4] > OUT_BITPOOL) {
                r[0] = 7; r[1] = E_UNSUPPORTED_CONF; av_reply(l, label, sig, 0, r, 2); return;
            }
            bp = s[5] < OUT_BITPOOL ? s[5] : OUT_BITPOOL;
        }
        if (!bp) { r[0] = 7; r[1] = E_UNSUPPORTED_CONF; av_reply(l, label, sig, 0, r, 2); return; }
        out.rseid = d[1] >> 2; out.bitpool = bp; out.sig = 0; out.int_at = 0; out_phase(O_ACP);
        fprintf(stderr, "a2dp: speaker %012llx configured us, SBC bitpool %d\n", (unsigned long long)l->addr, bp);
        av_reply(l, label, sig, 1, NULL, 0);
        return; }
    case AV_OPEN:
        if (!mine || out.phase != O_ACP) { r[0] = mine ? E_BAD_STATE : E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); return; }
        out_phase(O_MEDIA); out.sent_at = ms();          /* it opens the media channel */
        av_reply(l, label, sig, 1, NULL, 0);
        return;
    case AV_START: case AV_SUSPEND:
        if (!mine || out.phase < O_READY) { r[0] = seid << 2; r[1] = mine ? E_BAD_STATE : E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); return; }
        av_reply(l, label, sig, 1, NULL, 0);
        if (sig == AV_START) { if (out.phase != O_STREAMING) { out_phase(O_STREAMING); btout_streaming(1); } }
        else if (out.phase == O_STREAMING || out.phase == O_SUSPEND) { out_phase(O_READY); btout_streaming(0); }
        return;
    case AV_CLOSE: case AV_ABORT:
        av_reply(l, label, sig, 1, NULL, 0);
        fprintf(stderr, "a2dp: speaker %012llx closed the stream\n", (unsigned long long)l->addr);
        out_stream_reset(); out.int_at = ms() + 2000;    /* configure again unless it does */
        return;
    case AV_DELAY:                                      /* its latency, 1/10 ms: logged, Sendspin goes by the setting */
        if (dn >= 3 && (d[1] << 8 | d[2]) != out.delay_report) {
            out.delay_report = d[1] << 8 | d[2];
            fprintf(stderr, "a2dp: speaker reports %d ms of delay\n", out.delay_report / 10);
        }
        av_reply(l, label, sig, 1, NULL, 0);
        return;
    case AV_GET_CONF: case AV_RECONF: case AV_SECURITY: r[0] = E_NOT_SUPPORTED; av_reply(l, label, sig, 0, r, 1); return;
    default: { unsigned char g[2] = { label << 4 | 1, sig }; av_send(l, g, 2); return; }
    }
}

void out_rx(struct link *l, const unsigned char *p, size_t n)
{
    if (n < 2 || (p[0] >> 2 & 3)) return;              /* fragments: never this small */
    unsigned label = p[0] >> 4, msg = p[0] & 3, sig = p[1] & 0x3f;
    if (msg == 0) { out_command(l, label, sig, p + 2, n - 2); return; }
    if (!out.sig || label != (unsigned)out.label || sig != (unsigned)out.sig) return;     /* not what we wait for */
    out.sig = 0;
    out_answer(l, msg == 2, p + 2, n - 2);
}

/* inquiry: speakers, headphones and the like in pairing mode; the strongest wins */
void out_found(const unsigned char *r, size_t n, int eir)
{
    if (n < 14) return;
    /* audio/video: headset, hands-free, loudspeaker, headphones, portable, car, hi-fi; or anything rendering audio (a PC
     * with an A2DP sink says so in its service bits, phones do not) */
    unsigned cod = r[8] | r[9] << 8 | r[10] << 16, major = cod >> 8 & 0x1f, minor = cod >> 2 & 0x3f;
    int av = major == 4 && (minor == 1 || minor == 2 || minor == 5 || minor == 6 || minor == 7 || minor == 8 || minor == 10);
    if (!av && (cod & (1 << 18 | 1 << 21)) != (1 << 18 | 1 << 21)) return;
    char name[80] = ""; uint64_t a = addr_of(r); int rssi = (signed char)r[13];
    for (size_t i = 14; eir && i + 1 < n && r[i]; i += 1 + r[i]) {     /* EIR: length, type, data */
        if (i + 1 + r[i] > n || (r[i + 1] != 0x09 && r[i + 1] != 0x08) || r[i] < 2) continue;
        size_t k = (size_t)r[i] - 1 < sizeof name - 1 ? (size_t)r[i] - 1 : sizeof name - 1;
        memcpy(name, r + i + 2, k); name[k] = 0;
    }
    if (cand.addr && rssi <= cand.rssi && a != cand.addr) return;
    if (a != cand.addr) fprintf(stderr, "a2dp: found speaker %012llx \"%s\", %d dBm\n", (unsigned long long)a, name, rssi);
    cand.addr = a; cand.rssi = rssi; cand.psrm = r[6]; cand.clock = u16(r + 11) | 0x8000;
    if (name[0] || !cand.name[0]) snprintf(cand.name, sizeof cand.name, "%s", name);
}

static void search_end(void)
{
    if (!atomic_exchange(&out_searching, 0)) return;
    core_bt_pairing(0);
    bt_notify();
}

void out_inquiry_done(void)
{
    inquiring = 0;
    if (!atomic_load(&out_searching) || !cand.addr) return;
    if (out.l && out.l->addr != cand.addr) { out_stream_reset(); out_disconnect(out.l); }
    key_forget(cand.addr);                              /* in pairing mode: whatever it had with us is gone */
    out.addr = cand.addr; out_name(cand.name); out.on = 1; atomic_store(&out_enabled, 1);
    out.pairing = 1; out.pair_until = ms() + OUT_PAIR_S * 1000LL;
    out.failures = 0; out.next_page = ms(); out.psrm = cand.psrm; out.clock = cand.clock;
    fprintf(stderr, "a2dp: pairing with speaker %012llx \"%s\"\n", (unsigned long long)out.addr, cand.name);
    speaker_save();
    search_end();
}

/* The speaker's volume is the user's while we play on it (AVRCP absolute volume, we the controller): taken from it when
 * it connects (the ring and Home Assistant show it), the Echo's buttons and Home Assistant set it, its own buttons come
 * back; the mixer plays at full scale meanwhile and the Echo's own volume waits (btout_absvol, core_speaker).  Speakers
 * without it get their volume through the mixer, as stock did for all of them (persist.bluetooth.disableabsvol). */
static void out_vendor(unsigned ctype, unsigned pdu, const unsigned char *par, size_t n)
{
    struct link *l = out.l; unsigned char a[24] = { ctype, PANEL, AVC_VENDOR, 0x00, 0x19, 0x58, pdu, 0, n >> 8, n };
    if (!l || !l->avctp || n > sizeof a - 10) return;
    memcpy(a + 10, par, n); avctp_send(l, l->av_label++ & 15, 0, a, 10 + n);
}

static void out_register_volume(void) { unsigned char e[5] = { EVENT_VOLUME }; out_vendor(AVC_NOTIFY, PDU_REGISTER, e, 5); }
static void out_set_volume(int pct) { unsigned char v = out.sent_v = (pct * 127 + 50) / 100; out_vendor(AVC_CONTROL, PDU_SET_VOLUME, &v, 1); }

void out_avrcp_answer(unsigned code, unsigned pdu, const unsigned char *par, size_t n)
{
    if (pdu == PDU_REGISTER) {
        if (code == AVC_INTERIM && n >= 2 && par[0] == EVENT_VOLUME && out.absvol == A_ASKED) {
            unsigned v = par[1] & 0x7f; int pct = (v * 100 + 63) / 127;
            fprintf(stderr, "a2dp: speaker has absolute volume, at %d %%: it sets the volume\n", pct);
            out.absvol = A_ON; out.sent_v = v; volume_note(pct); btout_absvol(1, pct);
        } else if (code == AVC_CHANGED && n >= 2 && par[0] == EVENT_VOLUME) {
            unsigned v = par[1] & 0x7f;
            if (out.absvol == A_ON && (int)v != out.sent_v) {        /* its own buttons */
                int pct = (v * 100 + 63) / 127;
                out.sent_v = v; volume_note(pct); volume_set_later(pct);
            }
            out_register_volume();                      /* a notification is good for one change */
        } else if (code == AVC_REJECTED || code == AVC_NOT_IMPLEMENTED) {
            if (out.absvol == A_ASKED) fprintf(stderr, "a2dp: speaker has no absolute volume (0x%02x, error 0x%02x): the mixer sets the volume\n", code, n ? par[0] : 0);
            out.absvol = A_NO;
        }
    } else if (pdu == PDU_SET_VOLUME && out.absvol == A_ON && code != AVC_ACCEPTED) {
        out.absvol = A_NO; btout_absvol(0, 0);
        fprintf(stderr, "a2dp: speaker refused its volume: the mixer sets the volume\n");
    }
}

static void out_volume_upkeep(long long now)
{
    if (!out.l || !out.l->avctp || out.phase < O_READY) return;
    if (out.absvol == A_UNKNOWN) { out.absvol = A_ASKED; out.avol_at = now + 3000; out_register_volume(); }
    else if (out.absvol == A_ASKED && now > out.avol_at) { out.absvol = A_NO; fprintf(stderr, "a2dp: speaker does not answer about its volume\n"); }
    else if (out.absvol == A_ON && (volume_now() * 127 + 50) / 100 != out.sent_v) out_set_volume(volume_now());
}

void out_link_up(struct link *l, int paged)
{
    out.l = l; out.failures = 0; out.sig_lcid = 0; out.int_at = 0; out_phase(O_IDLE);
    l->auth_at = ms() + (paged ? 0 : 1500);           /* it may secure the link itself when it paged us */
    l->pend_until = ms() + 20000;
}

void out_link_lost(struct link *l)
{
    if (out.l != l) return;
    out_stream_reset(); out.l = NULL; out.sig_lcid = 0; out.int_at = 0;
    if (out.next_page < ms() + 5000) out.next_page = ms() + 5000;
}

int out_upkeep(void)
{
    long long now = ms(); int r, st;
    if ((r = atomic_exchange(&out_search_req, -1)) >= 0) {
        if (r && !atomic_load(&out_searching)) {
            atomic_store(&out_searching, 1); search_until = now + OUT_SEARCH_S * 1000LL; memset(&cand, 0, sizeof cand);
            fprintf(stderr, "a2dp: looking for a speaker\n");
            core_bt_pairing(1);
            bt_notify();
        } else if (!r) {
            if (inquiring && hci_cmd(OP_INQUIRY_CANCEL, NULL, 0) < 0) return -1;
            inquiring = 0; search_end();
        }
    }
    if ((r = atomic_exchange(&out_on_req, -1)) >= 0 && r != out.on) {
        out.on = r; out.failures = 0; out.next_page = now;
        fprintf(stderr, "a2dp: %s\n", r ? "playing on the speaker" : "playing on the Echo");
        if (!r) { out_stream_reset(); out_disconnect(out.l); }
        speaker_save();
        if (r && !out.addr && !atomic_load(&out_searching)) atomic_store(&out_search_req, 1);   /* none yet: find one */
    }
    if (atomic_exchange(&out_save_req, 0)) speaker_save();
    if (out.pairing && now > out.pair_until) { out.pairing = 0; fprintf(stderr, "a2dp: pairing with the speaker timed out\n"); }
    if (atomic_load(&out_searching)) {
        if (now > search_until) {
            fprintf(stderr, "a2dp: no speaker found\n");
            if (inquiring && hci_cmd(OP_INQUIRY_CANCEL, NULL, 0) < 0) return -1;
            inquiring = 0; search_end();
        } else if (!inquiring && !out.paging) {
            unsigned char p[5] = { 0x33, 0x8b, 0x9e, 4, 0 };    /* general inquiry, 5.12 s, any number of responses */
            if ((st = hci_cmd(OP_INQUIRY, p, 5)) < 0) return -1;
            if (!st) inquiring = 1; else { fprintf(stderr, "a2dp: inquiry refused (0x%02x)\n", st); search_until = 0; }
        }
    }
    if (out.on && out.addr && !out.l && !out.paging && !inquiring && now >= out.next_page) {
        unsigned char p[13];
        for (int i = 0; i < 6; i++) p[i] = out.addr >> 8 * i;
        put16(p + 6, 0xcc18); p[8] = out.psrm <= 2 ? out.psrm : 1; p[9] = 0; put16(p + 10, out.clock); p[12] = 1;   /* DM/DH 1-5; role switch ok */
        if ((st = hci_cmd(OP_CREATE_CONN, p, 13)) < 0) return -1;
        if (!st) out.paging = 1; else out.next_page = now + 10000;
    }
    struct link *l = out.l;
    int state = !out.addr ? S_NONE : !out.on ? S_OFF : out.phase == O_STREAMING ? S_PLAYING : out.phase >= O_READY ? S_READY
              : l || out.paging ? S_CONNECTING : S_AWAY;
    if (atomic_exchange(&out_state, state) != state) bt_notify();
    if (!l || l->dropping) return 0;
    if (!out.on) { out_disconnect(l); return 0; }
    if (!l->enc) { if (!l->pend_until) out_fail("link not encrypted"); return 0; }      /* a2dp_upkeep authenticates */
    if (!l->av_sig) {
        if (!out.sig_lcid) { out.sig_lcid = chan_open(l, PSM_AVDTP); out.sig_at = now; }
        else if (now - out.sig_at > 10000) out_fail("no AVDTP");
        return 0;
    }
    if (out.int_at && now >= out.int_at && out.phase == O_IDLE) { out.int_at = 0; out_phase(O_DISCOVER); out_cmd(AV_DISCOVER, NULL, 0); }
    if (out.sig && now - out.sent_at > 5000) { out_fail("no answer"); return 0; }
    if (out.phase == O_MEDIA && now - out.sent_at > 5000) { out_fail("no media channel"); return 0; }
    if (!out.sig && out.phase == O_READY && btout_want()) { out_phase(O_START); out_seid_cmd(AV_START); }
    else if (!out.sig && out.phase == O_STREAMING && !btout_want()) { out_phase(O_SUSPEND); out_seid_cmd(AV_SUSPEND); }
    out_volume_upkeep(now);
    struct chan *mc = out.phase == O_STREAMING ? chan_by(l, out.media) : NULL;
    unsigned char pkt[1024]; size_t n;
    while (mc && l->tx.queued < 2 && (n = btout_packet(pkt, out.mtu))) l2_send(l, mc->rcid, pkt, n);
    return 0;
}

/* ---------------------------------------------------------------- hooks for bt_link.c */

int out_page_complete(uint64_t a, unsigned status)
{
    if (!out.addr || a != out.addr || !out.paging) return 0;
    out.paging = 0;
    if (status) {                                       /* page timeout (0x04) mostly: off, or out of reach */
        if (!out.failures++) fprintf(stderr, "a2dp: speaker %012llx not reachable (0x%02x)\n", (unsigned long long)out.addr, status);
        out.next_page = ms() + (out.failures < 6 ? 10000 : 60000);
        if (out.pairing && out.failures >= 3) { out.pairing = 0; fprintf(stderr, "a2dp: pairing with the speaker failed\n"); }
        return -1;
    }
    return 1;
}

void out_named(struct link *l)
{
    if (is_speaker(l) && strcmp(out.name, l->name)) { out_name(l->name); atomic_store(&out_save_req, 1); bt_notify(); }
}

int out_paired(uint64_t a)
{
    if (!out.addr || a != out.addr) return 0;
    out.pairing = 0;
    return 1;
}

void out_sig_opened(struct link *l, struct chan *c)
{
    if (is_speaker(l)) out.int_at = ms() + (c->lcid == out.sig_lcid ? 0 : 1500);  /* it opened it: let it configure */
}

int out_media_opened(struct link *l, struct chan *c)
{
    if (!is_speaker(l)) return 0;
    if (l == out.l && out.phase == O_MEDIA && !out.media) out_media_open(l, c);
    return 1;
}

void out_avctp_closed(struct link *l)
{
    if (l == out.l && out.absvol != A_UNKNOWN) { out.absvol = A_UNKNOWN; btout_absvol(0, 0); }
}

void out_sig_closed(struct link *l)
{
    if (l == out.l) { out_stream_reset(); out.sig_lcid = 0; out.int_at = 0; }
}

int out_media_closed(struct link *l, int lcid)
{
    if (l != out.l || lcid != out.media) return 0;
    out_stream_reset(); out.int_at = ms() + 2000;
    return 1;
}

int  out_busy(void) { return atomic_load(&out_searching) || out.paging || (out.on && out.addr); }
void out_lost(void) { out.paging = 0; inquiring = 0; }
int  out_streaming(void) { return atomic_load(&out_live); }
void out_start(void) { speaker_load(); btout_start(); }

/* ---------------------------------------------------------------- API (a2dp.h), any thread */

void a2dp_out_search(int on) { atomic_store(&out_search_req, on != 0); hci_poke(); }
int  a2dp_out_searching(void) { return atomic_load(&out_searching); }
void a2dp_out_enable(int on) { atomic_store(&out_enabled, on != 0); atomic_store(&out_on_req, on != 0); hci_poke(); }
int  a2dp_out_enabled(void) { return atomic_load(&out_enabled); }

int a2dp_out_delay(int set)
{
    if (set >= 0) { atomic_store(&out_delay, set > 2000 ? 2000 : set); atomic_store(&out_save_req, 1); hci_poke(); }
    return atomic_load(&out_delay);
}

void a2dp_out_status(char *buf, unsigned n)
{
    static const char *const words[] = { "None", "Off", "Not reachable", "Connecting", "Connected", "Playing" };
    int st = atomic_load(&out_state);
    if (atomic_load(&out_searching)) { snprintf(buf, n, "Searching..."); return; }
    if (st == S_NONE) { snprintf(buf, n, "%s", words[st]); return; }
    pthread_mutex_lock(&out_lock); snprintf(buf, n, "%s: %s", out.name[0] ? out.name : "Speaker", words[st]); pthread_mutex_unlock(&out_lock);
}
