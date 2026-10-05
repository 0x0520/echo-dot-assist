/* The ACL and L2CAP code LE (ble.c) and BR/EDR (a2dp.c) share (acl.c), and the key files both keep (keyfile.c):
 * fragmentation and the packet boundary flags, the controller's flow control with its own and with a shared pool of
 * buffers, a link going with packets queued and outstanding, and reassembly against fragments a device gets wrong.
 * hci_write is stubbed and records what would go to the controller. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "acl.h"
#include "hci.h"
#include "keyfile.h"

static unsigned char wr[64][64]; static size_t wrn[64]; static int nwr, write_fails;
int hci_write(const void *b, size_t n)
{
    if (write_fails) return -1;
    if (nwr < 64) { memcpy(wr[nwr], b, n < 64 ? n : 64); wrn[nwr] = n; }
    nwr++;
    return 0;
}

static int bad;
static void expect(int ok, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    if (!ok) { printf("FAIL "); vprintf(fmt, a); printf("\n"); bad = 1; }
    va_end(a);
}

static struct acl_tx tx1, tx2; static int gone2;
static struct acl_tx *lookup(int h) { return h == 1 ? &tx1 : h == 2 && !gone2 ? &tx2 : NULL; }

static void test_fragments(void)
{
    struct acl_queue q = ACL_QUEUE(q, "test", lookup, 0x20, 100);
    unsigned char pdu[30]; for (int i = 0; i < 30; i++) pdu[i] = i;
    acl_buffers(&q, 27, 4, NULL);
    expect(q.credits == &q.own && q.own == 4, "own buffers not counted");
    nwr = 0; tx1 = (struct acl_tx){ 0 };
    l2cap_send(&q, 1, &tx1, 0x0041, pdu, 30);              /* 34 bytes with the header: 27 + 7 */
    expect(nwr == 2, "%d ACL packets for a 34-byte frame, want 2", nwr);
    expect(wrn[0] == 5 + 27 && wrn[1] == 5 + 7, "packet sizes %zu, %zu", wrn[0], wrn[1]);
    expect(wr[0][0] == 2 && wr[0][1] == 1 && wr[0][2] == 0x20 && wr[1][2] == 0x10, "H4 type, handle or boundary flags wrong");
    expect(wr[0][3] == 27 && wr[0][5] == 30 && wr[0][6] == 0 && wr[0][7] == 0x41 && wr[0][8] == 0, "L2CAP header wrong");
    expect(wr[0][9] == 0 && wr[1][5 + 6] == 29, "payload not carried over");
    expect(q.own == 2 && tx1.unacked == 2 && tx1.queued == 0, "credits %d, unacked %d, queued %d", q.own, tx1.unacked, tx1.queued);
    nwr = 0; l2cap_send(&q, 1, &tx1, 0x0041, pdu, 101);
    expect(nwr == 0 && tx1.queued == 0, "a PDU over max_pdu was sent");

    struct acl_queue le = ACL_QUEUE(le, "test", lookup, 0x00, 100);
    acl_buffers(&le, 27, 1, NULL); nwr = 0; l2cap_send(&le, 1, &tx1, 4, pdu, 3);
    expect(nwr == 1 && wr[0][2] == 0x00, "LE first fragment not marked non-flushable");
}

static void test_flow(void)
{
    struct acl_queue q = ACL_QUEUE(q, "test", lookup, 0x20, 1024); unsigned char pdu[10] = { 0 };
    acl_buffers(&q, 27, 2, NULL);
    tx1 = tx2 = (struct acl_tx){ 0 }; gone2 = 0; nwr = 0;
    for (int i = 0; i < 3; i++) l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu);
    l2cap_send(&q, 2, &tx2, 0x40, pdu, sizeof pdu);
    expect(nwr == 2 && q.own == 0 && tx1.queued == 1 && tx2.queued == 1, "no buffers: %d sent, %d queued", nwr, tx1.queued + tx2.queued);
    acl_completed(&q, &tx1, 1); acl_flush(&q);
    expect(nwr == 3 && tx1.queued == 0 && tx1.unacked == 2, "a free buffer did not send the next packet in order");
    acl_completed(&q, &tx1, 5);
    expect(q.own == 2 && tx1.unacked == 0, "completion counted past the buffers: %d, unacked %d", q.own, tx1.unacked);
    acl_completed(&q, NULL, 1);                             /* a handle nobody knows */
    expect(q.own == 2, "unknown handle's completion went past the buffers");

    /* link 2 goes with one packet queued and none outstanding; one more of link 1 queued behind it still goes */
    acl_buffers(&q, 27, 2, NULL); q.own = 0; tx1 = tx2 = (struct acl_tx){ 0 };
    l2cap_send(&q, 2, &tx2, 0x40, pdu, sizeof pdu); l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu);
    tx2.unacked = 1;                                        /* as if one were on the air */
    acl_forget(&q, 2, &tx2); gone2 = 1;
    expect(q.own == 1, "the gone link's outstanding buffer not returned (%d)", q.own);
    nwr = 0; acl_flush(&q);
    expect(nwr == 1 && wr[0][1] == 1 && tx1.queued == 0, "the other link's packet did not follow");
    l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu); l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu);
    expect(q.head != NULL, "nothing queued without buffers");
    acl_clear(&q);
    expect(!q.head && q.tail == &q.head, "queue not empty after clear");
    q.own = 1; nwr = 0; l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu);
    expect(nwr == 1 && !q.head, "the queue does not work after a clear");

    /* a fragment whose link went before it was sent drops without taking a buffer */
    acl_buffers(&q, 27, 2, NULL); q.own = 0; gone2 = 0; tx2 = (struct acl_tx){ 0 };
    l2cap_send(&q, 2, &tx2, 0x40, pdu, sizeof pdu); gone2 = 1; q.own = 1; nwr = 0;
    acl_flush(&q);
    expect(nwr == 0 && q.own == 1 && !q.head, "a gone link's fragment was written or kept");

    /* a failed write keeps its buffer */
    q.own = 1; write_fails = 1; tx1 = (struct acl_tx){ 0 };
    l2cap_send(&q, 1, &tx1, 0x40, pdu, sizeof pdu);
    write_fails = 0;
    expect(q.own == 1 && tx1.unacked == 0, "a failed write took a buffer");
}

static void test_pool(void)
{
    /* the controller has one pool for LE and BR/EDR: both queues count it down, either side's completions free it */
    struct acl_queue le = ACL_QUEUE(le, "le", lookup, 0x00, 100), br = ACL_QUEUE(br, "br", lookup, 0x20, 1024);
    unsigned char pdu[4] = { 0 };
    acl_buffers(&le, 27, 3, NULL); acl_buffers(&br, 27, 3, &le.own);
    expect(br.credits == &le.own, "BR/EDR does not count in the shared pool");
    tx1 = tx2 = (struct acl_tx){ 0 }; gone2 = 0; nwr = 0;
    l2cap_send(&le, 1, &tx1, 4, pdu, 4); l2cap_send(&br, 2, &tx2, 0x40, pdu, 4); l2cap_send(&br, 2, &tx2, 0x40, pdu, 4);
    l2cap_send(&br, 2, &tx2, 0x40, pdu, 4);
    expect(nwr == 3 && le.own == 0 && tx2.queued == 1, "pool overrun: %d written, %d free", nwr, le.own);
    acl_completed(&le, &tx1, 1); acl_flush(&le); acl_flush(&br);
    expect(nwr == 4 && le.own == 0 && tx2.queued == 0, "LE's completion did not let BR/EDR go on");
    acl_forget(&br, 2, &tx2);
    expect(le.own == 3, "a gone BR/EDR link's buffers not back in the pool (%d)", le.own);
}

static size_t feed(struct l2cap_rx *r, unsigned char *buf, size_t cap, unsigned flags, const void *d, size_t n)
{
    unsigned char p[300] = { 0x01, (unsigned char)(flags << 4), (unsigned char)n, (unsigned char)(n >> 8) };
    memcpy(p + 4, d, n);
    return l2cap_reassemble(r, buf, cap, p, 4 + n);
}

static void test_reassembly(void)
{
    struct l2cap_rx r = { 0 }; unsigned char buf[32];
    unsigned char whole[8] = { 4, 0, 0x41, 0, 'a', 'b', 'c', 'd' };
    expect(feed(&r, buf, sizeof buf, 2, whole, 8) == 8 && !memcmp(buf, whole, 8), "a frame in one packet not delivered");
    expect(feed(&r, buf, sizeof buf, 2, whole, 4) == 0, "a first fragment delivered early");
    expect(feed(&r, buf, sizeof buf, 1, whole + 4, 3) == 0, "a frame delivered short");
    expect(feed(&r, buf, sizeof buf, 1, whole + 7, 1) == 8 && !memcmp(buf, whole, 8), "fragments not put together");
    expect(feed(&r, buf, sizeof buf, 1, whole, 4) == 0, "a continuation without a start taken");
    expect(feed(&r, buf, sizeof buf, 0, whole, 8) == 8, "LE's first-fragment flag (0) not taken as a start");
    unsigned char hdr[3] = { 4, 0, 0x41 };
    expect(feed(&r, buf, sizeof buf, 2, hdr, 3) == 0 && !r.want, "a start too short for the L2CAP header taken");

    /* more than the buffer holds: dropped, and the rest of it is not taken as a frame */
    unsigned char big[24] = { 40, 0, 0x41, 0 };
    expect(feed(&r, buf, sizeof buf, 2, big, 24) == 0 && r.want == 44, "start of a long frame refused");
    expect(feed(&r, buf, sizeof buf, 1, big, 20) == 0 && !r.want, "a frame past the buffer not dropped");
    expect(feed(&r, buf, sizeof buf, 1, big, 4) == 0, "the rest of a dropped frame taken");

    /* an ACL length past what arrived, or a packet shorter than its header */
    unsigned char lie[8] = { 0x01, 0x20, 200, 0, 4, 0, 0x41, 0 };
    expect(l2cap_reassemble(&r, buf, sizeof buf, lie, 8) == 0, "ACL length past the packet taken");
    expect(l2cap_reassemble(&r, buf, sizeof buf, lie, 3) == 0, "packet shorter than the ACL header taken");

    /* a new start abandons a frame half put together */
    expect(feed(&r, buf, sizeof buf, 2, whole, 5) == 0, "partial start");
    expect(feed(&r, buf, sizeof buf, 2, whole, 8) == 8 && !memcmp(buf, whole, 8), "a new start did not replace the old");
}

static void test_keyfile(void)
{
    char dir[] = "/tmp/acl_testXXXXXX", p[300]; struct stat st;
    if (!mkdtemp(dir)) { printf("FAIL no temp dir\n"); bad = 1; return; }
    setenv("HASSMIC_STATE", dir, 1);
    expect(!keyfile_read("keys"), "a key file that does not exist read");
    struct keyfile k; FILE *f = keyfile_write(&k, "keys", "test");
    expect(f != NULL, "key file not created");
    if (!f) return;
    fputs("one\n", f); keyfile_commit(&k);
    snprintf(p, sizeof p, "%s/keys", dir);
    expect(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "key file mode %o, want 600", st.st_mode & 0777);
    snprintf(p, sizeof p, "%s/keys.tmp", dir);
    expect(access(p, F_OK) != 0, "temporary file left behind");
    if ((f = keyfile_write(&k, "keys", "test"))) { fputs("two\n", f); keyfile_commit(&k); }
    char l[16] = ""; f = keyfile_read("keys");
    expect(f && fgets(l, sizeof l, f) && !strcmp(l, "two\n"), "key file not replaced: \"%s\"", l);
    if (f) fclose(f);
    snprintf(p, sizeof p, "%s/keys", dir); unlink(p); rmdir(dir);
    setenv("HASSMIC_STATE", "/nonexistent/dir", 1);
    expect(!keyfile_write(&k, "keys", "test"), "key file written where it cannot be");
}

int main(void)
{
    test_fragments(); test_flow(); test_pool(); test_reassembly(); test_keyfile();
    if (!bad) printf("acl: fragments, flow control, shared pool, reassembly, key files ok\n");
    return bad;
}
