/* ACL fragment queue, controller flow control and L2CAP framing for LE (ble.c) and BR/EDR (bt_link.c); see acl.h. */
#include "acl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hci.h"

enum { H4_ACL = 2 };

struct acl_frag { struct acl_frag *next; int handle; size_t len; unsigned char b[]; };

static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

void acl_buffers(struct acl_queue *q, unsigned len, unsigned num, int *pool)
{
    q->len = len; q->num = num;
    if (pool) q->credits = pool;                    /* shared: the other side has filled it */
    else { q->credits = &q->own; q->own = num; }
}

void acl_flush(struct acl_queue *q)
{
    while (q->head && *q->credits > 0) {
        struct acl_frag *f = q->head; struct acl_tx *tx = q->lookup(f->handle);
        if (!(q->head = f->next)) q->tail = &q->head;
        if (tx) {
            tx->queued--;
            if (hci_write(f->b, f->len) < 0) fprintf(stderr, "%s: ACL write failed\n", q->tag);
            else { (*q->credits)--; tx->unacked++; }
        }
        free(f);
    }
}

void l2cap_send(struct acl_queue *q, int handle, struct acl_tx *tx, unsigned cid, const void *pdu, size_t n)
{
    unsigned char fr[4 + ACL_MAX_PDU];
    if (n > q->max_pdu || n > sizeof fr - 4) return;
    put16(fr, n); put16(fr + 2, cid); memcpy(fr + 4, pdu, n); n += 4;
    for (size_t o = 0; o < n; o += q->len) {
        size_t k = n - o < q->len ? n - o : q->len;
        struct acl_frag *f = malloc(sizeof *f + 5 + k);
        if (!f) return;
        f->next = NULL; f->handle = handle; f->len = 5 + k;
        f->b[0] = H4_ACL; f->b[1] = handle; f->b[2] = (handle >> 8 & 0x0f) | (o ? 0x10 : q->first);   /* continuing / first */
        put16(f->b + 3, k); memcpy(f->b + 5, fr + o, k);
        *q->tail = f; q->tail = &f->next; tx->queued++;
    }
    acl_flush(q);
}

void acl_completed(struct acl_queue *q, struct acl_tx *tx, unsigned n)
{
    *q->credits += n; if (*q->credits > (int)q->num) *q->credits = q->num;
    if (tx) { tx->unacked -= n; if (tx->unacked < 0) tx->unacked = 0; }
}

void acl_forget(struct acl_queue *q, int handle, struct acl_tx *tx)
{
    for (struct acl_frag **p = &q->head; *p; ) {
        struct acl_frag *f = *p;
        if (f->handle == handle) { *p = f->next; free(f); } else p = &f->next;
    }
    q->tail = &q->head; while (*q->tail) q->tail = &(*q->tail)->next;
    *q->credits += tx->unacked;
}

void acl_clear(struct acl_queue *q)
{
    for (struct acl_frag *f = q->head, *n; f; f = n) { n = f->next; free(f); }
    q->head = NULL; q->tail = &q->head;
}

size_t l2cap_reassemble(struct l2cap_rx *r, unsigned char *buf, size_t cap, const unsigned char *p, size_t n)
{
    if (n < 4) return 0;
    unsigned pb = p[1] >> 4 & 3; size_t len = u16(p + 2);
    if (len > n - 4) return 0;
    p += 4;
    if (pb != 1) {                                  /* first fragment (not "continuing"): L2CAP header inside */
        if (len < 4) return 0;
        r->want = 4 + u16(p); r->len = 0;
    } else if (!r->want) return 0;
    if (r->len + len > cap) { r->want = 0; return 0; }     /* bigger than we ever asked for: drop */
    memcpy(buf + r->len, p, len); r->len += len;
    if (r->len < r->want) return 0;
    r->want = 0;
    return r->len;
}
