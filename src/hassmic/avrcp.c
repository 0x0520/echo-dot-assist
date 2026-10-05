/*
 * AVRCP on the Bluetooth speaker's links (bt_int.h).  The Echo's volume is the device's volume slider, both ways; play /
 * pause to the device (action button, another music source starting on the Echo).
 *
 * Both roles over one AVCTP channel, which the device opens.  Target (the device controls us): absolute volume, the
 * phone's volume slider is the Echo's volume and the other way round.  Controller (we control the device): play and
 * pause as pass-through commands, like the buttons of headphones.  The speaker we play to is the other way round; its
 * side of this (we set its absolute volume) is in a2dp_source.c.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "a2dp.h"
#include "bt_int.h"
#include "core.h"
#include "hci.h"
#include "threadname.h"

static atomic_int echo_volume = -1, vol_dirty, key_req, avrcp_links;

/* Setting the Echo's volume runs helper programs (mixer property, LED ring) and takes core_lock: far too slow for the
 * controller thread, whose stalls empty the jitter buffer.  A thread of its own does it, latest request wins. */
static pthread_mutex_t vol_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vol_cond = PTHREAD_COND_INITIALIZER;
static int vol_want = -1;                               /* vol_lock */

static void *volume_thread(void *arg)
{
    thread_name("avrcp volume");
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&vol_lock);
        while (vol_want < 0) pthread_cond_wait(&vol_cond, &vol_lock);
        int v = vol_want; vol_want = -1;
        pthread_mutex_unlock(&vol_lock);
        pthread_mutex_lock(&core_lock); core_set_volume(v); pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

void volume_set_later(int pct)
{
    pthread_mutex_lock(&vol_lock); vol_want = pct; pthread_cond_signal(&vol_cond); pthread_mutex_unlock(&vol_lock);
}

int volume_now(void)                                    /* 0..100; read at start, then kept up to date by the core */
{
    int v = atomic_load(&echo_volume);
    return v < 0 ? 50 : v;
}

void avctp_send(struct link *l, unsigned label, int response, const unsigned char *avc, size_t n)
{
    unsigned char b[48]; struct chan *c = chan_by(l, l->avctp);
    if (!c || n > sizeof b - 3) return;
    b[0] = label << 4 | (response ? 2 : 0); b[1] = 0x11; b[2] = 0x0e;      /* profile: A/V remote control */
    memcpy(b + 3, avc, n); l2_send(l, c->rcid, b, 3 + n);
}

static void vendor_reply(struct link *l, unsigned label, unsigned code, unsigned pdu, const unsigned char *par, size_t n)
{
    unsigned char a[32] = { code, PANEL, AVC_VENDOR, 0x00, 0x19, 0x58, pdu, 0, n >> 8, n };      /* Bluetooth SIG company */
    memcpy(a + 10, par, n); avctp_send(l, label, 1, a, 10 + n);
}

void avrcp_rx(struct link *l, const unsigned char *p, size_t n)
{
    unsigned char r[16];
    if (n < 6 || (p[0] >> 2 & 3)) return;                  /* fragments: never this small */
    unsigned label = p[0] >> 4;
    if ((p[0] & 2) && is_speaker(l) && n >= 13 && p[5] == AVC_VENDOR) {     /* the speaker answers us: its volume */
        size_t pl = (size_t)(p[11] << 8 | p[12]);
        out_avrcp_answer(p[3], p[9], p + 13, pl < n - 13 ? pl : n - 13);
        return;
    }
    if (p[0] & 2) {                                         /* answers to our pass-through commands */
        if (n >= 7 && p[5] == AVC_PASS_THROUGH && !(p[6] & 0x80))
            fprintf(stderr, "a2dp: %012llx: %s %s\n", (unsigned long long)l->addr, (p[6] & 0x7f) == KEY_PLAY ? "play" : "pause",
                    p[3] == AVC_ACCEPTED ? "accepted" : p[3] == AVC_NOT_IMPLEMENTED ? "not implemented" : "rejected");
        return;
    }
    if (p[1] != 0x11 || p[2] != 0x0e) {                     /* another profile: "invalid profile identifier" */
        r[0] = label << 4 | 3; r[1] = p[1]; r[2] = p[2]; l2_send(l, chan_by(l, l->avctp)->rcid, r, 3); return;
    }
    const unsigned char *a = p + 3; size_t an = n - 3;
    if (a[2] == AVC_UNIT_INFO || a[2] == AVC_SUBUNIT_INFO) {
        unsigned char u[8] = { AVC_STABLE, 0xff, a[2], 0x07, PANEL, 0xff, 0xff, 0xff };
        if (a[2] == AVC_UNIT_INFO) { u[5] = 0x00; u[6] = 0x19; u[7] = 0x58; }
        avctp_send(l, label, 1, u, 8);
        return;
    }
    if (a[2] != AVC_VENDOR || an < 10 || a[3] || a[4] != 0x19 || a[5] != 0x58) {   /* pass-through to us and the rest */
        unsigned char e[16]; size_t k = an < sizeof e ? an : sizeof e;
        memcpy(e, a, k); e[0] = AVC_NOT_IMPLEMENTED; avctp_send(l, label, 1, e, k);
        return;
    }
    unsigned pdu = a[6]; size_t plen = a[8] << 8 | a[9]; const unsigned char *par = a + 10;
    if (10 + plen > an) plen = 0;
    /* The speaker we play to gets no absolute volume: the mixer applies the Echo's volume before encoding (stock sets
     * persist.bluetooth.disableabsvol too).  Offered, PipeWire took the Echo's 30 % as the stream's volume, -31 dB more. */
    if (is_speaker(l) && (pdu == PDU_REGISTER || pdu == PDU_SET_VOLUME || (pdu == PDU_GET_CAPS && plen >= 1 && par[0] == 3))) {
        if (pdu == PDU_GET_CAPS) { unsigned char c[2] = { 3, 0 }; vendor_reply(l, label, AVC_STABLE, pdu, c, 2); }
        else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }
        return;
    }
    switch (pdu) {
    case PDU_GET_CAPS:
        if (plen >= 1 && par[0] == 2) { unsigned char c[5] = { 2, 1, 0x00, 0x19, 0x58 }; vendor_reply(l, label, AVC_STABLE, pdu, c, 5); }
        else if (plen >= 1 && par[0] == 3) { unsigned char c[3] = { 3, 1, EVENT_VOLUME }; vendor_reply(l, label, AVC_STABLE, pdu, c, 3); }
        else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }       /* invalid parameter */
        break;
    case PDU_REGISTER:
        if (plen >= 1 && par[0] == EVENT_VOLUME) {
            l->vol_label = label; l->vol_pct = volume_now();
            r[0] = EVENT_VOLUME; r[1] = (l->vol_pct * 127 + 50) / 100; vendor_reply(l, label, AVC_INTERIM, pdu, r, 2);
        } else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }
        break;
    case PDU_SET_VOLUME:
        if (plen >= 1) {
            unsigned v = par[0] & 0x7f; int pct = (v * 100 + 63) / 127;
            l->vol_pct = pct;                               /* no notification back for its own change */
            atomic_store(&echo_volume, pct);
            volume_set_later(pct);
            r[0] = v; vendor_reply(l, label, AVC_ACCEPTED, pdu, r, 1);
        } else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }
        break;
    default: r[0] = 0x00; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); break;    /* invalid command */
    }
}

static struct link *avrcp_link(void)                    /* the streaming device if it has AVRCP, else any that has */
{
    struct link *s = sink_link();
    if (s && s->avctp) return s;
    for (struct link *l = NULL; (l = link_next(l)); ) if (l->avctp && !is_speaker(l)) return l;
    return NULL;
}

static void avctp_open(struct link *l) { chan_open(l, PSM_AVCTP); }   /* the device has not opened AVRCP: we do */

void avrcp_upkeep(void)
{
    for (struct link *k = NULL; (k = link_next(k)); ) {
        if (!k->avctp_at || ms() < k->avctp_at) continue;
        k->avctp_at = 0;
        int pending = 0; for (int j = 0; j < MAX_CHANS; j++) pending |= k->ch[j].used && k->ch[j].psm == PSM_AVCTP;
        if (!k->avctp && !pending && k->enc) avctp_open(k);
    }
    int key = atomic_exchange(&key_req, 0); struct link *l;
    if (key && (l = avrcp_link())) {                        /* press and release */
        for (int release = 0; release < 2; release++) {
            unsigned char a[5] = { AVC_CONTROL, PANEL, AVC_PASS_THROUGH, key | (release ? 0x80 : 0), 0 };
            avctp_send(l, l->av_label++ & 15, 0, a, 5);
        }
        fprintf(stderr, "a2dp: %012llx: %s\n", (unsigned long long)l->addr, key == KEY_PLAY ? "play" : "pause");
    }
    if (atomic_exchange(&vol_dirty, 0)) for (l = NULL; (l = link_next(l)); ) {
        int pct = volume_now();
        if (!l->avctp || l->vol_label < 0 || pct == l->vol_pct) continue;
        unsigned char c[2] = { EVENT_VOLUME, (pct * 127 + 50) / 100 };
        vendor_reply(l, l->vol_label, AVC_CHANGED, PDU_REGISTER, c, 2);
        l->vol_label = -1; l->vol_pct = pct;                /* the device registers again */
    }
}

void avrcp_opened(struct link *l, struct chan *c)
{
    l->avctp = c->lcid; l->vol_label = -1; atomic_fetch_add(&avrcp_links, 1);
    fprintf(stderr, "a2dp: %012llx: remote control\n", (unsigned long long)l->addr);
}

void avrcp_closed(struct link *l) { l->avctp = 0; atomic_fetch_sub(&avrcp_links, 1); }

void volume_note(int pct) { atomic_store(&echo_volume, pct); }

void volume_init(void) { pthread_mutex_lock(&core_lock); atomic_store(&echo_volume, core_volume()); pthread_mutex_unlock(&core_lock); }

void avrcp_start(void) { pthread_t t; pthread_create(&t, NULL, volume_thread, NULL); pthread_detach(t); }

/* any thread */
int  avrcp_present(void) { return atomic_load(&avrcp_links) != 0; }
void avrcp_press(int key) { atomic_store(&key_req, key); hci_poke(); }
void a2dp_volume_changed(int percent) { atomic_store(&echo_volume, percent); atomic_store(&vol_dirty, 1); hci_poke(); }
