/*
 * SDP server of the Bluetooth speaker (bt_int.h): the records a phone or a speaker looks for before it opens AVDTP or
 * AVCTP.  A2DP sink 1.3 (AVDTP 1.3), speaker; AVRCP 1.5 target (category 2: absolute volume) and controller (categories
 * 1 and 2: play / pause a phone, a speaker's volume); A2DP source 1.3 for playing to a speaker.  Requests come from any
 * device that connects, paired or not: every length in them is checked against what arrived.
 */
#include <string.h>
#include "bt_int.h"

struct attr { unsigned id; const unsigned char *v; unsigned n; };
#define A(id, ...) { id, (const unsigned char[]){ __VA_ARGS__ }, sizeof (const unsigned char[]){ __VA_ARGS__ } }
static const unsigned char a_browse[] = { 0x35, 0x03, 0x19, 0x10, 0x02 },                  /* PublicBrowseRoot */
    a_avctp[] = { 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,               /* L2CAP, PSM AVCTP */
                  0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04 },                           /* AVCTP 1.4 */
    a_avrcp[] = { 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x09, 0x01, 0x05 };             /* AVRCP 1.5 */
static const struct attr r_sink[] = {                  /* A2DP sink 1.3, speaker */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x01), A(0x0001, 0x35, 0x03, 0x19, 0x11, 0x0b),
    A(0x0004, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19, 0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03),
    { 0x0005, a_browse, sizeof a_browse }, A(0x0006, 0x35, 0x09, 0x09, 0x65, 0x6e, 0x09, 0x00, 0x6a, 0x09, 0x01, 0x00),
    A(0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03),
    A(0x0100, 0x25, 0x0a, 'A', 'u', 'd', 'i', 'o', ' ', 'S', 'i', 'n', 'k'), A(0x0311, 0x09, 0x00, 0x02),
}, r_target[] = {                                       /* AVRCP target, category 2 (amplifier): absolute volume */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x02), A(0x0001, 0x35, 0x03, 0x19, 0x11, 0x0c), { 0x0004, a_avctp, sizeof a_avctp },
    { 0x0005, a_browse, sizeof a_browse }, { 0x0009, a_avrcp, sizeof a_avrcp }, A(0x0311, 0x09, 0x00, 0x02),
}, r_control[] = {                                      /* AVRCP controller, category 1: play / pause the phone; 2:
                                                           the speaker's absolute volume (BlueZ refuses the event without) */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x03), A(0x0001, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x19, 0x11, 0x0f),
    { 0x0004, a_avctp, sizeof a_avctp }, { 0x0005, a_browse, sizeof a_browse }, { 0x0009, a_avrcp, sizeof a_avrcp },
    A(0x0311, 0x09, 0x00, 0x03),
}, r_source[] = {                                       /* A2DP source 1.3, player: playing to a speaker (some check) */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x04), A(0x0001, 0x35, 0x03, 0x19, 0x11, 0x0a),
    A(0x0004, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19, 0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03),
    { 0x0005, a_browse, sizeof a_browse }, A(0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03),
    A(0x0100, 0x25, 0x0c, 'A', 'u', 'd', 'i', 'o', ' ', 'S', 'o', 'u', 'r', 'c', 'e'), A(0x0311, 0x09, 0x00, 0x01),
};
#undef A
static const struct { unsigned handle; const struct attr *a; int n; unsigned uuids[6]; } records[] = {
    { 0x00010001, r_sink, sizeof r_sink / sizeof *r_sink, { 0x110b, 0x0100, 0x0019, 0x1002, 0x110d } },
    { 0x00010002, r_target, sizeof r_target / sizeof *r_target, { 0x110c, 0x0100, 0x0017, 0x1002, 0x110e } },
    { 0x00010003, r_control, sizeof r_control / sizeof *r_control, { 0x110e, 0x110f, 0x0100, 0x0017, 0x1002 } },
    { 0x00010004, r_source, sizeof r_source / sizeof *r_source, { 0x110a, 0x0100, 0x0019, 0x1002, 0x110d } },
};
#define NREC (int)(sizeof records / sizeof *records)

/* data element header: type, length of the data; returns the header's length, 0 if malformed */
static size_t de(const unsigned char *p, size_t n, unsigned *type, size_t *len)
{
    if (!n) return 0;
    unsigned s = p[0] & 7; *type = p[0] >> 3;
    if (*type == 0) { *len = 0; return 1; }
    if (s < 5) { *len = 1u << s; return *len <= n - 1 ? 1 : 0; }
    size_t h = s == 5 ? 2 : s == 6 ? 3 : 5;
    if (n < h) return 0;
    *len = s == 5 ? p[1] : s == 6 ? (size_t)(p[1] << 8 | p[2]) : (size_t)p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
    return *len <= n - h ? h : 0;                       /* not h + *len <= n: a 32-bit length wraps that on the Echo */
}

/* a UUID element as a 16-bit value; -1 if it is not one of the Bluetooth base UUIDs */
static long uuid16(const unsigned char *p, size_t len)
{
    static const unsigned char base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
    if (len == 2) return p[0] << 8 | p[1];
    if (len == 4) return p[0] || p[1] ? -1 : p[2] << 8 | p[3];
    if (len == 16) return p[0] || p[1] || memcmp(p + 4, base, 12) ? -1 : p[2] << 8 | p[3];
    return -1;
}

/* the service search pattern: a bit per record that holds every UUID asked for; -1 = malformed */
static int sdp_match(const unsigned char **pp, size_t *np)
{
    unsigned t; size_t len, h = de(*pp, *np, &t, &len), i;
    if (!h || t != 6) return -1;
    const unsigned char *p = *pp + h; int all = (1 << NREC) - 1;
    for (i = 0; i < len; ) {
        unsigned ut; size_t ul, uh = de(p + i, len - i, &ut, &ul);
        if (!uh || ut != 3) return -1;
        long u = uuid16(p + i + uh, ul);
        for (int r = 0; r < NREC; r++) {
            int found = 0;
            for (int k = 0; k < 6; k++) found |= records[r].uuids[k] && u == (long)records[r].uuids[k];
            if (!found) all &= ~(1 << r);
        }
        i += uh + ul;
    }
    *pp += h + len; *np -= h + len;
    return all;
}

/* sequence header of n bytes of content at o: 2 or 3 bytes */
static size_t seq_head(unsigned char *o, size_t n)
{
    if (n < 256) { o[0] = 0x35; o[1] = n; return 2; }
    o[0] = 0x36; o[1] = n >> 8; o[2] = n; return 3;
}

/* record r's attributes the ID list (at p, len bytes, checked) asks for, as a data element sequence; returns its length */
static size_t sdp_record(int r, const unsigned char *p, size_t len, unsigned char *out)
{
    unsigned char body[400]; size_t o = 0;
    for (int k = 0; k < records[r].n; k++) {
        const struct attr *a = &records[r].a[k]; int want = 0;
        for (size_t i = 0; i < len; ) {
            unsigned it; size_t il, ih = de(p + i, len - i, &it, &il);
            const unsigned char *v = p + i + ih;
            if (il == 2) want |= a->id == (unsigned)(v[0] << 8 | v[1]);
            else want |= a->id >= (unsigned)(v[0] << 8 | v[1]) && a->id <= (unsigned)(v[2] << 8 | v[3]);
            i += ih + il;
        }
        if (!want || o + 3 + a->n > sizeof body) continue;
        body[o] = 0x09; body[o + 1] = a->id >> 8; body[o + 2] = a->id; memcpy(body + o + 3, a->v, a->n);
        o += 3 + a->n;
    }
    size_t h = seq_head(out, o); memcpy(out + h, body, o);
    return h + o;
}

/* the attribute ID list: checked, *p / *len its content.  0 = malformed */
static int sdp_idlist(const unsigned char **pp, size_t *np, const unsigned char **p, size_t *len)
{
    unsigned t; size_t h = de(*pp, *np, &t, len);
    if (!h || t != 6) return 0;
    *p = *pp + h;
    for (size_t i = 0; i < *len; ) {
        unsigned it; size_t il, ih = de(*p + i, *len - i, &it, &il);
        if (!ih || it != 1 || (il != 2 && il != 4)) return 0;
        i += ih + il;
    }
    *pp += h + *len; *np -= h + *len;
    return 1;
}

static void sdp_error(struct link *l, struct chan *c, const unsigned char *tid, unsigned code)
{
    unsigned char r[7] = { 0x01, tid[0], tid[1], 0, 2, code >> 8, code };
    l2_send(l, c->rcid, r, 7);
}

void sdp_rx(struct link *l, struct chan *c, const unsigned char *p, size_t n)
{
    unsigned char full[1200], r[1024]; size_t fn = 0, rn;
    if (n < 5) return;
    unsigned pdu = p[0]; const unsigned char *tid = p + 1, *q = p + 5, *ids; size_t qn = (size_t)(p[3] << 8 | p[4]), idn;
    if (qn > n - 5) { sdp_error(l, c, tid, 0x0004); return; }                                          /* invalid PDU size */
    if (pdu == 0x02) {                                      /* ServiceSearch: the matching record handles */
        int m = sdp_match(&q, &qn);
        if (m < 0 || qn < 2) { sdp_error(l, c, tid, 0x0003); return; }
        unsigned max = q[0] << 8 | q[1], cnt = 0;
        rn = 9;
        for (int i = 0; i < NREC; i++) if (m >> i & 1 && cnt < max) {
            unsigned h = records[i].handle; r[rn++] = h >> 24; r[rn++] = h >> 16; r[rn++] = h >> 8; r[rn++] = h; cnt++;
        }
        r[rn++] = 0;                                        /* no continuation */
        r[0] = 0x03; r[1] = tid[0]; r[2] = tid[1]; r[3] = (rn - 5) >> 8; r[4] = rn - 5;
        r[5] = 0; r[6] = cnt; r[7] = 0; r[8] = cnt;
        l2_send(l, c->rcid, r, rn);
        return;
    }
    if (pdu != 0x04 && pdu != 0x06) { sdp_error(l, c, tid, 0x0003); return; }
    int m = 0;
    if (pdu == 0x04) {                                      /* ServiceAttribute: handle, max, IDs, continuation */
        if (qn < 4) { sdp_error(l, c, tid, 0x0003); return; }
        unsigned h = (unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
        for (int i = 0; i < NREC; i++) if (records[i].handle == h) m = 1 << i;
        if (!m) { sdp_error(l, c, tid, 0x0002); return; }
        q += 4; qn -= 4;
    } else if ((m = sdp_match(&q, &qn)) < 0) { sdp_error(l, c, tid, 0x0003); return; }
    if (qn < 2) { sdp_error(l, c, tid, 0x0003); return; }
    unsigned max = q[0] << 8 | q[1]; q += 2; qn -= 2;
    if (!sdp_idlist(&q, &qn, &ids, &idn) || qn < 1) { sdp_error(l, c, tid, 0x0003); return; }
    if (pdu == 0x04) fn = sdp_record(__builtin_ctz(m), ids, idn, full);
    else {                                                  /* a sequence of records */
        unsigned char recs[1100]; size_t rl = 0;
        for (int i = 0; i < NREC; i++) if (m >> i & 1) rl += sdp_record(i, ids, idn, recs + rl);
        fn = seq_head(full, rl); memcpy(full + fn, recs, rl); fn += rl;
    }
    size_t off = 0;
    if (q[0] == 2 && qn >= 3) off = q[1] << 8 | q[2];       /* our continuation state: the offset reached */
    if (off > fn) { sdp_error(l, c, tid, 0x0005); return; }  /* invalid continuation state */
    size_t k = fn - off, lim = c->rmtu > 16 ? c->rmtu - 10 : 16;
    if (max < 7) max = 7;
    if (k > max) k = max;
    if (k > lim) k = lim;
    if (k > sizeof r - 10) k = sizeof r - 10;
    rn = 0; r[rn++] = pdu + 1; r[rn++] = tid[0]; r[rn++] = tid[1]; rn += 2;
    r[rn++] = k >> 8; r[rn++] = k; memcpy(r + rn, full + off, k); rn += k;
    if (off + k < fn) { r[rn++] = 2; r[rn++] = (off + k) >> 8; r[rn++] = off + k; } else r[rn++] = 0;
    r[3] = (rn - 5) >> 8; r[4] = rn - 5;
    l2_send(l, c->rcid, r, rn);
}
