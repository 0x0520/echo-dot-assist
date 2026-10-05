/*
 * Improv Wi-Fi over Bluetooth LE (improv.h): the GATT service, the state machine, and the hand-off to root.
 *
 * When.  Advertising a writable service that ends in a root action is attack surface, so only in a window: 2 min after
 * the Echo last had an address on wlan0 (once per outage: after a boot without Wi-Fi, after the link went), or when the
 * action button was held 5 s (what stock's oobed answered to).  5 min, longer while root is joining.  Within the window
 * the state is "authorization required" until someone presses the action button, which authorizes for 1 min (the spec's
 * suggestion; a failed attempt starts it again).  So joining needs a hand on the Echo, as stock's setup did.  A window
 * opened for a missing address closes once there is one again, unless a client is connected.
 *
 * Root.  hassmic runs unprivileged and cannot touch wpa_supplicant.  As with adb over Wi-Fi (adbwifi.c) and push
 * updates (ota.c) it asks: state/wifi-request, mode 0600, line 1 the SSID in hex (an SSID is any 32 bytes, newlines
 * included), line 2 the passphrase (WPA allows printable ASCII only: one line, always).  Root's firewall service takes
 * it within 2 s, checks it again, joins through wifi-join.sh and answers in state/wifi-result: "OK <what>" or
 * "FAILED <why>" (main.sh wifi_watch).  That takes up to ~45 s; Improv clients wait (Home Assistant's: no time limit).
 *
 * Threads.  The improv thread ticks every 500 ms; the controller thread calls the ble_peripheral callbacks; the button
 * thread calls improv_button / improv_hold.  Everything below is under lk.  The handler's callbacks run without it.
 */
#include "improv.h"
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ble.h"
#include "board.h"
#include "gatts.h"

#define NO_WIFI_MS (120 * 1000)         /* without an address this long: the window opens */
#define WINDOW_MS (5 * 60 * 1000)
#define AUTH_MS (60 * 1000)             /* authorized by a press this long (spec: "1 minute") */
#define LINGER_MS (60 * 1000)           /* after success: the client reads the result, then the window closes */
#define ANSWER_MS (90 * 1000)           /* root joins within ~45 s (wifi-join.sh); after this nobody is answering */
#define WLAN "wlan0"                    /* all models so far (device.conf WLAN; wifimotion.c, proto_esphome.c too) */
#define CAPS (IMPROV_CAP_IDENTIFY | IMPROV_CAP_DEVICE_INFO)

enum { ID_NAME = 1, ID_APPEARANCE, ID_STATE, ID_ERROR, ID_RPC, ID_RESULT, ID_CAPS };
enum { N_ERROR = 1, N_STATE = 2, N_RESULT = 4 };              /* notifications due, sent in this order */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static const struct improv_handler *H;
static const char *dev_name = "", *dev_version = "";
static struct gatts g;
static int enabled = 1, state = IMPROV_AUTH_REQUIRED, err, connected, due, shown;
static long long now, window_until, auth_until, answer_until, no_addr_since;
static int window_auto, auto_spent;
static uint8_t result[GATTS_MAX_VALUE]; static size_t result_len;

/* ---------------------------------------------------------------- packets */

unsigned improv_checksum(const uint8_t *p, size_t n) { unsigned s = 0; while (n--) s += *p++; return s & 0xff; }

int improv_rpc_parse(const uint8_t *p, size_t n, const uint8_t **data, size_t *len)
{
    if (n < 3 || (size_t)p[1] + 3 != n || improv_checksum(p, n - 1) != p[n - 1]) return -1;
    *data = p + 2; *len = p[1];
    return p[0];
}

static int hex_digits(const char *s, size_t n) { for (size_t i = 0; i < n; i++) if (!strchr("0123456789abcdefABCDEF", s[i]) || !s[i]) return 0; return 1; }

int improv_wifi_parse(const uint8_t *d, size_t n, uint8_t ssid[32], size_t *ssid_len, char psk[65])
{
    if (n < 2 || (size_t)d[0] + 2 > n) return -1;
    size_t sl = d[0], pl = d[sl + 1];
    if (sl + pl + 2 != n) return -1;
    const uint8_t *pw = d + sl + 2;
    if (!sl || sl > 32) return -2;
    if (pl == 64) { if (!hex_digits((const char *)pw, 64)) return -2; }     /* the key itself */
    else if (pl && (pl < 8 || pl > 63)) return -2;
    for (size_t i = 0; i < pl; i++) if (pw[i] < 0x20 || pw[i] > 0x7e) return -2;
    memcpy(ssid, d + 1, sl); *ssid_len = sl;
    memcpy(psk, pw, pl); psk[pl] = 0;
    return 0;
}

size_t improv_rpc_result(unsigned cmd, const char *const *strs, int n, uint8_t *out, size_t cap)
{
    size_t k = 2;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(strs[i]); if (l > 255) l = 255;
        if (k + 1 + l + 1 > cap || k - 2 + 1 + l > 255) return 0;
        out[k++] = l; memcpy(out + k, strs[i], l); k += l;
    }
    if (k + 1 > cap) return 0;
    out[0] = cmd; out[1] = k - 2; out[k] = improv_checksum(out, k);
    return k + 1;
}

/* The service UUID 00467768-6228-2272-4663-27747826800N, least significant octet first: N = 0 the service, 1..5 the
 * characteristics */
static void improv_uuid(uint8_t u[16], unsigned n)
{
    static const uint8_t base[16] = { 0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    memcpy(u, base, 16); u[0] = n;
}

/* Flags, the service UUID and its service data (state, capabilities, 4 reserved): 31 bytes exactly, all in the
 * advertisement as the spec wants (not the scan response) */
size_t improv_adv_data(unsigned state_, unsigned caps, uint8_t out[31])
{
    uint8_t *p = out;
    *p++ = 2; *p++ = 0x01; *p++ = 0x06;                     /* LE General Discoverable, no BR/EDR */
    *p++ = 17; *p++ = 0x07; improv_uuid(p, 0); p += 16;     /* complete list of 128-bit service UUIDs */
    *p++ = 9; *p++ = 0x16; *p++ = 0x77; *p++ = 0x46;        /* service data, 16-bit UUID 0x4677 */
    *p++ = state_; *p++ = caps; memset(p, 0, 4); p += 4;
    return p - out;
}

/* The name in the scan response, cut at a character if it does not fit (then "shortened", as GAP wants) */
size_t improv_scan_rsp(const char *name, uint8_t out[31])
{
    size_t l = strlen(name);
    if (l > 29) { l = 29; while (l && ((unsigned char)name[l] & 0xc0) == 0x80) l--; }
    out[0] = l + 1; out[1] = l == strlen(name) ? 0x09 : 0x08; memcpy(out + 2, name, l);
    return l + 2;
}

/* ---------------------------------------------------------------- the hand-off to root */

static const char *state_file(const char *name)
{
    static char p[2][300]; static int i;
    const char *d = getenv("HASSMIC_STATE");
    i ^= 1; snprintf(p[i], sizeof p[i], "%s/%s", d ? d : "/data/local/hassmic/state", name);
    return p[i];
}

static void wipe(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }

static int write_request(const uint8_t *ssid, size_t sl, const char *psk)
{
    char tmp[310], line[2 * 32 + 1 + 64 + 2]; size_t k = 0; int ok;
    for (size_t i = 0; i < sl; i++) k += snprintf(line + k, sizeof line - k, "%02x", ssid[i]);
    k += snprintf(line + k, sizeof line - k, "\n%s\n", psk);
    snprintf(tmp, sizeof tmp, "%s.tmp", state_file("wifi-request"));
    unlink(tmp);
    int f = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    ok = f >= 0 && fchmod(f, 0600) == 0 && write(f, line, k) == (ssize_t)k;
    if (f >= 0 && close(f)) ok = 0;
    wipe(line, sizeof line);
    if (!ok || rename(tmp, state_file("wifi-request"))) { fprintf(stderr, "improv: cannot write %s: %s\n", tmp, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

/* root's answer: 1 joined, -1 failed, 0 none yet */
static int read_result(char *why, size_t n)
{
    char l[200] = ""; FILE *f = fopen(state_file("wifi-result"), "r");
    if (!f) return 0;
    if (!fgets(l, sizeof l, f)) l[0] = 0;
    fclose(f); unlink(state_file("wifi-result"));
    l[strcspn(l, "\n")] = 0;
    snprintf(why, n, "%s", l);
    return !strncmp(l, "OK", 2) ? 1 : -1;
}

/* ---------------------------------------------------------------- state machine (lk held) */

static void set_state(int s) { if (s != state) { state = s; due |= N_STATE; } }
static void set_error(int e) { if (e != err || e) { err = e; due |= N_ERROR; } }

static void open_window(int automatic)
{
    if (!window_until) {
        fprintf(stderr, "improv: Wi-Fi setup over Bluetooth open for %d min (%s): press the action button to allow it\n",
                WINDOW_MS / 60000, automatic ? "no Wi-Fi" : "action button held");
        state = IMPROV_AUTH_REQUIRED; err = 0; result_len = 0;
    }
    window_until = now + WINDOW_MS; window_auto = automatic;
}

static void close_window(const char *why)
{
    if (!window_until) return;
    fprintf(stderr, "improv: Wi-Fi setup over Bluetooth closed (%s)\n", why);
    window_until = 0; state = IMPROV_AUTH_REQUIRED; err = 0; result_len = 0; due = 0;
}

static void provisioning_check(void)
{
    char why[200]; int r = read_result(why, sizeof why);
    if (!r && now < answer_until) return;
    if (r > 0) {
        fprintf(stderr, "improv: Wi-Fi joined (%s)\n", why);
        set_state(IMPROV_PROVISIONED);
        /* no URL to send on to: Home Assistant finds the Echo by mDNS (ESPHome sends none without its web server) */
        result_len = improv_rpc_result(IMPROV_WIFI, NULL, 0, result, sizeof result); due |= N_RESULT;
        window_until = now + LINGER_MS;
        return;
    }
    if (!r) { unlink(state_file("wifi-request")); snprintf(why, sizeof why, "no answer from root (is the hassmic_fw service running?)"); }
    fprintf(stderr, "improv: Wi-Fi not joined: %s\n", why);
    set_error(r ? IMPROV_E_UNABLE_TO_CONNECT : IMPROV_E_UNKNOWN);
    set_state(IMPROV_AUTHORIZED); auth_until = now + AUTH_MS;         /* the spec: a failed attempt restarts the timeout */
}

void improv_tick(long long t, int wifi_up)
{
    pthread_mutex_lock(&lk);
    int s0 = state;
    now = t;
    if (wifi_up) { no_addr_since = 0; auto_spent = 0; }                 /* a later outage may open it again */
    else if (!no_addr_since) no_addr_since = t;
    if (enabled && !wifi_up && !auto_spent && !window_until && t - no_addr_since >= NO_WIFI_MS) { auto_spent = 1; open_window(1); }
    if (state == IMPROV_PROVISIONING) provisioning_check();
    else if (!enabled) close_window("switched off");
    else if (window_until && t >= window_until) close_window(state == IMPROV_PROVISIONED ? "done" : "time is up");
    else if (window_until && window_auto && wifi_up && !connected) close_window("Wi-Fi is back");
    if (state == IMPROV_AUTHORIZED && t >= auth_until) { set_state(IMPROV_AUTH_REQUIRED); fprintf(stderr, "improv: authorization timed out\n"); }
    if (!connected) due = 0;
    int open = window_until != 0, change = open != shown, poke = change || state != s0 || due;
    shown = open;
    pthread_mutex_unlock(&lk);
    if (change && H && H->window) H->window(open);
    if (poke) ble_peripheral_poke();                                    /* new advertising data or notifications */
}

static int command(const uint8_t *p, size_t n)                          /* an RPC from the client: 1 = identify */
{
    const uint8_t *d; size_t dl; uint8_t ssid[32]; size_t sl; char psk[65]; int cmd = improv_rpc_parse(p, n, &d, &dl);
    set_error(IMPROV_E_NONE);
    if (cmd < 0) { set_error(IMPROV_E_INVALID_RPC); return 0; }
    if (cmd == IMPROV_IDENTIFY) return 1;
    if (cmd == IMPROV_DEVICE_INFO) {
        const char *s[] = { "hassmic", dev_version, board.model, dev_name };
        result_len = improv_rpc_result(cmd, s, 4, result, sizeof result); due |= N_RESULT;
        return 0;
    }
    if (cmd != IMPROV_WIFI) { set_error(IMPROV_E_UNKNOWN_RPC); return 0; }
    if (state == IMPROV_PROVISIONING) { set_error(IMPROV_E_UNKNOWN); return 0; }       /* one at a time */
    if (state != IMPROV_AUTHORIZED) { fprintf(stderr, "improv: Wi-Fi settings refused, not authorized (press the action button)\n");
                                      set_error(IMPROV_E_NOT_AUTHORIZED); return 0; }
    int r = improv_wifi_parse(d, dl, ssid, &sl, psk);
    if (r == -1) set_error(IMPROV_E_INVALID_RPC);
    else if (r < 0) { fprintf(stderr, "improv: Wi-Fi settings refused: no WPA network has that SSID or passphrase\n"); set_error(IMPROV_E_UNABLE_TO_CONNECT); }
    else if (write_request(ssid, sl, psk)) set_error(IMPROV_E_UNKNOWN);
    else {
        fprintf(stderr, "improv: Wi-Fi settings received (%s network), root joins\n", psk[0] ? "WPA" : "open");
        set_state(IMPROV_PROVISIONING); answer_until = now + ANSWER_MS; window_until = now + WINDOW_MS;
    }
    wipe(psk, sizeof psk);
    return 0;
}

/* ---------------------------------------------------------------- GATT */

static size_t read_cb(void *ctx, int id, uint8_t *b, size_t cap)
{
    (void)ctx;
    switch (id) {
    case ID_NAME: { size_t l = strlen(dev_name); l = l < cap ? l : cap; memcpy(b, dev_name, l); return l; }
    case ID_APPEARANCE: b[0] = b[1] = 0; return 2;                   /* unknown: GAP has no category for a speaker with mics */
    case ID_STATE: b[0] = state; return 1;
    case ID_ERROR: b[0] = err; return 1;
    case ID_CAPS: b[0] = CAPS; return 1;
    case ID_RESULT: { size_t l = result_len < cap ? result_len : cap; memcpy(b, result, l); return l; }
    }
    return 0;
}

static int identify_wanted;
static int write_cb(void *ctx, int id, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (id != ID_RPC) return 0x03;                                      /* write not permitted */
    if (command(d, n)) identify_wanted = 1;
    return 0;
}

static const struct gatts_ops ops = { read_cb, write_cb };

size_t improv_att(const uint8_t *req, size_t n, uint8_t *rsp, size_t cap)
{
    pthread_mutex_lock(&lk);
    identify_wanted = 0;
    size_t k = gatts_rx(&g, req, n, rsp, cap);
    int id = identify_wanted;
    pthread_mutex_unlock(&lk);
    if (id && H && H->identify) H->identify();
    return k;
}

size_t improv_notify(uint8_t *pdu, size_t cap)
{
    size_t k = 0;
    pthread_mutex_lock(&lk);
    while (connected && due && !k) {
        int bit = due & N_ERROR ? N_ERROR : due & N_STATE ? N_STATE : N_RESULT;
        uint8_t v = bit == N_ERROR ? err : state;
        due &= ~bit;
        if (bit == N_RESULT) k = gatts_notification(&g, ID_RESULT, result, result_len, pdu, cap);
        else k = gatts_notification(&g, bit == N_ERROR ? ID_ERROR : ID_STATE, &v, 1, pdu, cap);
    }
    if (!connected) due = 0;
    pthread_mutex_unlock(&lk);
    return k;
}

int improv_advertise(uint8_t *adv, size_t *adv_len, uint8_t *rsp, size_t *rsp_len)
{
    pthread_mutex_lock(&lk);
    int open = window_until != 0;
    if (open) { *adv_len = improv_adv_data(state, CAPS, adv); *rsp_len = improv_scan_rsp(dev_name, rsp); }
    pthread_mutex_unlock(&lk);
    return open;
}

void improv_connected(uint64_t addr, int on)
{
    pthread_mutex_lock(&lk);
    connected = on; due = 0;
    if (on) gatts_connected(&g);
    fprintf(stderr, "improv: %012llx %s\n", (unsigned long long)addr, on ? "connected" : "disconnected");
    pthread_mutex_unlock(&lk);
}

/* ---------------------------------------------------------------- the rest of the API */

int improv_state(void) { pthread_mutex_lock(&lk); int s = state; pthread_mutex_unlock(&lk); return s; }
int improv_error(void) { pthread_mutex_lock(&lk); int e = err; pthread_mutex_unlock(&lk); return e; }
int improv_open(void) { pthread_mutex_lock(&lk); int o = window_until != 0; pthread_mutex_unlock(&lk); return o; }

int improv_button(void)
{
    pthread_mutex_lock(&lk);
    int took = window_until && (state == IMPROV_AUTH_REQUIRED || state == IMPROV_AUTHORIZED);
    if (took) {
        if (state == IMPROV_AUTH_REQUIRED) fprintf(stderr, "improv: authorized by the action button\n");
        set_state(IMPROV_AUTHORIZED); auth_until = now + AUTH_MS;
        if (window_until < auth_until) window_until = auth_until;
    }
    pthread_mutex_unlock(&lk);
    if (took) ble_peripheral_poke();
    return took;
}

void improv_hold(void)
{
    pthread_mutex_lock(&lk);
    if (enabled && state != IMPROV_PROVISIONING) open_window(0);
    else if (!enabled) fprintf(stderr, "improv: action button held, but Wi-Fi setup over Bluetooth is switched off\n");
    pthread_mutex_unlock(&lk);
    ble_peripheral_poke();
}

int improv_enable(int set)
{
    pthread_mutex_lock(&lk);
    if (set >= 0 && set != enabled) { enabled = set; fprintf(stderr, "improv: Wi-Fi setup over Bluetooth %s\n", set ? "on" : "off"); }
    int e = enabled;
    pthread_mutex_unlock(&lk);
    return e;
}

/* The switch is field 17 of the protocol's settings file, which it loads only once Home Assistant connects; without
 * Wi-Fi it never does, and that is when this is wanted.  So read here at start. */
static void setting_at_start(void)
{
    const char *p = getenv("HASSMIC_SETTINGS"); char w[64]; int i = 0;
    FILE *f = fopen(p ? p : "/data/local/hassmic/state/settings", "r");
    if (!f) return;
    while (i < 17 && fscanf(f, "%63s", w) == 1) i++;
    fclose(f);
    if (i == 17) enabled = strcmp(w, "0") != 0;
}

static int wlan_up(void)
{
    struct ifaddrs *ifs = NULL; int up = 0;
    if (getifaddrs(&ifs)) return 1;                                     /* cannot tell: assume there is Wi-Fi */
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
        up |= i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && !strcmp(i->ifa_name, WLAN) &&
              ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr != 0;
    freeifaddrs(ifs);
    return up;
}

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void *thread(void *arg)
{
    (void)arg;
    for (;;) { improv_tick(mono_ms(), wlan_up()); usleep(500000); }
    return NULL;
}

void improv_init(const struct improv_handler *h, const char *name, const char *version)
{
    uint8_t u[16], s16[2];
    H = h; dev_name = name; dev_version = version;
    gatts_init(&g, &ops, NULL);
    s16[0] = 0x00; s16[1] = 0x18; gatts_service(&g, s16, 2);                     /* GAP */
    s16[0] = 0x00; s16[1] = 0x2a; gatts_char(&g, s16, 2, GATTS_READ, ID_NAME);
    s16[0] = 0x01; s16[1] = 0x2a; gatts_char(&g, s16, 2, GATTS_READ, ID_APPEARANCE);
    improv_uuid(u, 0); gatts_service(&g, u, 16);
    improv_uuid(u, 1); gatts_char(&g, u, 16, GATTS_READ | GATTS_NOTIFY, ID_STATE);
    improv_uuid(u, 2); gatts_char(&g, u, 16, GATTS_READ | GATTS_NOTIFY, ID_ERROR);
    improv_uuid(u, 3); gatts_char(&g, u, 16, GATTS_WRITE | GATTS_WRITE_CMD, ID_RPC);
    improv_uuid(u, 4); gatts_char(&g, u, 16, GATTS_READ | GATTS_NOTIFY, ID_RESULT);
    improv_uuid(u, 5); gatts_char(&g, u, 16, GATTS_READ, ID_CAPS);
    unlink(state_file("wifi-request")); unlink(state_file("wifi-result"));       /* a previous run's */
}

void improv_start(const struct improv_handler *h, const char *name, const char *version)
{
    static const struct ble_peripheral periph = { improv_advertise, improv_connected, improv_att, improv_notify };
    pthread_t t;
    if (!ble_present()) return;
    improv_init(h, name, version);
    setting_at_start();
    if (!enabled) fprintf(stderr, "improv: Wi-Fi setup over Bluetooth is switched off\n");
    ble_peripheral(&periph);
    if (!pthread_create(&t, NULL, thread, NULL)) pthread_detach(t);
}
