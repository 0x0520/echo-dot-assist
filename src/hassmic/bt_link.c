/*
 * Bluetooth speaker: an A2DP sink on BR/EDR, next to the LE proxy on the same controller (ble.c owns /dev/stpbt and its
 * thread; this module gets the BR/EDR events and ACL links through hci.h).  What stock Alexa did with btmanagerd and
 * BTSinkPlayer, without the Alexa app: Home Assistant's "Bluetooth pairing" switch makes the Echo discoverable for
 * A2DP_PAIR_SECONDS, like "Alexa, pair".  This is the link layer the profiles share; they are sdp.c, a2dp_sink.c,
 * avrcp.c and, the other way, a2dp_source.c (the Echo playing to a Bluetooth speaker; the mixer's side is btout.c).
 * What they share and the thread rules: bt_int.h.
 *
 *   GAP       page scan whenever a paired device exists (it connects to us, we never page), inquiry scan only while
 *             pairing.  Class of device: audio / loudspeaker.  One source streams at a time, MAX_LINKS may be connected.
 *   pairing   Secure Simple Pairing, NoInputNoOutput (Just Works: the phone asks its user, we accept), legacy PIN 0000
 *             for old devices; both only while pairing is on.  Link keys in state/bt_keys (0600), kept when a device
 *             says it has none (it may be another one under that address).  Connections from unknown devices are
 *             refused outside the pairing window.
 *   L2CAP     basic mode, we accept channels (SDP, AVDTP, AVCTP) and open only AVCTP, when the device has not 2 s after
 *             AVDTP (BlueZ does not always).  AVDTP and AVCTP need an encrypted link: a device that asks before
 *             encrypting gets "pending" while we authenticate and encrypt.  Framing, ACL queue and flow control: acl.c.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2dp.h"
#include "ble.h"
#include "bt_int.h"
#include "core.h"
#include "hci.h"
#include "keyfile.h"

#define MAX_KEYS 8
#define RX_MTU 1024                     /* what we receive per L2CAP frame; phones size their media packets to it */

enum { EV_INQUIRY_COMPLETE = 0x01, EV_CONN_COMPLETE = 0x03, EV_CONN_REQUEST, EV_DISCONNECT, EV_AUTH_COMPLETE, EV_REMOTE_NAME = 0x07, EV_ENC_CHANGE, EV_PIN_REQUEST = 0x16,
       EV_LINK_KEY_REQUEST, EV_LINK_KEY_NOTIFY, EV_KEY_REFRESH = 0x30, EV_IO_CAP_REQUEST, EV_IO_CAP_RESPONSE, EV_USER_CONFIRM,
       EV_SSP_COMPLETE = 0x36, EV_INQUIRY_RSSI = 0x22, EV_INQUIRY_EXT = 0x2f };

static void (*notify)(void);
static int started;

void bt_notify(void) { if (notify) notify(); }

/* ---------------------------------------------------------------- link keys */

static struct key { uint64_t addr; unsigned type; unsigned char k[16]; } keys[MAX_KEYS];
static int nkeys;

static void keys_load(void)                     /* one line per device: address type key (hex) */
{
    char l[128], h[40]; unsigned long long a; FILE *f = keyfile_read("bt_keys");
    if (!f) return;
    while (nkeys < MAX_KEYS && fgets(l, sizeof l, f)) {
        struct key *k = &keys[nkeys]; int ok = sscanf(l, "%llx %u %39s", &a, &k->type, h) == 3 && strlen(h) == 32;
        for (int i = 0; ok && i < 16; i++) ok = sscanf(h + 2 * i, "%2hhx", &k->k[i]) == 1;
        if (ok) { k->addr = a; nkeys++; }
    }
    fclose(f);
    if (nkeys) fprintf(stderr, "a2dp: %d paired source%s\n", nkeys, nkeys == 1 ? "" : "s");
}

static void keys_save(void)
{
    struct keyfile kf; FILE *f = keyfile_write(&kf, "bt_keys", "a2dp");
    if (!f) return;
    for (int i = 0; i < nkeys; i++) {
        fprintf(f, "%012llx %u ", (unsigned long long)keys[i].addr, keys[i].type);
        for (int j = 0; j < 16; j++) fprintf(f, "%02x", keys[i].k[j]);
        fputc('\n', f);
    }
    keyfile_commit(&kf);
}

static struct key *key_for(uint64_t a) { for (int i = 0; i < nkeys; i++) if (keys[i].addr == a) return &keys[i]; return NULL; }

void key_forget(uint64_t a)
{
    struct key *k = key_for(a);
    if (!k) return;
    fprintf(stderr, "a2dp: forgetting %012llx\n", (unsigned long long)a);
    *k = keys[--nkeys]; keys_save();
}

/* ---------------------------------------------------------------- pairing window, scan mode */

static atomic_int pair_req = -1, pairing_on;    /* request from other threads (-1 none); current state */
static long long pair_until;
static int scan_mode = -1;                      /* Write Scan Enable as sent: 0 off, 2 page, 3 inquiry + page */

static void set_pairing(int on)
{
    pair_until = on ? ms() + A2DP_PAIR_SECONDS * 1000LL : 0;
    if (atomic_exchange(&pairing_on, on) != on) {
        fprintf(stderr, "a2dp: pairing %s\n", on ? "on" : "off");
        core_bt_pairing(on);
        bt_notify();
    }
}

/* ---------------------------------------------------------------- commands asked for by events (sent in upkeep) */

static struct { unsigned op, n; unsigned char p[24]; } pend[16];
static int npend;

void later(unsigned op, const void *p, unsigned n)
{
    if (npend == (int)(sizeof pend / sizeof *pend) || n > sizeof pend[0].p) { fprintf(stderr, "a2dp: command %04x dropped\n", op); return; }
    pend[npend].op = op; pend[npend].n = n; memcpy(pend[npend].p, p, n); npend++;
}

static void later_addr(unsigned op, const unsigned char *addr, const void *extra, unsigned n)
{
    unsigned char b[24]; memcpy(b, addr, 6); if (n) memcpy(b + 6, extra, n);
    later(op, b, 6 + n);
}

/* ---------------------------------------------------------------- links, ACL, L2CAP */

static struct link links[MAX_LINKS];

static struct link *link_by(int handle) { for (int i = 0; i < MAX_LINKS; i++) if (links[i].used && links[i].handle == handle) return &links[i]; return NULL; }
struct chan *chan_by(struct link *l, int lcid) { for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used && l->ch[i].lcid == lcid) return &l->ch[i]; return NULL; }

struct link *link_next(struct link *prev)
{
    for (struct link *l = prev ? prev + 1 : links; l < links + MAX_LINKS; l++) if (l->used) return l;
    return NULL;
}

static struct acl_tx *tx_by_handle(int handle) { struct link *l = link_by(handle); return l ? &l->tx : NULL; }
/* BR/EDR's ACL queue: first fragments automatically flushable; credits shared with LE when the controller has one pool
 * (hci_acl_pool, set in a2dp_setup) */
static struct acl_queue aq = ACL_QUEUE(aq, "a2dp", tx_by_handle, 0x20, ACL_MAX_PDU);

void l2_send(struct link *l, unsigned cid, const void *pdu, size_t n) { l2cap_send(&aq, l->handle, &l->tx, cid, pdu, n); }

static void sig_send(struct link *l, unsigned code, unsigned id, const void *data, size_t n)
{
    unsigned char b[64];
    if (n > sizeof b - 4) return;
    b[0] = code; b[1] = id; put16(b + 2, n); memcpy(b + 4, data, n);
    l2_send(l, 0x0001, b, 4 + n);
}

static void config_request(struct link *l, struct chan *c)
{
    unsigned char d[8]; put16(d, c->rcid); put16(d + 2, 0); d[4] = 0x01; d[5] = 2; put16(d + 6, RX_MTU);   /* option MTU */
    if (++l->next_id > 255) l->next_id = 1;
    sig_send(l, 0x04, l->next_id, d, 8);
}

static void conn_response(struct link *l, unsigned id, unsigned lcid, unsigned rcid, unsigned result, unsigned status)
{
    unsigned char d[8]; put16(d, lcid); put16(d + 2, rcid); put16(d + 4, result); put16(d + 6, status);
    sig_send(l, 0x03, id, d, 8);
}

static void chan_opened(struct link *l, struct chan *c);
static void chan_closed(struct link *l, struct chan *c);

/* pending AVDTP channels once security is settled: go on, or refuse (security block) */
static void pending_resolve(struct link *l, int ok)
{
    for (int i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &l->ch[i];
        if (!c->used || !c->pending) continue;
        c->pending = 0;
        conn_response(l, c->pend_id, ok ? c->lcid : 0, c->rcid, ok ? 0 : 3, 0);
        if (ok) config_request(l, c); else memset(c, 0, sizeof *c);
    }
    l->auth_at = l->pend_until = 0; l->auth_sent = 0;
}

static void conn_request(struct link *l, unsigned id, const unsigned char *d, size_t n)
{
    if (n < 4) return;
    unsigned psm = u16(d), rcid = u16(d + 2); struct chan *c = NULL;
    if (psm != PSM_SDP && psm != PSM_AVDTP && psm != PSM_AVCTP) { conn_response(l, id, 0, rcid, 2, 0); return; }  /* not supported */
    for (int i = 0; i < MAX_CHANS && !c; i++) if (!l->ch[i].used) c = &l->ch[i];
    if (!c) { conn_response(l, id, 0, rcid, 4, 0); return; }                                           /* no resources */
    memset(c, 0, sizeof *c);
    c->used = 1; c->psm = psm; c->rcid = rcid; c->rmtu = 672; c->lcid = 0x40 + (int)(c - l->ch) + 8 * (int)(l - links);
    if (psm != PSM_SDP && !l->enc) {                       /* authenticated + encrypted first (security mode 4) */
        c->pending = 1; c->pend_id = id;
        if (!l->pend_until) { l->auth_at = ms() + 1000; l->pend_until = ms() + 15000; }   /* the phone may be at it already */
        conn_response(l, id, c->lcid, rcid, 1, 1);           /* pending, authentication pending */
        return;
    }
    conn_response(l, id, c->lcid, rcid, 0, 0);
    config_request(l, c);
}

static void config_req_rx(struct link *l, unsigned id, const unsigned char *d, size_t n)
{
    if (n < 4) return;
    struct chan *c = chan_by(l, u16(d)); unsigned flags = u16(d + 2); unsigned char r[48]; size_t rn = 6;
    if (!c) { unsigned char e[6] = { 0x02, 0x00 }; put16(e + 2, u16(d)); put16(e + 4, 0); sig_send(l, 0x01, id, e, 6); return; }  /* invalid CID */
    put16(r, c->rcid); put16(r + 2, flags & 1); put16(r + 4, 0);
    for (size_t i = 4; i + 2 <= n && i + 2 + d[i + 1] <= n; i += 2 + d[i + 1]) {
        unsigned type = d[i] & 0x7f, len = d[i + 1];
        if (type == 0x01 && len >= 2) c->rmtu = u16(d + i + 2) < 48 ? 48 : u16(d + i + 2);    /* below the L2CAP minimum
                                                       (48) nothing fits: SDP answers and speaker packets assume more */
        else if (type == 0x04 && len >= 1 && d[i + 2] != 0) {                      /* not basic mode: offer basic */
            static const unsigned char basic[11] = { 0x04, 9 };
            put16(r + 4, 1); memcpy(r + 6, basic, 11); rn = 17; break;
        } else if (type != 0x02 && type != 0x03 && type != 0x05 && !(d[i] & 0x80) && rn + 2 + len <= sizeof r) {   /* unknown, no hint */
            put16(r + 4, 3); memcpy(r + rn, d + i, 2 + len); rn += 2 + len;
        }
    }
    sig_send(l, 0x05, id, r, rn);
    if (!u16(r + 4) && !(flags & 1) && !c->cfg_in) { c->cfg_in = 1; if (c->cfg_out) chan_opened(l, c); }
}

static void chan_free(struct link *l, struct chan *c)
{
    if (c->cfg_in && c->cfg_out) chan_closed(l, c);
    memset(c, 0, sizeof *c);
}

static void sig_rx(struct link *l, const unsigned char *p, size_t n)
{
    while (n >= 4) {
        unsigned code = p[0], id = p[1]; size_t len = u16(p + 2);
        if (len > n - 4) return;
        const unsigned char *d = p + 4; unsigned char r[8]; struct chan *c;
        switch (code) {
        case 0x02: conn_request(l, id, d, len); break;
        case 0x04: config_req_rx(l, id, d, len); break;
        case 0x05:                                          /* Configuration Response to ours */
            if (len >= 6 && (c = chan_by(l, u16(d)))) {
                if (u16(d + 4) == 0) { if (!c->cfg_out) { c->cfg_out = 1; if (c->cfg_in) chan_opened(l, c); } }
                else if (!c->cfg_out) {                     /* our MTU refused: take the default */
                    unsigned char q[4]; put16(q, c->rcid); put16(q + 2, 0);
                    if (++l->next_id > 255) l->next_id = 1;
                    sig_send(l, 0x04, l->next_id, q, 4);
                }
            }
            break;
        case 0x06:                                          /* Disconnection Request */
            if (len >= 4) {
                memcpy(r, d, 4); sig_send(l, 0x07, id, r, 4);
                if ((c = chan_by(l, u16(d)))) chan_free(l, c);
            }
            break;
        case 0x08: sig_send(l, 0x09, id, d, len < 48 ? len : 48); break;           /* Echo */
        case 0x0a:                                          /* Information Request */
            if (len >= 2) {
                unsigned char q[8]; unsigned t = u16(d); put16(q, t);
                if (t == 2) { put16(q + 2, 0); memset(q + 4, 0, 4); sig_send(l, 0x0b, id, q, 8); }     /* no extended features */
                else { put16(q + 2, 1); sig_send(l, 0x0b, id, q, 4); }                                  /* not supported */
            }
            break;
        case 0x03:                                          /* Connection Response to ours: dcid, scid, result */
            for (int i = 0; len >= 8 && i < MAX_CHANS; i++) {
                c = &l->ch[i];
                if (!c->used || c->out_id != (int)id || c->lcid != (int)u16(d + 2)) continue;
                if (u16(d + 4) == 1) break;                 /* pending: the final answer follows */
                c->out_id = 0;
                if (u16(d + 4) == 0) { c->rcid = u16(d); config_request(l, c); }
                else { fprintf(stderr, "a2dp: %012llx refused our channel (%u)\n", (unsigned long long)l->addr, u16(d + 4)); memset(c, 0, sizeof *c); }
            }
            break;
        case 0x01:                                          /* Command Reject: of our connection request, maybe */
            for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used && l->ch[i].out_id == (int)id) memset(&l->ch[i], 0, sizeof l->ch[i]);
            break;
        case 0x07: case 0x09: case 0x0b: break;             /* responses */
        default: r[0] = 0; r[1] = 0; sig_send(l, 0x01, id, r, 2); break;          /* Command Reject: not understood */
        }
        p += 4 + len; n -= 4 + len;
    }
}

int chan_open(struct link *l, int psm)                  /* a channel of ours: its CID, 0 = no room */
{
    struct chan *c = NULL;
    for (int i = 0; i < MAX_CHANS && !c; i++) if (!l->ch[i].used) c = &l->ch[i];
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    c->used = 1; c->psm = psm; c->rmtu = 672; c->lcid = 0x40 + (int)(c - l->ch) + 8 * (int)(l - links);
    if (++l->next_id > 255) l->next_id = 1;
    c->out_id = l->next_id;
    unsigned char d[4]; put16(d, psm); put16(d + 2, c->lcid);
    sig_send(l, 0x02, c->out_id, d, 4);
    return c->lcid;
}

/* "Connected to <name>" like stock Alexa, once the device has opened AVDTP (a speaker connection, not just an ACL link)
 * and its name is in.  Phones ask for AVDTP a second or two after connecting; the name takes some 100 ms.  A Pixel
 * "disconnecting" in its Bluetooth settings closes AVDTP and AVRCP but keeps the ACL link up: the announcement follows
 * AVDTP (chan_closed), not the link. */
static void maybe_tell(struct link *l)
{
    if (l->told || !l->av_sig || !l->named) return;
    l->told = 1;
    core_bt_device(l->name, 1);
}

static void chan_opened(struct link *l, struct chan *c)
{
    if (c->psm == PSM_AVCTP && !l->avctp) avrcp_opened(l, c);
    if (c->psm != PSM_AVDTP) return;
    if (!l->av_sig) {                                   /* the first AVDTP channel signals, the next one carries media */
        l->av_sig = c->lcid;
        out_sig_opened(l, c);
        if (!l->avctp) l->avctp_at = ms() + 2000;
        maybe_tell(l);
        return;
    }
    if (!out_media_opened(l, c)) sink_media_opened(l, c);
}

static void chan_closed(struct link *l, struct chan *c)
{
    if (c->lcid == l->avctp) { avrcp_closed(l); out_avctp_closed(l); }
    if (c->lcid == l->av_sig) {
        l->av_sig = 0; sink_closed(l);
        out_sig_closed(l);
        if (l->told) { l->told = 0; core_bt_device(l->name, 0); }  /* phones drop the profile, not always the link */
    }
    else if (!out_media_closed(l, c->lcid)) sink_media_closed(l, c->lcid);
}

static void l2_rx(struct link *l, const unsigned char *p, size_t n)
{
    int cid = u16(p + 2); struct chan *c;
    p += 4; n -= 4;
    if (cid == 0x0001) { sig_rx(l, p, n); return; }
    if (!(c = chan_by(l, cid)) || !c->cfg_in || !c->cfg_out) return;
    if (c->psm == PSM_SDP) sdp_rx(l, c, p, n);
    else if (cid == l->av_sig) { if (is_speaker(l)) out_rx(l, p, n); else av_rx(l, p, n); }
    else if (cid == l->avctp) avrcp_rx(l, p, n);
    else sink_media_rx(l, cid, p, n);
}

/* ---------------------------------------------------------------- hooks for ble.c */

static void link_gone(struct link *l)
{
    for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used) chan_free(l, &l->ch[i]);
    sink_closed(l);
    out_link_lost(l);
    acl_forget(&aq, l->handle, &l->tx);
    for (int i = 0; i < npend; i++)                         /* commands for this handle: it may be another link's soon */
        if ((pend[i].op == OP_DISCONNECT || pend[i].op == OP_ENCRYPT) && (int)u16(pend[i].p) == l->handle) pend[i].op = 0;
    memset(l, 0, sizeof *l);
}

int a2dp_acl(const unsigned char *p, size_t n)
{
    if (n < 4) return 0;
    struct link *l = link_by(u16(p) & 0x0fff); size_t len;
    if (!l) return 0;
    if ((len = l2cap_reassemble(&l->rxs, l->rx, sizeof l->rx, p, n))) l2_rx(l, l->rx, len);
    return 1;
}

int a2dp_completed(unsigned handle, unsigned n)
{
    struct link *l = link_by(handle);
    if (!l) return 0;
    acl_completed(&aq, &l->tx, n);
    acl_flush(&aq);
    return 1;
}

void a2dp_acl_flush(void) { acl_flush(&aq); }

int a2dp_event(const unsigned char *e, size_t n)
{
    if (n < 2) return 0;
    const unsigned char *q = e + 2; size_t qn = n - 2; struct link *l;
    int pairing = atomic_load(&pairing_on);
    switch (e[0]) {
    case EV_INQUIRY_COMPLETE: out_inquiry_done(); return 1;
    case EV_INQUIRY_RSSI: case EV_INQUIRY_EXT:              /* one response each: count, address, ... */
        if (qn >= 15 && q[0]) out_found(q + 1, qn - 1, e[0] == EV_INQUIRY_EXT);
        return 1;
    case EV_CONN_REQUEST:                                   /* address, class of device, link type */
        if (qn < 10) return 1;
        {   uint64_t a = addr_of(q); int free_slot = 0;
            for (int i = 0; i < MAX_LINKS; i++) free_slot |= !links[i].used;
            if (q[9] == 1 && free_slot && (pairing || key_for(a))) { unsigned char role = 0x01; later_addr(OP_ACCEPT, q, &role, 1); }
            else {
                unsigned char why = q[9] != 1 || !free_slot ? 0x0d : 0x0f;        /* limited resources / unacceptable address */
                fprintf(stderr, "a2dp: connection from %012llx refused\n", (unsigned long long)a);
                later_addr(OP_REJECT, q, &why, 1);
            }
        }
        return 1;
    case EV_CONN_COMPLETE: {                                /* status, handle, address, link type, encryption */
        if (qn < 11) return 1;
        int paged = out_page_complete(addr_of(q + 3), q[0]);   /* 1: we paged the speaker; -1: that failed */
        if (paged < 0) return 1;
        if (q[0] || q[9] != 1) return 1;
        for (int i = 0; i < MAX_LINKS; i++) if (!links[i].used) {
            l = &links[i]; memset(l, 0, sizeof *l);
            l->used = 1; l->handle = u16(q + 1) & 0x0fff; l->addr = addr_of(q + 3); l->enc = q[10];
            fprintf(stderr, "a2dp: %012llx connected\n", (unsigned long long)l->addr);
            { unsigned char r[4] = { 0x01, 0, 0, 0 }; later_addr(OP_REMOTE_NAME, q + 3, r, 4); }   /* page scan R1, clock offset unknown */
            if (is_speaker(l)) out_link_up(l, paged);
            return 1;
        }
        { unsigned char d[3]; put16(d, u16(q + 1)); d[2] = 0x14; later(OP_DISCONNECT, d, 3); }  /* no room after all */
        return 1; }
    case EV_DISCONNECT:
        if (qn < 4 || q[0] || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        fprintf(stderr, "a2dp: %012llx disconnected (0x%02x)\n", (unsigned long long)l->addr, q[3]);
        link_gone(l);
        return 1;
    case EV_REMOTE_NAME: {                                  /* status, address, name (UTF-8, NUL padded to 248) */
        if (qn < 7) return 1;
        uint64_t a = addr_of(q + 1); l = NULL;
        for (int i = 0; i < MAX_LINKS; i++) if (links[i].used && links[i].addr == a) l = &links[i];
        if (!l) return 0;                                   /* not ours (ble.c never asks, but let it see) */
        size_t nl = 0, max = qn - 7 < sizeof l->name - 1 ? qn - 7 : sizeof l->name - 1;
        if (!q[0]) while (nl < max && q[7 + nl]) nl++;
        if (nl == max && 7 + nl < qn) while (nl && (q[7 + nl] & 0xc0) == 0x80) nl--;      /* cut: not inside a UTF-8 character */
        memcpy(l->name, q + 7, nl); l->name[nl] = 0;
        for (size_t i = 0; i < nl; i++) if ((unsigned char)l->name[i] < 0x20) l->name[i] = ' ';
        if (nl) out_named(l);
        if (q[0]) fprintf(stderr, "a2dp: %012llx: no name (0x%02x)\n", (unsigned long long)a, q[0]);
        else fprintf(stderr, "a2dp: %012llx is \"%s\"\n", (unsigned long long)a, l->name);
        l->named = 1;
        maybe_tell(l);
        return 1; }
    case EV_LINK_KEY_REQUEST: {
        if (qn < 6) return 1;
        struct key *k = key_for(addr_of(q));
        if (k) later_addr(OP_LINK_KEY_REPLY, q, k->k, 16); else later_addr(OP_LINK_KEY_NEG, q, NULL, 0);
        return 1; }
    case EV_LINK_KEY_NOTIFY: {                              /* address, key, type */
        if (qn < 23) return 1;
        uint64_t a = addr_of(q); struct key *k = key_for(a);
        if (!k) { if (nkeys == MAX_KEYS) key_forget(keys[0].addr); k = &keys[nkeys++]; }
        k->addr = a; memcpy(k->k, q + 6, 16); k->type = q[22];
        keys_save();
        fprintf(stderr, "a2dp: paired with %012llx (key type %u)\n", (unsigned long long)a, q[22]);
        if (!out_paired(a) && pairing) set_pairing(0);      /* like the stock speaker: one device per "pair" */
        return 1; }
    case EV_IO_CAP_REQUEST:
        if (qn < 6) return 1;
        if (pairing || out_may_pair(addr_of(q))) { unsigned char io[3] = { 0x03, 0x00, 0x04 }; later_addr(OP_IO_CAP_REPLY, q, io, 3); }   /* NoInputNoOutput, no OOB, general bonding */
        else { unsigned char why = 0x18; later_addr(OP_IO_CAP_NEG, q, &why, 1);              /* pairing not allowed */
               fprintf(stderr, "a2dp: %012llx wants to pair, pairing is off\n", (unsigned long long)addr_of(q)); }
        return 1;
    case EV_USER_CONFIRM:
        if (qn < 6) return 1;
        later_addr(pairing || out_may_pair(addr_of(q)) ? OP_CONFIRM_REPLY : OP_CONFIRM_NEG, q, NULL, 0);
        return 1;
    case EV_PIN_REQUEST:
        if (qn < 6) return 1;
        if (pairing || out_may_pair(addr_of(q))) { unsigned char pin[17] = { 4, '0', '0', '0', '0' }; later_addr(OP_PIN_REPLY, q, pin, 17); }
        else later_addr(OP_PIN_NEG, q, NULL, 0);
        return 1;
    case EV_IO_CAP_RESPONSE: return 1;
    case EV_SSP_COMPLETE:
        if (qn >= 7 && q[0]) fprintf(stderr, "a2dp: pairing with %012llx failed (0x%02x)\n", (unsigned long long)addr_of(q + 1), q[0]);
        return 1;
    case EV_AUTH_COMPLETE:                                  /* status, handle */
        if (qn < 3 || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        if (q[0]) {
            fprintf(stderr, "a2dp: %012llx: authentication failed (0x%02x)\n", (unsigned long long)l->addr, q[0]);
            /* Key missing (0x06; BlueZ says 0x05): the device forgot us, or it is another one under its address.  The
             * key stays: deleting it on the peer's word let a lookalike pair in its place.  A phone pairs again in the
             * pairing window (its new key replaces this one), the speaker through "Bluetooth speaker search".  Within
             * the window, where anyone may pair anyway, the key goes and we authenticate again: that pairs. */
            if ((q[0] == 0x06 || q[0] == 0x05) && key_for(l->addr)) {
                if (pairing && !is_speaker(l) && l->pend_until) {
                    key_forget(l->addr); l->auth_sent = 0; l->auth_at = ms() + 100;
                    return 1;
                }
                fprintf(stderr, "a2dp: %012llx has no key for us: %s\n", (unsigned long long)l->addr,
                        is_speaker(l) ? "search for the speaker again to pair it" : "pair it again with pairing on");
            }
            pending_resolve(l, 0);
        } else if (!l->enc) { unsigned char d[3]; put16(d, l->handle); d[2] = 1; later(OP_ENCRYPT, d, 3); }
        else pending_resolve(l, 1);
        return 1;
    case EV_ENC_CHANGE:                                     /* status, handle, on */
        if (qn < 4 || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        l->enc = !q[0] && q[3];
        if (q[0] || l->enc) pending_resolve(l, l->enc);
        return 1;
    case EV_KEY_REFRESH:
        return qn >= 3 && link_by(u16(q + 1) & 0x0fff);
    }
    return 0;
}

int a2dp_upkeep(void)
{
    avrcp_upkeep();
    if (out_upkeep() < 0) return -1;
    /* hci_cmd handles events while it waits: they may queue more (npend grows, run here too) or end a link, whose
     * commands are then voided (link_gone: op 0) rather than sent to a handle the controller may hand out again */
    for (int i = 0; i < npend; i++) if (pend[i].op && hci_cmd(pend[i].op, pend[i].p, pend[i].n) < 0) { npend = 0; return -1; }
    npend = 0;
    int r = atomic_exchange(&pair_req, -1);
    if (r >= 0) set_pairing(r);
    if (pair_until && ms() > pair_until) set_pairing(0);
    for (int i = 0; i < MAX_LINKS; i++) {
        struct link *l = &links[i];
        if (!l->used || !l->pend_until) continue;
        if (ms() > l->pend_until) { fprintf(stderr, "a2dp: %012llx: link never encrypted\n", (unsigned long long)l->addr); pending_resolve(l, 0); continue; }
        if (l->auth_sent || ms() < l->auth_at) continue;
        unsigned char d[2]; put16(d, l->handle); l->auth_sent = 1;
        if (hci_cmd(OP_AUTH, d, 2) < 0) return -1;        /* refused (the phone is at it): its result comes anyway */
    }
    int mode = atomic_load(&pairing_on) ? 3 : nkeys ? 2 : 0;
    if (mode != scan_mode) {
        unsigned char m = mode;
        int st = hci_cmd(OP_SCAN_ENABLE, &m, 1);
        if (st < 0) return -1;
        if (!st) scan_mode = mode;
        else { fprintf(stderr, "a2dp: scan mode refused (0x%02x)\n", st); scan_mode = mode; }
    }
    return 0;
}

int a2dp_streaming(void) { return sink_streaming() || out_streaming(); }

int a2dp_busy(void)
{
    int b = pair_until != 0 || npend > 0 || out_busy();
    for (int i = 0; i < MAX_LINKS; i++) b |= links[i].used && (links[i].pend_until || links[i].avctp_at);
    return b;
}

void a2dp_lost(void)
{
    for (int i = 0; i < MAX_LINKS; i++) if (links[i].used) link_gone(&links[i]);
    acl_clear(&aq); npend = 0; scan_mode = -1;
    out_lost();
}

int a2dp_setup(void)
{
    unsigned char b[248]; const char *name = core_name; size_t nl = strlen(name) < 232 ? strlen(name) : 232;
    unsigned acl_len = aq.len, acl_num = aq.num;           /* the last ones if the command does not answer */
    a2dp_lost();
    if (hci_cmd(OP_READ_BUFFER, NULL, 0) == 0) { const unsigned char *r = hci_ret(); acl_len = u16(r); acl_num = u16(r + 3); }
    if (acl_len < 27) acl_len = 27;
    if (acl_len > 1021) acl_len = 1021;
    if (!acl_num) acl_num = 1;
    acl_buffers(&aq, acl_len, acl_num, hci_acl_pool());   /* shared: ble.c has filled the pool */
    memset(b, 0, sizeof b); memcpy(b, name, nl);
    if (hci_cmd(OP_LOCAL_NAME, b, 248) < 0) return -1;
    { unsigned char cod[3] = { 0x14, 0x04, 0x24 };          /* audio + rendering; audio/video, loudspeaker */
      unsigned char one = 1, two = 2, pol[2] = { 0x05, 0x00 };   /* link policy: role switch, sniff */
      if (hci_cmd(OP_CLASS, cod, 3) < 0 || hci_cmd(OP_SSP_MODE, &one, 1) < 0 || hci_cmd(OP_INQUIRY_MODE, &two, 1) < 0 ||
          hci_cmd(OP_LINK_POLICY, pol, 2) < 0) return -1; }
    /* extended inquiry response: the name and the A2DP sink UUID, so phones show a speaker before they connect */
    memset(b, 0, 241); b[1] = nl + 1; b[2] = 0x09; memcpy(b + 3, name, nl);
    { unsigned char *u = b + 3 + nl; u[0] = 3; u[1] = 0x03; u[2] = 0x0b; u[3] = 0x11; }
    b[0] = 0;                                               /* FEC not required */
    if (hci_cmd(OP_EIR, b, 241) < 0) return -1;
    fprintf(stderr, "a2dp: ready as \"%s\", ACL %u x %u%s\n", name, acl_num, acl_len, aq.credits != &aq.own ? ", shared with LE" : "");
    return 0;
}

/* ---------------------------------------------------------------- API (a2dp.h) */

void a2dp_pair(int on) { atomic_store(&pair_req, on != 0); hci_poke(); }
int  a2dp_pairing(void) { return atomic_load(&pairing_on); }

void a2dp_start(void (*changed)(void))
{
    if (changed) notify = changed;
    if (started || !ble_present()) return;
    started = 1;
    volume_init();
    keys_load();
    out_start();
    sink_start();
    avrcp_start();
    ble_start(NULL);
}
