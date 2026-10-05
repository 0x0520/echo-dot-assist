/* Minimal GATT server, see gatts.h.  ATT as Bluetooth Core 5.3 Vol 3 Part F has it, the requests a central uses to
 * discover and use a small database: primary services (by group type and by UUID), characteristics, descriptors, reads
 * (long ones too), writes (long ones through one queued attribute) and notification subscriptions.  Anything else is
 * answered "request not supported", as the spec wants for requests; commands get no answer. */
#include "gatts.h"
#include <string.h>

enum { A_SERVICE = 1, A_CHAR, A_VALUE, A_CCC };
enum { ERROR_RSP = 0x01, MTU_REQ, MTU_RSP, FIND_INFO_REQ, FIND_INFO_RSP, FIND_TYPE_REQ, FIND_TYPE_RSP, READ_TYPE_REQ,
       READ_TYPE_RSP, READ_REQ, READ_RSP, READ_BLOB_REQ, READ_BLOB_RSP, READ_GROUP_REQ = 0x10, READ_GROUP_RSP, WRITE_REQ,
       WRITE_RSP, PREP_WRITE_REQ = 0x16, PREP_WRITE_RSP, EXEC_WRITE_REQ, EXEC_WRITE_RSP, CONFIRM = 0x1e, WRITE_CMD = 0x52 };
enum { E_INVALID_HANDLE = 0x01, E_READ_NOT_PERMITTED, E_WRITE_NOT_PERMITTED, E_INVALID_PDU, E_NOT_SUPPORTED = 0x06,
       E_INVALID_OFFSET, E_QUEUE_FULL = 0x09, E_NOT_FOUND, E_INVALID_LENGTH = 0x0d, E_UNSUPPORTED_GROUP = 0x10 };
enum { T_PRIMARY = 0x2800, T_CHAR = 0x2803, T_CCC = 0x2902 };

static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

void gatts_init(struct gatts *s, const struct gatts_ops *ops, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->ops = ops; s->ctx = ctx; s->mtu = 23;
}

static struct gatts_attr *add(struct gatts *s, int kind)
{
    if (s->n == GATTS_MAX_ATTR) return NULL;
    struct gatts_attr *a = &s->a[s->n++];
    memset(a, 0, sizeof *a); a->kind = kind;
    if (kind != A_SERVICE)                                  /* grows the service it follows */
        for (int i = s->n - 2; i >= 0; i--) if (s->a[i].kind == A_SERVICE) { s->a[i].end = s->n; break; }
    return a;
}

unsigned gatts_service(struct gatts *s, const uint8_t *uuid, unsigned uuid_len)
{
    struct gatts_attr *a = add(s, A_SERVICE);
    if (!a || (uuid_len != 2 && uuid_len != 16)) return 0;
    memcpy(a->uuid, uuid, uuid_len); a->uuid_len = uuid_len; a->end = s->n;
    return s->n;
}

unsigned gatts_char(struct gatts *s, const uint8_t *uuid, unsigned uuid_len, unsigned props, int id)
{
    if (s->n + 2 + !!(props & GATTS_NOTIFY) > GATTS_MAX_ATTR || (uuid_len != 2 && uuid_len != 16)) return 0;
    struct gatts_attr *d = add(s, A_CHAR), *v = add(s, A_VALUE);
    d->props = v->props = props; d->value = s->n; v->id = id;
    memcpy(d->uuid, uuid, uuid_len); memcpy(v->uuid, uuid, uuid_len); d->uuid_len = v->uuid_len = uuid_len;
    if (props & GATTS_NOTIFY) { struct gatts_attr *c = add(s, A_CCC); c->id = id; }
    return d->value;
}

void gatts_connected(struct gatts *s)
{
    s->mtu = 23; s->prep_len = 0; s->prep_handle = 0;
    for (int i = 0; i < s->n; i++) s->a[i].ccc = 0;
}

/* the attribute's type, as on the air */
static unsigned type_of(const struct gatts_attr *a, uint8_t t[16])
{
    switch (a->kind) {
    case A_SERVICE: put16(t, T_PRIMARY); return 2;
    case A_CHAR: put16(t, T_CHAR); return 2;
    case A_CCC: put16(t, T_CCC); return 2;
    }
    memcpy(t, a->uuid, a->uuid_len); return a->uuid_len;
}

static int readable(const struct gatts_attr *a) { return a->kind != A_VALUE || (a->props & GATTS_READ); }
static int writable(const struct gatts_attr *a) { return a->kind == A_CCC || (a->kind == A_VALUE && (a->props & (GATTS_WRITE | GATTS_WRITE_CMD))); }

static size_t value_of(const struct gatts *s, const struct gatts_attr *a, uint8_t *v, size_t cap)
{
    size_t k;
    switch (a->kind) {
    case A_SERVICE: memcpy(v, a->uuid, a->uuid_len); return a->uuid_len;
    case A_CHAR: v[0] = a->props; put16(v + 1, a->value); memcpy(v + 3, a->uuid, a->uuid_len); return 3 + a->uuid_len;
    case A_CCC: put16(v, a->ccc); return 2;
    }
    k = s->ops && s->ops->read ? s->ops->read(s->ctx, a->id, v, cap) : 0;
    return k < cap ? k : cap;
}

static size_t error(uint8_t *r, unsigned op, unsigned handle, unsigned code)
{
    r[0] = ERROR_RSP; r[1] = op; put16(r + 2, handle); r[4] = code;
    return 5;
}

/* [start, end] of a request: 0 if fine, else the answer */
static size_t range_bad(const uint8_t *p, unsigned *start, unsigned *end, uint8_t *r)
{
    *start = u16(p + 1); *end = u16(p + 3);
    return !*start || *start > *end ? error(r, p[0], *start, E_INVALID_HANDLE) : 0;
}

static int same_type(const struct gatts_attr *a, const uint8_t *t, unsigned tl)
{
    uint8_t mine[16]; unsigned ml = type_of(a, mine);
    if (ml == tl) return !memcmp(mine, t, tl);
    /* a 16-bit type against its 128-bit form (Bluetooth base UUID) */
    static const uint8_t base[12] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
    const uint8_t *s16 = ml == 2 ? mine : t, *s128 = ml == 2 ? t : mine;
    return !memcmp(s128, base, 12) && s128[12] == s16[0] && s128[13] == s16[1] && !s128[14] && !s128[15];
}

static size_t find_info(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    unsigned start, end; size_t k = 2, bad; uint8_t t[16]; unsigned fmt = 0;
    if (n < 5) return error(r, p[0], 0, E_INVALID_PDU);
    if ((bad = range_bad(p, &start, &end, r))) return bad;
    for (unsigned h = start; h <= end && h <= (unsigned)s->n; h++) {
        unsigned tl = type_of(&s->a[h - 1], t);
        if (!fmt) fmt = tl;
        if (tl != fmt || k + 2 + tl > s->mtu) break;           /* one format per answer */
        put16(r + k, h); memcpy(r + k + 2, t, tl); k += 2 + tl;
    }
    if (!fmt) return error(r, p[0], start, E_NOT_FOUND);
    r[0] = FIND_INFO_RSP; r[1] = fmt == 2 ? 1 : 2;
    return k;
}

static size_t find_by_type(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    unsigned start, end; size_t k = 1, bad; uint8_t v[GATTS_MAX_VALUE];
    if (n < 7) return error(r, p[0], 0, E_INVALID_PDU);
    if ((bad = range_bad(p, &start, &end, r))) return bad;
    for (unsigned h = start; h <= end && h <= (unsigned)s->n && k + 4 <= s->mtu; h++) {
        struct gatts_attr *a = &s->a[h - 1];
        if (!same_type(a, p + 5, 2) || !readable(a)) continue;
        size_t vl = value_of(s, a, v, sizeof v);
        if (vl != n - 7 || memcmp(v, p + 7, vl)) continue;
        put16(r + k, h); put16(r + k + 2, a->kind == A_SERVICE ? a->end : h); k += 4;
    }
    if (k == 1) return error(r, p[0], start, E_NOT_FOUND);
    r[0] = FIND_TYPE_RSP;
    return k;
}

static size_t read_by_type(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r, int group)
{
    unsigned start, end; size_t k = 2, bad, each = 0; uint8_t v[GATTS_MAX_VALUE];
    if (n != 7 && n != 21) return error(r, p[0], 0, E_INVALID_PDU);
    if ((bad = range_bad(p, &start, &end, r))) return bad;
    if (group) {                                            /* only primary services are groups here */
        struct gatts_attr probe = { .kind = A_SERVICE };
        if (!same_type(&probe, p + 5, n - 5)) return error(r, p[0], start, E_UNSUPPORTED_GROUP);
    }
    for (unsigned h = start; h <= end && h <= (unsigned)s->n; h++) {
        struct gatts_attr *a = &s->a[h - 1];
        if (!same_type(a, p + 5, n - 5)) continue;
        if (!readable(a)) { if (!each) return error(r, p[0], h, E_READ_NOT_PERMITTED); break; }
        size_t vl = value_of(s, a, v, sizeof v), head = group ? 4 : 2, max = s->mtu - 2 - head;
        if (max > 253 - head) max = 253 - head;                /* the length octet counts handle(s) and value */
        if (vl > max) vl = max;
        if (!each) each = head + vl;
        if (head + vl != each || k + each > s->mtu) break;      /* one length per answer */
        put16(r + k, h); if (group) put16(r + k + 2, a->end);
        memcpy(r + k + head, v, vl); k += each;
    }
    if (!each) return error(r, p[0], start, E_NOT_FOUND);
    r[0] = group ? READ_GROUP_RSP : READ_TYPE_RSP; r[1] = each;
    return k;
}

static size_t read_value(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    int blob = p[0] == READ_BLOB_REQ; uint8_t v[GATTS_MAX_VALUE];
    if (n < (blob ? 5u : 3u)) return error(r, p[0], 0, E_INVALID_PDU);
    unsigned h = u16(p + 1), off = blob ? u16(p + 3) : 0;
    if (!h || h > (unsigned)s->n) return error(r, p[0], h, E_INVALID_HANDLE);
    struct gatts_attr *a = &s->a[h - 1];
    if (!readable(a)) return error(r, p[0], h, E_READ_NOT_PERMITTED);
    size_t vl = value_of(s, a, v, sizeof v);
    if (off > vl) return error(r, p[0], h, E_INVALID_OFFSET);
    size_t k = vl - off < s->mtu - 1 ? vl - off : s->mtu - 1;
    r[0] = blob ? READ_BLOB_RSP : READ_RSP; memcpy(r + 1, v + off, k);
    return 1 + k;
}

/* a whole value written: 0 or the ATT error */
static int store(struct gatts *s, struct gatts_attr *a, const uint8_t *v, size_t vl)
{
    if (a->kind == A_CCC) { if (vl != 2) return E_INVALID_LENGTH; a->ccc = u16(v); return 0; }
    return s->ops && s->ops->write ? s->ops->write(s->ctx, a->id, v, vl) : E_WRITE_NOT_PERMITTED;
}

static size_t write_value(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    int cmd = p[0] == WRITE_CMD;
    if (n < 3) return cmd ? 0 : error(r, p[0], 0, E_INVALID_PDU);
    unsigned h = u16(p + 1); int e;
    if (!h || h > (unsigned)s->n) return cmd ? 0 : error(r, p[0], h, E_INVALID_HANDLE);
    if (!writable(&s->a[h - 1])) return cmd ? 0 : error(r, p[0], h, E_WRITE_NOT_PERMITTED);
    if ((e = store(s, &s->a[h - 1], p + 3, n - 3))) return cmd ? 0 : error(r, p[0], h, e);
    if (cmd) return 0;
    r[0] = WRITE_RSP;
    return 1;
}

static size_t prep_write(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    if (n < 5) return error(r, p[0], 0, E_INVALID_PDU);
    unsigned h = u16(p + 1), off = u16(p + 3); size_t vl = n - 5;
    if (!h || h > (unsigned)s->n) return error(r, p[0], h, E_INVALID_HANDLE);
    if (s->a[h - 1].kind != A_VALUE || !writable(&s->a[h - 1])) return error(r, p[0], h, E_WRITE_NOT_PERMITTED);
    if (s->prep_handle && s->prep_handle != h) return error(r, p[0], h, E_QUEUE_FULL);     /* one attribute at a time */
    if (off != s->prep_len) return error(r, p[0], h, E_INVALID_OFFSET);                  /* in order, as clients send */
    if (off + vl > sizeof s->prep) return error(r, p[0], h, E_INVALID_LENGTH);
    memcpy(s->prep + off, p + 5, vl); s->prep_len += vl; s->prep_handle = h;
    memcpy(r, p, n); r[0] = PREP_WRITE_RSP;                                              /* echoed for the client to check */
    return n;
}

static size_t exec_write(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r)
{
    if (n < 2) return error(r, p[0], 0, E_INVALID_PDU);
    unsigned h = s->prep_handle; int e = 0;
    if (p[1] == 1 && h) e = store(s, &s->a[h - 1], s->prep, s->prep_len);
    s->prep_handle = 0; s->prep_len = 0;
    if (e) return error(r, p[0], h, e);
    r[0] = EXEC_WRITE_RSP;
    return 1;
}

size_t gatts_rx(struct gatts *s, const uint8_t *p, size_t n, uint8_t *r, size_t cap)
{
    /* Nothing longer than the MTU is a valid PDU, and answers echo requests (Prepare Write): one that is longer would
     * overrun rsp.  Dropped, as a central that ignores the MTU deserves */
    if (!n || n > s->mtu || cap < GATTS_MTU) return 0;
    switch (p[0]) {
    case MTU_REQ:
        if (n < 3) return error(r, p[0], 0, E_INVALID_PDU);
        { unsigned m = u16(p + 1); s->mtu = m < 23 ? 23 : m > GATTS_MTU ? GATTS_MTU : m; }
        r[0] = MTU_RSP; put16(r + 1, GATTS_MTU);
        return 3;
    case FIND_INFO_REQ: return find_info(s, p, n, r);
    case FIND_TYPE_REQ: return find_by_type(s, p, n, r);
    case READ_TYPE_REQ: return read_by_type(s, p, n, r, 0);
    case READ_GROUP_REQ: return read_by_type(s, p, n, r, 1);
    case READ_REQ: case READ_BLOB_REQ: return read_value(s, p, n, r);
    case WRITE_REQ: case WRITE_CMD: return write_value(s, p, n, r);
    case PREP_WRITE_REQ: return prep_write(s, p, n, r);
    case EXEC_WRITE_REQ: return exec_write(s, p, n, r);
    case CONFIRM: return 0;
    }
    if (p[0] & 0x40 || p[0] & 1) return 0;                  /* a command (signed write too), or a stray response */
    return error(r, p[0], 0, E_NOT_SUPPORTED);
}

static const struct gatts_attr *ccc_of(const struct gatts *s, int id)
{
    for (int i = 0; i < s->n; i++) if (s->a[i].kind == A_CCC && s->a[i].id == id) return &s->a[i];
    return NULL;
}

int gatts_subscribed(const struct gatts *s, int id) { const struct gatts_attr *c = ccc_of(s, id); return c && (c->ccc & 1); }

size_t gatts_notification(const struct gatts *s, int id, const uint8_t *val, size_t len, uint8_t *pdu, size_t cap)
{
    if (!gatts_subscribed(s, id)) return 0;
    unsigned h = 0;
    for (int i = 0; i < s->n; i++) if (s->a[i].kind == A_VALUE && s->a[i].id == id) h = i + 1;
    if (len > s->mtu - 3) len = s->mtu - 3;
    if (!h || cap < 3 + len) return 0;
    pdu[0] = 0x1b; put16(pdu + 1, h); memcpy(pdu + 3, val, len);
    return 3 + len;
}
