/* Wi-Fi setup over Bluetooth: Improv's packets (improv.c), the GATT server (gatts.c) with Improv's database, the
 * state machine with its hand-off file to root, and the peripheral link (ble_periph.c) over a fake controller: HCI
 * commands and LE ACL frames are recorded instead of sent, ACL packets are fed in as the controller would. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "acl.h"
#include "ble.h"
#include "gatts.h"
#include "hci.h"
#include "improv.h"

static int bad;
static void expect(int ok, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    if (!ok) { printf("FAIL "); vprintf(fmt, a); printf("\n"); bad = 1; }
    va_end(a);
}

/* ---------------------------------------------------------------- the fake controller */

static struct { unsigned op; unsigned char p[64]; unsigned n; } cmds[64]; static int ncmd;
static int status_for_op[0x10000];
int hci_cmd(unsigned op, const void *par, unsigned n)
{
    if (ncmd < 64) { cmds[ncmd].op = op; cmds[ncmd].n = n; memcpy(cmds[ncmd].p, par, n < 64 ? n : 64); ncmd++; }
    return status_for_op[op];
}
int hci_write(const void *b, size_t n) { (void)b; (void)n; return 0; }
void hci_poke(void) {}
int ble_present(void) { return 1; }

static struct { int handle; unsigned cid; unsigned char p[300]; size_t n; } sent[64]; static int nsent;
void le_send(int handle, struct acl_tx *tx, unsigned cid, const void *pdu, size_t n)
{
    (void)tx;
    if (nsent < 64 && n <= sizeof sent[0].p) { sent[nsent].handle = handle; sent[nsent].cid = cid; memcpy(sent[nsent].p, pdu, n); sent[nsent].n = n; nsent++; }
}
static int forgotten;
void le_forget(int handle, struct acl_tx *tx) { (void)tx; forgotten = handle; }

static int find_cmd(unsigned op) { for (int i = 0; i < ncmd; i++) if (cmds[i].op == op) return i; return -1; }

/* ---------------------------------------------------------------- helpers */

static size_t att(const unsigned char *req, size_t n, unsigned char *rsp) { return improv_att(req, n, rsp, GATTS_MTU + 8); }

static size_t rpc(unsigned cmd, const unsigned char *d, size_t n, unsigned char *out)
{
    out[0] = cmd; out[1] = n; memcpy(out + 2, d, n); out[n + 2] = improv_checksum(out, n + 2);
    return n + 3;
}

static size_t wifi_rpc(const char *ssid, size_t sl, const char *psk, unsigned char *out)
{
    unsigned char d[200]; size_t pl = strlen(psk);
    d[0] = sl; memcpy(d + 1, ssid, sl); d[sl + 1] = pl; memcpy(d + sl + 2, psk, pl);
    return rpc(IMPROV_WIFI, d, sl + pl + 2, out);
}

/* a write request to the RPC characteristic (handle 14) with value v */
static size_t write_req(unsigned handle, const unsigned char *v, size_t n, unsigned char *out)
{
    out[0] = 0x12; out[1] = handle; out[2] = handle >> 8; memcpy(out + 3, v, n);
    return n + 3;
}

static int notified(unsigned handle, const unsigned char *v, size_t n)   /* the next notification is this */
{
    unsigned char pdu[GATTS_MTU + 8]; size_t k = improv_notify(pdu, sizeof pdu);
    return k == 3 + n && pdu[0] == 0x1b && pdu[1] == handle && !memcmp(pdu + 3, v, n);
}

/* ---------------------------------------------------------------- packets */

static void test_packets(void)
{
    unsigned char b[300], ssid[32], adv[31]; const unsigned char *d; size_t dl, sl; char psk[65];
    expect(improv_checksum((const unsigned char *)"\x01\x02\x03", 3) == 6 && improv_checksum((const unsigned char *)"\xff\x02", 2) == 1, "checksum");
    size_t n = wifi_rpc("home", 4, "password1", b);
    expect(n == 3 + 2 + 4 + 9 && b[1] == 15, "RPC framing: %zu bytes", n);
    expect(improv_rpc_parse(b, n, &d, &dl) == IMPROV_WIFI && dl == 15 && d == b + 2, "RPC not parsed");
    b[n - 1]++; expect(improv_rpc_parse(b, n, &d, &dl) < 0, "wrong checksum taken"); b[n - 1]--;
    expect(improv_rpc_parse(b, n - 1, &d, &dl) < 0 && improv_rpc_parse(b, 2, &d, &dl) < 0, "short frame taken");
    expect(improv_wifi_parse(d, dl, ssid, &sl, psk) == 0 && sl == 4 && !memcmp(ssid, "home", 4) && !strcmp(psk, "password1"), "Wi-Fi settings");
    /* any SSID bytes; WPA passphrases only */
    struct { const char *ssid; size_t sl; const char *psk; int want; } c[] = {
        { "a\nb\"$(x)", 9, "12345678", 0 }, { "open", 4, "", 0 }, { "x", 1, "1234567", -2 }, { "x", 1, "pass\nword", -2 },
        { "123456789012345678901234567890123", 33, "password1", -2 }, { "", 0, "password1", -2 },
        { "x", 1, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF", 0 },
        { "x", 1, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", -2 },
        { "x", 1, "123456789012345678901234567890123456789012345678901234567890123", 0 },
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        n = wifi_rpc(c[i].ssid, c[i].sl, c[i].psk, b); improv_rpc_parse(b, n, &d, &dl);
        int r = improv_wifi_parse(d, dl, ssid, &sl, psk);
        expect(r == c[i].want, "Wi-Fi settings %zu: %d, want %d", i, r, c[i].want);
        if (!r) expect(sl == c[i].sl && !memcmp(ssid, c[i].ssid, sl) && !strcmp(psk, c[i].psk), "Wi-Fi settings %zu changed", i);
    }
    unsigned char trunc[] = { 4, 'h', 'o', 'm' };
    expect(improv_wifi_parse(trunc, sizeof trunc, ssid, &sl, psk) == -1, "truncated settings taken");
    const char *s[] = { "hassmic", "1.0" };
    n = improv_rpc_result(IMPROV_DEVICE_INFO, s, 2, b, sizeof b);
    expect(n == 3 + 8 + 4 && b[0] == 3 && b[1] == 12 && b[2] == 7 && !memcmp(b + 3, "hassmic", 7) && b[10] == 3 &&
           b[n - 1] == improv_checksum(b, n - 1), "RPC result");
    n = improv_rpc_result(IMPROV_WIFI, NULL, 0, b, sizeof b);
    expect(n == 3 && b[0] == 1 && b[1] == 0 && b[2] == 1, "empty RPC result");
    n = improv_adv_data(IMPROV_AUTHORIZED, 3, adv);
    static const unsigned char uuid[16] = { 0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    expect(n == 31 && adv[0] == 2 && adv[1] == 1 && adv[2] == 6 && adv[3] == 17 && adv[4] == 7 && !memcmp(adv + 5, uuid, 16) &&
           adv[21] == 9 && adv[22] == 0x16 && adv[23] == 0x77 && adv[24] == 0x46 && adv[25] == 2 && adv[26] == 3 && !adv[27] && !adv[30],
           "advertising data");
    n = improv_scan_rsp("Echo Dot", adv);
    expect(n == 10 && adv[0] == 9 && adv[1] == 0x09 && !memcmp(adv + 2, "Echo Dot", 8), "scan response");
    n = improv_scan_rsp("Küchen Echo im Erdgeschoss links", adv);          /* 33 bytes: cut, and not inside the ü */
    expect(n <= 31 && adv[1] == 0x08 && adv[0] == n - 1, "long name not shortened: %zu", n);
    char straddle[40]; memset(straddle, 'x', 28); strcpy(straddle + 28, "\xc3\xbc");      /* the ü would straddle the cut */
    n = improv_scan_rsp(straddle, adv);
    expect(n == 30 && adv[1] == 0x08 && adv[n - 1] == 'x', "name cut inside a character");
}

/* ---------------------------------------------------------------- GATT */

static void test_gatt(void)
{
    unsigned char r[GATTS_MTU + 8]; size_t k;
    improv_connected(0x112233445566ULL, 1);
    unsigned char mtu[] = { 0x02, 0x00, 0x02 };                             /* the client offers 512 */
    k = att(mtu, 3, r); expect(k == 3 && r[0] == 0x03 && r[1] == 247 && r[2] == 0, "MTU exchange");
    unsigned char grp[] = { 0x10, 1, 0, 0xff, 0xff, 0x00, 0x28 };
    k = att(grp, 7, r);                                                    /* one length per answer: GAP first */
    expect(k == 2 + 6 && r[0] == 0x11 && r[1] == 6 && r[2] == 1 && r[4] == 5 && r[6] == 0x00 && r[7] == 0x18, "primary services: GAP");
    grp[1] = 6; k = att(grp, 7, r);
    expect(k == 2 + 20 && r[1] == 20 && r[2] == 6 && r[4] == 19 && r[6] == 0x00 && r[7] == 0x80 && r[21] == 0x00, "primary services: Improv");
    grp[1] = 20; k = att(grp, 7, r); expect(k == 5 && r[0] == 0x01 && r[4] == 0x0a, "past the last service: not found");
    unsigned char bytype[] = { 0x06, 1, 0, 0xff, 0xff, 0x00, 0x28, 0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    k = att(bytype, sizeof bytype, r); expect(k == 5 && r[0] == 0x07 && r[1] == 6 && r[3] == 19, "find the service by its UUID");
    unsigned char chars[] = { 0x08, 6, 0, 19, 0, 0x03, 0x28 };
    k = att(chars, 7, r);
    expect(k == 2 + 5 * 21 && r[1] == 21 && r[2] == 7 && r[4] == 0x12 && r[5] == 8 && r[7] == 0x01 && r[8] == 0x80, "characteristics: %zu", k);
    expect(r[2 + 2 * 21] == 13 && r[4 + 2 * 21] == 0x0c && r[2 + 4 * 21] == 18 && r[4 + 4 * 21] == 0x02 && r[7 + 4 * 21] == 0x05,
           "RPC (write) and capabilities (read) declarations");
    unsigned char info[] = { 0x04, 9, 0, 9, 0 };
    k = att(info, 5, r); expect(k == 6 && r[0] == 0x05 && r[1] == 1 && r[2] == 9 && r[4] == 0x02 && r[5] == 0x29, "descriptor: CCC");
    info[1] = 7; info[3] = 9; k = att(info, 5, r);
    expect(k == 2 + 4 && r[2] == 7, "find information: one format per answer (%zu)", k);
    unsigned char rd[] = { 0x0a, 19, 0 };
    k = att(rd, 3, r); expect(k == 2 && r[0] == 0x0b && r[1] == 3, "capabilities: identify + device info");
    rd[1] = 8; k = att(rd, 3, r); expect(k == 2 && r[1] == IMPROV_AUTH_REQUIRED, "state read");
    rd[1] = 14; k = att(rd, 3, r); expect(k == 5 && r[0] == 0x01 && r[4] == 0x02, "RPC command read: not permitted");
    rd[1] = 99; k = att(rd, 3, r); expect(k == 5 && r[4] == 0x01, "invalid handle");
    unsigned char rbt[] = { 0x08, 1, 0, 0xff, 0xff, 0x05, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    k = att(rbt, sizeof rbt, r); expect(k == 5 && r[1] == 3 && r[2] == 19 && r[4] == 3, "read by type: the capabilities value");
    unsigned char blob[] = { 0x0c, 3, 0, 5, 0 };                            /* the name ("Test Echo") from offset 5 */
    k = att(blob, 5, r); expect(k == 5 && r[0] == 0x0d && !memcmp(r + 1, "Echo", 4), "read blob");
    blob[3] = 20; k = att(blob, 5, r); expect(k == 5 && r[4] == 0x07, "read blob past the end: invalid offset");
    unsigned char w[8], one = 1, ccc[2] = { 1, 0 };
    k = att(w, write_req(8, &one, 1, w), r); expect(k == 5 && r[4] == 0x03, "state written: not permitted");
    k = att(w, write_req(9, &one, 1, w), r); expect(k == 5 && r[4] == 0x0d, "CCC with one byte taken");
    for (unsigned h = 9; h <= 17; h += h == 9 ? 3 : 5) { k = att(w, write_req(h, ccc, 2, w), r); expect(k == 1 && r[0] == 0x13, "CCC %u not written", h); }
    unsigned char unk[] = { 0x20, 0, 0 }, cmd[] = { 0x52, 99, 0, 1 }, conf[] = { 0x1e };
    k = att(unk, 3, r); expect(k == 5 && r[0] == 0x01 && r[1] == 0x20 && r[4] == 0x06, "unknown request: not supported");
    expect(att(cmd, 4, r) == 0 && att(conf, 1, r) == 0, "a command or a confirmation answered");
}

/* ---------------------------------------------------------------- the state machine and root's file */

static char dir[] = "/tmp/improv_testXXXXXX";
static int windows, identified;
static void on_window(int open) { windows = windows * 10 + 1 + open; }
static void on_identify(void) { identified++; }

static int request_is(const char *want)
{
    char p[300], b[300] = ""; struct stat st; snprintf(p, sizeof p, "%s/wifi-request", dir);
    FILE *f = fopen(p, "r"); size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
    if (f) fclose(f);
    b[n] = 0;
    return f && stat(p, &st) == 0 && (st.st_mode & 0777) == 0600 && !strcmp(b, want);
}

static void answer(const char *line)
{
    char p[300]; snprintf(p, sizeof p, "%s/wifi-request", dir); unlink(p);
    snprintf(p, sizeof p, "%s/wifi-result", dir);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void test_machine(void)
{
    unsigned char b[300], w[300], r[GATTS_MTU + 8], adv[31], rsp[31]; size_t al, rl; long long t = 1000000;
    unsigned char e4 = IMPROV_E_NOT_AUTHORIZED, s2 = IMPROV_AUTHORIZED, s3 = IMPROV_PROVISIONING, s4 = IMPROV_PROVISIONED,
                  e3 = IMPROV_E_UNABLE_TO_CONNECT, e0 = 0, ok[] = { 1, 0, 1 };
    improv_tick(t, 1);
    expect(!improv_advertise(adv, &al, rsp, &rl) && !improv_open(), "advertising with Wi-Fi");
    improv_tick(t += 1000, 0); improv_tick(t + 119000, 0);
    expect(!improv_open(), "open before 2 min without an address");
    improv_tick(t += 120000, 0);
    expect(improv_open() && windows == 2, "not open after 2 min without an address");
    expect(improv_advertise(adv, &al, rsp, &rl) && al == 31 && adv[25] == IMPROV_AUTH_REQUIRED && rl == 11, "advertised state");

    /* Wi-Fi settings before the button: refused */
    att(w, write_req(14, b, wifi_rpc("home", 4, "password1", b), w), r);
    expect(r[0] == 0x13 && improv_error() == IMPROV_E_NOT_AUTHORIZED && notified(11, &e4, 1), "not authorized");
    expect(improv_button() == 1 && improv_state() == IMPROV_AUTHORIZED && notified(8, &s2, 1), "the button did not authorize");
    expect(improv_advertise(adv, &al, rsp, &rl) && adv[25] == IMPROV_AUTHORIZED, "advertised state after the button");

    /* now taken: root's file has the SSID in hex (any bytes), the passphrase as it is */
    att(w, write_req(14, b, wifi_rpc("my\nnet", 6, "pa ss$(x)\"'", b), w), r);
    expect(r[0] == 0x13 && improv_state() == IMPROV_PROVISIONING, "Wi-Fi settings not taken");
    expect(notified(11, &e0, 1) && notified(8, &s3, 1), "error cleared, provisioning");
    expect(request_is("6d790a6e6574\npa ss$(x)\"'\n"), "request file wrong");
    improv_tick(t += 500, 0); expect(improv_state() == IMPROV_PROVISIONING, "provisioning ended without an answer");
    answer("OK joined, address 10.0.0.7");
    improv_tick(t += 500, 0);
    expect(improv_state() == IMPROV_PROVISIONED && notified(8, &s4, 1) && notified(16, ok, 3), "provisioned: state, then the RPC result");
    improv_tick(t += 1000, 1); expect(improv_open(), "closed right after success, before the client read it");
    improv_tick(t += 61000, 1); expect(!improv_open() && windows == 21, "still open a minute after success");

    /* the button held: open with Wi-Fi up; a failure; then nobody answering */
    improv_hold(); improv_tick(t += 500, 1);
    expect(improv_open() && improv_state() == IMPROV_AUTH_REQUIRED, "hold did not open it");
    improv_button(); while (improv_notify(r, sizeof r)) ;
    att(w, write_req(14, b, wifi_rpc("home", 4, "password1", b), w), r);
    expect(request_is("686f6d65\npassword1\n") && notified(8, &s3, 1), "second request wrong");
    answer("FAILED not joined: SCANNING after 30s"); improv_tick(t += 500, 1);
    expect(improv_state() == IMPROV_AUTHORIZED && improv_error() == IMPROV_E_UNABLE_TO_CONNECT, "failure not reported");
    expect(notified(11, &e3, 1) && notified(8, &s2, 1), "failure notifications: error, then the state");
    att(w, write_req(14, b, wifi_rpc("home", 4, "password1", b), w), r);
    improv_tick(t += 89000, 1); expect(improv_state() == IMPROV_PROVISIONING, "gave up on root too early");
    improv_tick(t += 2000, 1);
    expect(improv_error() == IMPROV_E_UNKNOWN && !request_is("686f6d65\npassword1\n"), "no answer: request left, or no error");
    while (improv_notify(r, sizeof r)) ;

    /* passphrases no WPA network has, and the other RPCs */
    att(w, write_req(14, b, wifi_rpc("home", 4, "short", b), w), r);
    expect(improv_error() == IMPROV_E_UNABLE_TO_CONNECT && improv_state() == IMPROV_AUTHORIZED, "short passphrase taken");
    unsigned char bogus[] = { 0x01, 0x01, 0x00, 0x00 };
    att(w, write_req(14, bogus, 4, w), r); expect(improv_error() == IMPROV_E_INVALID_RPC, "bad checksum taken");
    att(w, write_req(14, b, rpc(0x04, NULL, 0, b), w), r); expect(improv_error() == IMPROV_E_UNKNOWN_RPC, "scan is not offered");
    att(w, write_req(14, b, rpc(IMPROV_IDENTIFY, NULL, 0, b), w), r); expect(identified == 1 && improv_error() == 0, "identify");
    while (improv_notify(r, sizeof r)) ;
    att(w, write_req(14, b, rpc(IMPROV_DEVICE_INFO, NULL, 0, b), w), r);
    unsigned char pdu[GATTS_MTU + 8]; size_t k;
    while ((k = improv_notify(pdu, sizeof pdu)) && pdu[1] != 16) ;
    expect(k > 10 && pdu[3] == IMPROV_DEVICE_INFO && pdu[5] == 7 && !memcmp(pdu + 6, "hassmic", 7) && pdu[k - 1] == improv_checksum(pdu + 3, k - 4),
           "device info result");

    /* a long write (Prepare Write + Execute) of Wi-Fi settings */
    size_t n = wifi_rpc("a-very-long-network-name-32-byte", 32, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", b);
    unsigned char p1[] = { 0x16, 14, 0, 0, 0 }; memcpy(w, p1, 5); memcpy(w + 5, b, 40);
    k = att(w, 45, r); expect(k == 45 && r[0] == 0x17, "prepare write");
    w[3] = 40; memcpy(w + 5, b + 40, n - 40); k = att(w, 5 + n - 40, r); expect(r[0] == 0x17, "second prepare write");
    unsigned char ex[] = { 0x18, 1 }; k = att(ex, 2, r);
    expect(k == 1 && r[0] == 0x19 && improv_state() == IMPROV_PROVISIONING, "long write of the settings");
    answer("OK"); improv_tick(t += 500, 1);

    /* switched off: closes, and the button is the satellite's again */
    improv_enable(0); improv_tick(t += 500, 1);
    expect(!improv_open() && improv_button() == 0, "switched off but open");
    improv_hold(); improv_tick(t += 500, 1); expect(!improv_open(), "held while switched off: opened");
    improv_enable(1);

    /* an outage opens it once; the address coming back closes it again */
    improv_connected(0x112233445566ULL, 0);
    improv_tick(t += 1000, 0); improv_tick(t += 121000, 0); expect(improv_open(), "second outage did not open it");
    improv_tick(t += 1000, 1); expect(!improv_open(), "Wi-Fi back, still open");
    improv_tick(t += 1000, 0); improv_tick(t += 121000, 0); expect(improv_open(), "third outage did not open it");
    improv_tick(t += 301000, 0); expect(!improv_open(), "open past 5 min");
    improv_tick(t += 200000, 0); expect(!improv_open(), "the same outage opened it twice");
}

/* ---------------------------------------------------------------- the peripheral link over the fake controller */

static const struct ble_peripheral periph = { improv_advertise, improv_connected, improv_att, improv_notify };

static void acl_in(int handle, int first, const unsigned char *d, size_t n)    /* one ACL packet from the controller */
{
    unsigned char p[300] = { handle, (handle >> 8) | (first ? 0x20 : 0x10), n, n >> 8 };
    memcpy(p + 4, d, n);
    expect(periph_acl(p, 4 + n) == 1, "ACL packet on the peripheral link not taken");
}

static void l2cap_in(int handle, unsigned cid, const unsigned char *d, size_t n)
{
    unsigned char f[300] = { n, n >> 8, cid, cid >> 8 }; memcpy(f + 4, d, n);
    acl_in(handle, 1, f, n + 4);
}

static void test_periph(void)
{
    long long t = 900000000;
    ble_peripheral(&periph);
    improv_tick(t, 1); improv_hold(); improv_tick(t, 1);
    ncmd = 0; expect(periph_upkeep(0) == 0, "upkeep failed");
    int ip = find_cmd(0x2006), id = find_cmd(0x2008), ir = find_cmd(0x2009), ie = find_cmd(0x200a);
    expect(ip >= 0 && id > ip && ir > ip && ie > id && ie > ir && cmds[ie].p[0] == 1, "advertising not set up: params, data, scan response, enable");
    expect(id >= 0 && cmds[id].n == 32 && cmds[id].p[0] == 31 && cmds[id].p[1 + 25] == IMPROV_AUTH_REQUIRED, "advertising data");
    ncmd = 0; periph_upkeep(0); expect(ncmd == 0, "advertising set again without a change");
    improv_button(); improv_tick(t += 500, 1);
    ncmd = 0; periph_upkeep(0); id = find_cmd(0x2008);
    expect(id >= 0 && cmds[id].p[1 + 25] == IMPROV_AUTHORIZED && find_cmd(0x200a) < 0, "state change not advertised (or re-enabled)");
    ncmd = 0; periph_upkeep(1); ie = find_cmd(0x200a);
    expect(ie >= 0 && cmds[ie].p[0] == 0, "advertising not paused for a connection of ours");
    periph_lost();

    /* a controller that cannot scan and advertise at once */
    status_for_op[0x200a] = 0x0c; ncmd = 0; periph_upkeep(0);
    expect(periph_scan_pause() && periph_busy(), "refusal (0x0c) does not pause scanning");
    status_for_op[0x200a] = 0; ncmd = 0; periph_upkeep(0);
    expect(find_cmd(0x200a) >= 0 && periph_scan_pause() && !periph_busy(), "advertising not on once the scan made room");

    /* a central connects (LE Connection Complete, role peripheral) */
    unsigned char cc[] = { 0x3e, 19, 0x01, 0x00, 0x40, 0x00, 0x01, 0x00, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 24, 0, 0, 0, 0xf4, 0x01, 0 };
    expect(periph_event(cc, sizeof cc) == 1 && periph_tx(0x40) != NULL && !periph_scan_pause(), "connection not taken");
    unsigned char central[] = { 0x3e, 19, 0x01, 0x00, 0x41, 0x00, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 24, 0, 0, 0, 0xf4, 0x01, 0 };
    expect(periph_event(central, sizeof central) == 0, "a connection of ours (role central) taken");
    unsigned char second[sizeof cc]; memcpy(second, cc, sizeof cc); second[4] = 0x42;      /* one central at a time */
    expect(periph_event(second, sizeof second) == 1 && !periph_tx(0x42), "a second central taken");
    ncmd = 0; periph_upkeep(0); int dc2 = find_cmd(0x0406);
    expect(dc2 >= 0 && cmds[dc2].p[0] == 0x42, "a second central not dropped");
    unsigned char disc2[] = { 0x05, 4, 0x00, 0x42, 0x00, 0x16 };
    expect(periph_event(disc2, sizeof disc2) == 1 && periph_tx(0x40), "the second central's disconnection took the first");
    unsigned char other[4] = { 0x41, 0x00, 4, 0 };
    expect(periph_acl(other, 4) == 0, "another link's ACL taken");

    nsent = 0;
    unsigned char mtu[] = { 0x02, 0xf7, 0x00 }; l2cap_in(0x40, 4, mtu, 3);
    expect(nsent == 1 && sent[0].handle == 0x40 && sent[0].cid == 4 && sent[0].p[0] == 0x03, "MTU answer not sent");
    unsigned char ccc[] = { 0x12, 9, 0, 1, 0 }; l2cap_in(0x40, 4, ccc, 5); ccc[1] = 12; l2cap_in(0x40, 4, ccc, 5); ccc[1] = 17; l2cap_in(0x40, 4, ccc, 5);
    /* Wi-Fi settings in two ACL packets: the write's answer, then the notifications it caused */
    unsigned char b[200], w[200], f[300]; size_t n = write_req(14, b, wifi_rpc("home", 4, "password1", b), w);
    f[0] = n; f[1] = 0; f[2] = 4; f[3] = 0; memcpy(f + 4, w, n);
    nsent = 0; acl_in(0x40, 1, f, 10); acl_in(0x40, 0, f + 10, n + 4 - 10);
    expect(nsent == 2 && sent[0].p[0] == 0x13 && sent[1].p[0] == 0x1b && sent[1].p[1] == 8 && sent[1].p[3] == IMPROV_PROVISIONING,
           "write answer and notifications over the link (%d frames)", nsent);
    expect(request_is("686f6d65\npassword1\n"), "request over the link");
    answer("OK"); improv_tick(t += 500, 1);
    nsent = 0; periph_upkeep(0);
    expect(nsent == 2 && sent[0].p[1] == 8 && sent[0].p[3] == IMPROV_PROVISIONED && sent[1].p[1] == 16, "provisioned not notified from upkeep");
    unsigned char pair[] = { 0x01, 0x03, 0x00, 0x01, 16, 0, 0 };
    nsent = 0; l2cap_in(0x40, 6, pair, 7);
    expect(nsent == 1 && sent[0].cid == 6 && sent[0].p[0] == 0x05 && sent[0].p[1] == 0x05, "pairing not refused");
    unsigned char sig[] = { 0x14, 7, 8, 0, 0x80, 0, 0x40, 0, 0x40, 0, 1, 0 };
    nsent = 0; l2cap_in(0x40, 5, sig, sizeof sig);
    expect(nsent == 1 && sent[0].cid == 5 && sent[0].p[0] == 0x01 && sent[0].p[1] == 7, "L2CAP request not rejected");
    unsigned char ltk[] = { 0x3e, 13, 0x05, 0x40, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    expect(periph_event(ltk, sizeof ltk) == 1, "LTK request not taken");
    ncmd = 0; periph_upkeep(0); expect(find_cmd(0x201b) >= 0, "LTK negative reply not sent");

    /* the window closes (done): the central is dropped, then its disconnection comes */
    improv_tick(t += 61000, 1); expect(!improv_open(), "window still open");
    ncmd = 0; periph_upkeep(0); int dc = find_cmd(0x0406);
    expect(dc >= 0 && cmds[dc].p[0] == 0x40 && cmds[dc].p[2] == 0x13 && find_cmd(0x200a) < 0, "central not dropped");
    unsigned char disc[] = { 0x05, 4, 0x00, 0x40, 0x00, 0x16 };
    expect(periph_event(disc, sizeof disc) == 1 && forgotten == 0x40 && !periph_tx(0x40) && !periph_busy(), "disconnection not handled");
    periph_lost();
}

int main(void)
{
    if (!mkdtemp(dir)) { printf("FAIL no temp dir\n"); return 1; }
    setenv("HASSMIC_STATE", dir, 1);
    static const struct improv_handler h = { on_window, on_identify };
    improv_init(&h, "Test Echo", "2026.10.06.000000");
    test_packets(); test_gatt(); test_machine(); test_periph();
    char p[300]; snprintf(p, sizeof p, "%s/wifi-request", dir); unlink(p); snprintf(p, sizeof p, "%s/wifi-result", dir); unlink(p); rmdir(dir);
    if (!bad) printf("improv: packets, GATT server, window and authorization, root's request, peripheral link ok\n");
    return bad;
}
