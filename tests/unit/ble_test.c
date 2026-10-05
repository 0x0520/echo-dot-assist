/* LE pairing (ble.c) against devices that know only legacy pairing: allowed by default, refused with Authentication
 * Requirements once "secure pairing only" is on, and a legacy bond made before is then not used.  ble.c is included
 * whole, one translation unit, so its statics are in reach; the controller is a pipe (hci_write writes to fd), the
 * BR/EDR side (hci.h) is stubbed; nothing here runs a thread. */
#include <stdarg.h>
#include "../../src/hassmic/ble.c"

/* ---------------------------------------------------------------- stubs */
const struct board board = { .bt_dev = "/nonexistent" };
int  a2dp_setup(void) { return 0; }
int  a2dp_event(const unsigned char *p, size_t n) { (void)p; (void)n; return 0; }
int  a2dp_acl(const unsigned char *p, size_t n) { (void)p; (void)n; return 0; }
int  a2dp_completed(unsigned h, unsigned n) { (void)h; (void)n; return 0; }
void a2dp_acl_flush(void) {}
int  a2dp_upkeep(void) { return 0; }
int  a2dp_busy(void) { return 0; }
void a2dp_lost(void) {}
int  a2dp_streaming(void) { return 0; }

static int paired_calls, paired_ok, paired_err;
static void on_paired(uint64_t a, int ok, int err) { (void)a; paired_calls++; paired_ok = ok; paired_err = err; }
static const struct ble_handler handler = { .paired = on_paired };

/* ---------------------------------------------------------------- checks */
static int bad, rx = -1;
static void expect(int ok, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    if (!ok) { printf("FAIL "); vprintf(fmt, a); printf("\n"); bad = 1; }
    va_end(a);
}

/* The next SMP PDU we sent to the device, copied to out: its length, 0 if none.  H4 byte, ACL header, L2CAP header */
static size_t sent_smp(unsigned char *out, size_t cap)
{
    unsigned char b[128]; ssize_t n = read(rx, b, sizeof b);
    if (n < 9 || u16(b + 7) != CID_SMP) return 0;
    acl_completed(&aq, &conns[0].tx, 1);
    size_t len = u16(b + 5); if (len > cap || len > (size_t)n - 9) return 0;
    memcpy(out, b + 9, len);
    return len;
}

static struct conn *fresh(void)
{
    struct conn *c = &conns[0]; unsigned char b[64];
    while (read(rx, b, sizeof b) > 0) ;
    memset(c, 0, sizeof *c);
    c->state = C_UP; c->handle = 0x40; c->addr = 0xc0ffee000001; c->addr_type = 0;
    paired_calls = paired_ok = paired_err = 0;
    return c;
}

/* Home Assistant asks to pair; the device answers the Pairing Request with Secure Connections or without.  Returns
 * what we send next (its length, the PDU in out) */
static size_t pair(int peer_sc, unsigned char *out)
{
    struct conn *c = fresh(); unsigned char req[16];
    c->smp.asked = 1; smp_start(c);
    size_t n = sent_smp(req, sizeof req);
    if (n != 7 || req[0] != 0x01) return n && req[0] == 0x05 ? (memcpy(out, req, n), n) : 0;
    unsigned char rsp[7] = { 0x02, 0x03, 0x00, 0x01 | (peer_sc ? 0x08 : 0), 16, 0x00, 0x03 };
    smp_rx(c, rsp, sizeof rsp);
    return sent_smp(out, 80);
}

static void test_pairing(void)
{
    unsigned char o[80]; size_t n;
    have_sc = 1;
    ble_sc_only(0);
    n = pair(0, o);
    expect(n == 17 && o[0] == 0x03 && conns[0].smp.state == S_CONFIRM, "default: legacy device not paired with legacy (sent %02x)", n ? o[0] : 0);
    n = pair(1, o);
    expect(n == 65 && o[0] == 0x0c, "default: Secure Connections device not paired with it (sent %02x)", n ? o[0] : 0);

    ble_sc_only(1);
    n = pair(0, o);
    expect(n == 2 && o[0] == 0x05 && o[1] == 0x03, "secure only: legacy device not refused with Authentication Requirements");
    expect(conns[0].smp.state == S_IDLE && paired_calls == 1 && !paired_ok && paired_err == 0x03, "secure only: Home Assistant not told the pairing failed");
    n = pair(1, o);
    expect(n == 65 && o[0] == 0x0c, "secure only: Secure Connections device refused (sent %02x)", n ? o[0] : 0);

    have_sc = 0;                                            /* a controller without P-256: nothing can pair */
    n = pair(1, o);
    expect(n == 2 && o[0] == 0x05 && o[1] == 0x05 && paired_calls == 1 && !paired_ok, "secure only, controller legacy only: pairing tried");
    have_sc = 1;
}

static void test_bonds(void)
{
    struct conn *c; unsigned char sr[2] = { 0x0b, 0x01 }, o[80];
    nbonds = 1; memset(bonds, 0, sizeof bonds);
    bonds[0].addr = 0xc0ffee000001; bonds[0].type = 0; bonds[0].sc = 0; bonds[0].keysize = 16;

    ble_sc_only(0);
    c = fresh(); smp_rx(c, sr, 2);
    expect(c->need_enc && c->smp.state == S_IDLE, "default: legacy bond not used on the device's Security Request");
    ble_sc_only(1);
    c = fresh(); smp_rx(c, sr, 2);
    expect(!c->need_enc && c->smp.state == S_IDLE && !sent_smp(o, sizeof o), "secure only: legacy bond used, or replaced on the device's word");
    expect(bond_usable(bonds[0].addr, 0) == NULL, "secure only: legacy bond still usable");
    bonds[0].sc = 1;
    expect(bond_usable(bonds[0].addr, 0) == &bonds[0], "secure only: Secure Connections bond not usable");
    nbonds = 0; ble_sc_only(0);
}

int main(void)
{
    int p[2];
    if (pipe(p)) { printf("FAIL no pipe\n"); return 1; }
    rx = p[0]; fd = p[1]; fcntl(rx, F_SETFL, O_NONBLOCK);
    H = &handler;
    acl_buffers(&aq, 251, 8, NULL);
    test_pairing(); test_bonds();
    if (!bad) printf("ble: legacy pairing allowed by default, refused and legacy bonds unused with secure pairing only ok\n");
    return bad;
}
