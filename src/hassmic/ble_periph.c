/*
 * The LE peripheral role beside ble.c's central (ble.h, struct ble_peripheral): legacy connectable advertising, one
 * central connected to us at a time, its ATT traffic to the owner's GATT server.  Only while the owner asks for it
 * (Improv Wi-Fi: a few minutes when the Echo has no Wi-Fi or the action button was held).
 *
 * The controller decides what it can do at once (LE states).  Advertising while scanning is tried first; a controller
 * that refuses it (Command Disallowed) gets scanning paused while it advertises, which the proxy then reports as its
 * scanner idle.  Advertising pauses while ble.c sets up a connection of its own, as scanning does, so that the
 * controller is never asked to initiate and advertise at once.  Advertising stops by itself once a central connects
 * (legacy undirected advertising does); a second one meanwhile is dropped.
 *
 * Nothing on this link needs security: pairing requests are refused (SMP "pairing not supported"), an LTK request
 * answered negatively, L2CAP signalling requests rejected.  Controller thread only, like everything in hci.h.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "acl.h"
#include "ble.h"
#include "gatts.h"
#include "hci.h"

#define ADV_INTERVAL 160                /* 100 ms in 0.625 ms units: found within a second or two by phones */
#define RETRY_MS 5000                   /* after the controller refused to advertise */

enum { EV_DISCONNECT = 0x05, EV_LE_META = 0x3e, LE_CONN_COMPLETE = 0x01, LE_LTK_REQUEST = 0x05 };
enum { OP_DISCONNECT = 0x0406, OP_ADV_PARAMS = 0x2006, OP_ADV_DATA = 0x2008, OP_SCAN_RSP = 0x2009, OP_ADV_ENABLE = 0x200a,
       OP_LTK_NEG = 0x201b };
enum { CID_ATT = 4, CID_SIG = 5, CID_SMP = 6 };
enum { E_DISALLOWED = 0x0c, E_USER_ENDED = 0x13 };

static const struct ble_peripheral *_Atomic P;
static struct {
    int up, handle, ltk_neg, close_now, closing; uint64_t addr; long long t;
    struct acl_tx tx; unsigned char rx[4 + GATTS_MTU + 64]; struct l2cap_rx rxs;
} pl;
static int adv_on, adv_params, wanted, excl, last_refusal;
static int drop = -1, dropped = -1;     /* a second central: to be disconnected, disconnection asked for */
static long long retry_at;
static uint8_t adv[31], rsp[31]; static size_t adv_len = (size_t)-1, rsp_len = (size_t)-1;

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

void ble_peripheral(const struct ble_peripheral *p) { atomic_store(&P, p); hci_poke(); }
void ble_peripheral_poke(void) { hci_poke(); }

static void gone(unsigned reason)
{
    const struct ble_peripheral *h = P;
    fprintf(stderr, "bluetooth: central %012llx disconnected (0x%02x)\n", (unsigned long long)pl.addr, reason);
    le_forget(pl.handle, &pl.tx);
    uint64_t a = pl.addr;
    memset(&pl, 0, sizeof pl);
    if (h && h->connected) h->connected(a, 0);
}

static void connected(const unsigned char *q)     /* LE Connection Complete after the subevent code, we are peripheral */
{
    const struct ble_peripheral *h = P; uint64_t a = 0; int handle = u16(q + 1) & 0x0fff;
    adv_on = wanted = 0;                            /* the controller stopped advertising for it: the scan may go on */
    if (pl.up) { drop = handle; return; }           /* one at a time */
    for (int b = 5; b >= 0; b--) a = a << 8 | q[5 + b];
    memset(&pl, 0, sizeof pl);
    pl.up = 1; pl.handle = handle; pl.addr = a;
    fprintf(stderr, "bluetooth: central %012llx connected to us\n", (unsigned long long)a);
    if (h && h->connected) h->connected(a, 1);
    if (!h) pl.close_now = 1;
}

int periph_event(const unsigned char *p, size_t n)
{
    const unsigned char *q = p + 2;
    if (n < 2) return 0;
    n -= 2;
    if (p[0] == EV_DISCONNECT && n >= 4 && !q[0]) {
        int h = u16(q + 1) & 0x0fff;
        if (h == drop || h == dropped) { if (h == drop) drop = -1; else dropped = -1; return 1; }
        if (pl.up && h == pl.handle) { gone(q[3]); return 1; }
    } else if (p[0] == EV_LE_META && n >= 19 && q[0] == LE_CONN_COMPLETE && !q[1] && q[4] == 1) {
        connected(q + 1); return 1;
    } else if (p[0] == EV_LE_META && n >= 3 && q[0] == LE_LTK_REQUEST && pl.up && (int)(u16(q + 1) & 0x0fff) == pl.handle) {
        pl.ltk_neg = 1; return 1;
    }
    return 0;
}

struct acl_tx *periph_tx(int handle) { return pl.up && handle == pl.handle ? &pl.tx : NULL; }

static void send_notifications(void)
{
    const struct ble_peripheral *h = P; uint8_t b[GATTS_MTU + 8]; size_t k;
    for (int i = 0; h && h->notify && pl.up && i < 8 && (k = h->notify(b, sizeof b)); i++) le_send(pl.handle, &pl.tx, CID_ATT, b, k);
}

int periph_acl(const unsigned char *p, size_t n)
{
    if (n < 4 || !pl.up || (int)(u16(p) & 0x0fff) != pl.handle) return 0;
    size_t len = l2cap_reassemble(&pl.rxs, pl.rx, sizeof pl.rx, p, n);
    if (!len) return 1;
    const struct ble_peripheral *h = P; const unsigned char *d = pl.rx + 4; size_t dl = u16(pl.rx); unsigned char r[GATTS_MTU + 8];
    switch (u16(pl.rx + 2)) {
    case CID_ATT:
        if (h && h->att && dl) { size_t k = h->att(d, dl, r, sizeof r); if (k) le_send(pl.handle, &pl.tx, CID_ATT, r, k); }
        send_notifications();                       /* what the request changed goes out right behind its answer */
        break;
    case CID_SIG:                                   /* a request (not a response or a credit): Command Reject */
        if (dl >= 4 && d[0] != 0x01 && d[0] != 0x13 && d[0] != 0x15 && d[0] != 0x16) {
            r[0] = 0x01; r[1] = d[1]; put16(r + 2, 2); put16(r + 4, 0);
            le_send(pl.handle, &pl.tx, CID_SIG, r, 6);
        }
        break;
    case CID_SMP:                                   /* Pairing Request: pairing not supported */
        if (dl && d[0] == 0x01) { r[0] = 0x05; r[1] = 0x05; le_send(pl.handle, &pl.tx, CID_SMP, r, 2); }
        break;
    }
    return 1;
}

static int set_data(unsigned op, const uint8_t *d, size_t n)
{
    uint8_t b[32] = { n };
    memcpy(b + 1, d, n);
    return hci_cmd(op, b, 32);
}

static void refused(const char *what, int st)
{
    if (st == last_refusal) return;                 /* once, not every retry */
    last_refusal = st;
    fprintf(stderr, "bluetooth: %s refused, HCI status 0x%02x\n", what, st);
}

/* Advertising as wanted: -1 = controller gone */
static int advertising(int want)
{
    int st;
    if (adv_on && !want) {
        uint8_t off = 0;
        if ((st = hci_cmd(OP_ADV_ENABLE, &off, 1)) < 0) return -1;
        adv_on = 0;
        fprintf(stderr, "bluetooth: advertising off\n");
    }
    if (!want || ms() < retry_at) return 0;
    if (!adv_params) {
        uint8_t p[15] = { ADV_INTERVAL & 0xff, ADV_INTERVAL >> 8, ADV_INTERVAL & 0xff, ADV_INTERVAL >> 8, 0x00 /* ADV_IND */,
                          0x00 /* public */, 0, 0, 0, 0, 0, 0, 0, 0x07 /* all channels */, 0x00 /* no filter */ };
        if ((st = hci_cmd(OP_ADV_PARAMS, p, 15)) < 0) return -1;
        if (st) { refused("advertising parameters", st); retry_at = ms() + RETRY_MS; return 0; }
        adv_params = 1;
    }
    const struct ble_peripheral *h = P; uint8_t a[31], r[31]; size_t al = 0, rl = 0;
    if (!h || !h->advertise(a, &al, r, &rl)) return 0;
    if (al > 31) al = 31;
    if (rl > 31) rl = 31;
    if (al != adv_len || memcmp(a, adv, al)) {
        if ((st = set_data(OP_ADV_DATA, a, al)) < 0) return -1;
        if (!st) { memcpy(adv, a, al); adv_len = al; }
    }
    if (rl != rsp_len || memcmp(r, rsp, rl)) {
        if ((st = set_data(OP_SCAN_RSP, r, rl)) < 0) return -1;
        if (!st) { memcpy(rsp, r, rl); rsp_len = rl; }
    }
    if (adv_on || pl.up) return 0;                  /* a central came in while the data went out */
    uint8_t on = 1;
    if ((st = hci_cmd(OP_ADV_ENABLE, &on, 1)) < 0) return -1;
    if (!st) { adv_on = 1; last_refusal = 0; fprintf(stderr, "bluetooth: advertising%s\n", excl ? " (scanning paused for it)" : ""); return 0; }
    if (st == E_DISALLOWED && !excl) {              /* not while scanning, on this controller: the scan makes room */
        excl = 1;
        fprintf(stderr, "bluetooth: the controller cannot scan and advertise at once: scanning pauses while it advertises\n");
        return 0;
    }
    refused("advertising", st); retry_at = ms() + RETRY_MS;
    return 0;
}

int periph_upkeep(int central_busy)
{
    const struct ble_peripheral *h = P; uint8_t a[31], r[31]; size_t al, rl; unsigned char p[3];
    int want = h && h->advertise(a, &al, r, &rl);
    if (pl.up && !want && !pl.closing) pl.close_now = 1;
    if (pl.ltk_neg && pl.up) {
        pl.ltk_neg = 0; put16(p, pl.handle);
        if (hci_cmd(OP_LTK_NEG, p, 2) < 0) return -1;
    }
    if (drop >= 0) {
        put16(p, drop); p[2] = E_USER_ENDED;
        dropped = drop; drop = -1;
        if (hci_cmd(OP_DISCONNECT, p, 3) < 0) return -1;
    }
    if (pl.up && pl.close_now && !pl.closing) {
        pl.close_now = 0; pl.closing = 1; pl.t = ms();
        put16(p, pl.handle); p[2] = E_USER_ENDED;
        if (hci_cmd(OP_DISCONNECT, p, 3) < 0) return -1;
    }
    if (pl.up && pl.closing && ms() - pl.t > 5000) gone(0x16);     /* never confirmed: forget it */
    wanted = want && !pl.up && !central_busy;
    if (advertising(wanted) < 0) return -1;
    if (pl.up) send_notifications();
    return 0;
}

int periph_scan_pause(void) { return excl && wanted; }
int periph_busy(void) { return pl.closing || pl.close_now || (wanted && !adv_on); }

void periph_lost(void)
{
    const struct ble_peripheral *h = P; uint64_t a = pl.addr; int was = pl.up;
    memset(&pl, 0, sizeof pl);
    adv_on = adv_params = wanted = excl = last_refusal = 0; drop = dropped = -1; retry_at = 0;
    adv_len = rsp_len = (size_t)-1;
    if (was && h && h->connected) h->connected(a, 0);
}
