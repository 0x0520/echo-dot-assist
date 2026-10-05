/* A minimal GATT server: what a phone or Home Assistant finds when it connects to the Echo as a central (Improv Wi-Fi,
 * improv.c).  One connection at a time, no security (nothing in it needs pairing).  Pure ATT: PDUs in, PDUs out; the
 * link around it is ble_periph.c.  Not thread safe: its owner serialises calls. */
#ifndef GATTS_H
#define GATTS_H
#include <stddef.h>
#include <stdint.h>

#define GATTS_MAX_ATTR 24
#define GATTS_MTU 247                   /* what we offer: an Improv request (SSID, passphrase) fits one write */
#define GATTS_MAX_VALUE 512             /* the most an attribute holds (ATT) */

/* characteristic properties, as the declaration carries them */
enum { GATTS_READ = 0x02, GATTS_WRITE_CMD = 0x04, GATTS_WRITE = 0x08, GATTS_NOTIFY = 0x10 };

struct gatts_ops {
    size_t (*read)(void *ctx, int id, uint8_t *buf, size_t cap);           /* the whole value of characteristic id */
    int    (*write)(void *ctx, int id, const uint8_t *data, size_t len);   /* 0, or the ATT error to answer with */
};

struct gatts_attr {
    int kind;                           /* A_SERVICE, A_CHAR (declaration), A_VALUE, A_CCC (gatts.c) */
    uint8_t uuid[16]; unsigned uuid_len;            /* service or characteristic UUID as on the air: 2 or 16 bytes */
    unsigned props, value;              /* A_CHAR: properties, handle of its value */
    int id;                             /* A_VALUE / A_CCC: the characteristic's id for the callbacks */
    unsigned end;                       /* A_SERVICE: last handle of the service */
    uint16_t ccc;                       /* A_CCC: what the central wrote (bit 0: notifications) */
};

struct gatts {
    struct gatts_attr a[GATTS_MAX_ATTR]; int n;     /* handle = index + 1 */
    const struct gatts_ops *ops; void *ctx;
    unsigned mtu;
    uint8_t prep[GATTS_MAX_VALUE]; size_t prep_len; unsigned prep_handle;    /* queued writes (Prepare Write) */
};

void gatts_init(struct gatts *s, const struct gatts_ops *ops, void *ctx);
/* The database, built once.  UUIDs least significant octet first, 2 or 16 bytes.  Returns the handle; for a
 * characteristic the value's, with a Client Characteristic Configuration descriptor after it if it notifies. */
unsigned gatts_service(struct gatts *s, const uint8_t *uuid, unsigned uuid_len);
unsigned gatts_char(struct gatts *s, const uint8_t *uuid, unsigned uuid_len, unsigned props, int id);
void   gatts_connected(struct gatts *s);        /* a new central: default MTU, nothing subscribed, no queued writes */
/* One ATT PDU from the central.  Returns the length of the answer written to rsp (cap >= GATTS_MTU), 0 for none
 * (commands, confirmations). */
size_t gatts_rx(struct gatts *s, const uint8_t *p, size_t n, uint8_t *rsp, size_t cap);
int    gatts_subscribed(const struct gatts *s, int id);
/* Handle Value Notification of characteristic id with val (cut to the MTU), 0 if the central has not subscribed */
size_t gatts_notification(const struct gatts *s, int id, const uint8_t *val, size_t len, uint8_t *pdu, size_t cap);
#endif
