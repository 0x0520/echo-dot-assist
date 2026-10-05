/* ACL data on the controller ble.c drives, for both of its users: LE (ble.c) and BR/EDR (bt_link.c).  Outgoing L2CAP
 * frames are cut into ACL packets the controller takes and queued until it has a free buffer for them (its flow
 * control: Number Of Completed Packets returns them); incoming ACL packets are put together into L2CAP frames.  Each
 * side keeps a queue of its own; when the controller has one pool of buffers for both (hci_acl_pool), the two queues
 * count down the same credits.  Controller thread only, like everything in hci.h. */
#ifndef ACL_H
#define ACL_H
#include <stddef.h>

#define ACL_MAX_PDU 1024                /* the largest L2CAP payload either side sends (BR/EDR's RX_MTU) */

struct acl_tx { int queued, unacked; };    /* per link: fragments not yet written, written but not reported complete */
struct acl_frag;

struct acl_queue {
    const char *tag;                                /* log prefix */
    struct acl_tx *(*lookup)(int handle);           /* the live link with this handle; NULL: gone, its fragments drop */
    unsigned first;                                 /* packet boundary flag of a frame's first fragment: 0x00 on LE
                                                       (not automatically flushable, all LE allows), 0x20 on BR/EDR */
    size_t max_pdu;                                 /* larger L2CAP payloads are not sent at all */
    unsigned len, num;                              /* the controller's ACL buffers: size, how many */
    int own, *credits;                              /* free buffers: own, or the pool shared with the other side */
    struct acl_frag *head, **tail;
};
#define ACL_QUEUE(q, tag_, lookup_, first_, max_pdu_) \
    { .tag = tag_, .lookup = lookup_, .first = first_, .max_pdu = max_pdu_, .len = 27, .num = 1, .credits = &(q).own, \
      .tail = &(q).head }

/* the controller's buffers (Read Buffer Size), counted in pool if not NULL (the other side's), else in q->own */
void acl_buffers(struct acl_queue *q, unsigned len, unsigned num, int *pool);
/* One L2CAP frame (basic header + pdu) for handle, queued in ACL packets of q->len and sent as far as credits allow */
void l2cap_send(struct acl_queue *q, int handle, struct acl_tx *tx, unsigned cid, const void *pdu, size_t n);
void acl_flush(struct acl_queue *q);                /* send what credits allow */
/* Number Of Completed Packets: n buffers free again, tx (NULL: link unknown) has n fewer outstanding.  No flush */
void acl_completed(struct acl_queue *q, struct acl_tx *tx, unsigned n);
/* the link is gone: its queued fragments drop, the buffers it had outstanding are free (the controller forgets them
 * at disconnection) */
void acl_forget(struct acl_queue *q, int handle, struct acl_tx *tx);
void acl_clear(struct acl_queue *q);                /* controller lost: drop every fragment */

/* L2CAP reassembly of one link's incoming frames into buf (cap bytes) */
struct l2cap_rx { size_t len, want; };
/* One ACL packet as after the H4 byte (handle + flags, length, data).  Returns the length of the whole frame in buf
 * (L2CAP header included, at least 4) once its last fragment is in, else 0.  Packets that do not fit what was announced
 * or buf are dropped, a continuation without a start is ignored. */
size_t l2cap_reassemble(struct l2cap_rx *r, unsigned char *buf, size_t cap, const unsigned char *p, size_t n);
#endif
