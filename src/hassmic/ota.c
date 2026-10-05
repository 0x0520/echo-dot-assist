/*
 * Receiving end of scripts/ota-push.sh.  Runs inside hassmic, i.e. unprivileged and network-facing, so it decides nothing:
 * it checks the bundle's signature (to refuse junk before it touches the disk), stores bundle + signature in state/ota/
 * and drops a "request" file.  The root-side loop in main.sh verifies again with the tool and key from the read-only
 * system partition, installs, and writes "result", which is relayed to the pusher.
 *
 *   -> "HMOTA-PUSH1 <bundle bytes>\n"  <64 byte signature>  <bundle>
 *   <- one line: "OK <version>" | "FAILED <why>"
 *
 * The same key opens adb over Wi-Fi (adbwifi.c) without Home Assistant: the way back in when the Echo is not adopted,
 * has lost its key, or runs Wyoming, and its USB wires are gone.  Challenge and response, so that a recorded exchange
 * cannot be played again; what is signed starts differently from every bundle ("HMOTA1\n"), so neither passes for the
 * other.
 *   -> "HMOTA-ADB1\n"
 *   <- "NONCE <64 hex digits>\n"
 *   -> <64 byte signature over "HMOTA-ADB1\n" + the 32 nonce bytes>
 *   <- one line: "OK ..." once root's firewall watcher has opened it | "FAILED <why>"
 * It opens for the address the challenge was signed from only (adbwifi.c): the key holder's PC, not the network.
 *
 * One connection at a time, so the port has to be shared fairly: an address whose connections fail (no request, no
 * signature, a wrong one, too slow) three times within a minute is turned away for a minute, with one line and at once,
 * instead of holding the port for the 30 s each of those take.  One that proves the key starts from zero, and nobody
 * else's failures count against it: the owner is never locked out for longer than a minute of their own mistakes.
 *
 * An update that passes its self test (main.c) becomes the factory copy on the system partition, the one the Echo falls
 * back to: ota_healthy() tells root's installer loop, which checks that it is the installed update and that the running
 * hassmic is its binary, then writes it (scripts/system/sysinstall.sh).
 *
 * Online updates (update.c) come the same way to root, signed with the project's release key instead of the owner's:
 * ota_handoff() checks that one and leaves the bundle where a push leaves it.  This port takes the owner's key only.
 */
#include "ota.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/monocypher.h"
#include "adbwifi.h"
#include "net.h"
#include "netio.h"
#include "threadname.h"

#define MAX_BUNDLE (16u << 20)
#define HEAD_MS    10000        /* the request line */
#define SIGN_MS    20000        /* then the signature over the nonce: ota-push.sh signs in well under a second */
#define BODY_MS    30000        /* signature and bundle: this, plus a second per MIN_RATE bytes it announced */
#define MIN_RATE   (64u << 10)  /* bytes/s; Wi-Fi to the Echo does megabytes per second.  16 MiB: 4 min 46 s at most */
static int port;

static pthread_mutex_t handoff_lock = PTHREAD_MUTEX_INITIALIZER;     /* one bundle in state/ota/ at a time: push or download */

static const char *pub_path(void) { const char *e = getenv("HASSMIC_UPDATE_PUB"); return e ? e : "/system/hassmic/update.pub"; }
/* main.sh names the release key of the copy that runs (an update may bring a new one), else the factory copy's */
static const char *release_pub_path(void) { const char *e = getenv("HASSMIC_RELEASE_PUB"); return e && *e ? e : "/system/hassmic/release.pub"; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void reply(int fd, const char *line) { char b[300]; int n = snprintf(b, sizeof b, "%s\n", line); write_all(fd, b, n); fprintf(stderr, "update: %s\n", line); }

static int store(const char *dir, const char *name, const void *data, size_t len)
{
    char path[300], tmp[320]; snprintf(path, sizeof path, "%s/%s", dir, name); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(data, 1, len, f) != len) { if (f) fclose(f); return -1; }
    fclose(f);
    return rename(tmp, path);
}

static int load_key(const char *path, uint8_t pk[32])
{
    FILE *f = fopen(path, "rb");
    int ok = f && fread(pk, 1, 32, f) == 32;
    if (f) fclose(f);
    return ok;
}

/* 0 if the peer signs "<line>\n" + a fresh nonce with the update key; otherwise it has been answered already. */
static int challenge(int fd, const uint8_t pk[32], const char *line, long long by)
{
    uint8_t msg[160], sig[64]; char hex[80]; size_t ll = strlen(line); int r = open("/dev/urandom", O_RDONLY);
    memcpy(msg, line, ll); msg[ll++] = '\n';
    if (r < 0 || read(r, msg + ll, 32) != 32) { if (r >= 0) close(r); reply(fd, "FAILED no randomness"); return -1; }
    close(r);
    int n = sprintf(hex, "NONCE ");
    for (int i = 0; i < 32; i++) n += sprintf(hex + n, "%02x", msg[ll + i]);
    hex[n++] = '\n';
    if (write_all(fd, hex, n) || net_read_full_by(fd, sig, 64, by) != 64) { reply(fd, "FAILED no signature"); return -1; }
    if (crypto_eddsa_check(sig, pk, msg, ll + 32)) { reply(fd, "FAILED signature does not verify against this device's update key"); return -1; }
    return 0;
}

/* Root's answer in DIR/NAME (the installer loop looks every 2 s), waited for up to SECS. */
static void answer(const char *dir, const char *name, int secs, char *res, size_t cap)
{
    char path[300]; FILE *f; snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int t = 0; t < secs * 2; t++) {
        if ((f = fopen(path, "r"))) {
            res[0] = 0; if (fgets(res, cap, f)) res[strcspn(res, "\n")] = 0;
            fclose(f);
            if (!res[0]) snprintf(res, cap, "FAILED empty result");
            return;
        }
        usleep(500000);
    }
    snprintf(res, cap, "FAILED the installer did not answer (is the hassmic_fw service running?)");
}

static void relay(int fd, const char *dir, const char *name, int secs)
{
    char res[256]; answer(dir, name, secs, res, sizeof res); reply(fd, res);
}

/* Bundle and signature into state/ota/ with a request for root's installer; 0 if stored. */
static int stage(const uint8_t *b, size_t len, const uint8_t sig[64], char *dir, size_t dircap)
{
    char path[300];
    snprintf(dir, dircap, "%s/ota", state_dir()); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/result", dir); unlink(path);
    return store(dir, "bundle", b, len) || store(dir, "bundle.sig", sig, 64) || store(dir, "request", "1\n", 2) ? -1 : 0;
}

int ota_handoff(const uint8_t *b, size_t len, const uint8_t sig[64], char *res, size_t cap)
{
    uint8_t pk[32]; char dir[280];
    if (!load_key(release_pub_path(), pk)) { snprintf(res, cap, "FAILED no release key on this device (%s): an update with it pushed or installed first", release_pub_path()); return -1; }
    if (crypto_eddsa_check(sig, pk, b, len)) { snprintf(res, cap, "FAILED signature does not verify against the release key"); return -1; }
    pthread_mutex_lock(&handoff_lock);
    if (stage(b, len, sig, dir, sizeof dir)) snprintf(res, cap, "FAILED cannot store the bundle");
    else { fprintf(stderr, "update: %zu bytes downloaded, signed with the release key, handed to the installer\n", len); answer(dir, "result", 60, res, cap); }
    pthread_mutex_unlock(&handoff_lock);
    fprintf(stderr, "update: %s\n", res);
    return strncmp(res, "OK", 2) ? -1 : 0;
}

void ota_healthy(void)
{
    char dir[280];
    snprintf(dir, sizeof dir, "%s/ota", state_dir()); mkdir(dir, 0700);
    if (store(dir, "healthy", "1\n", 2)) fprintf(stderr, "self test: cannot tell the installer\n");
}

/* 1 once the peer has proven the key (whatever comes after), 0 if not */
static int adb_open(int fd, const uint8_t pk[32], long long by, uint32_t from)
{
    char ip[16], ok[80];
    if (!from) { reply(fd, "FAILED not an IPv4 peer"); return 0; }
    if (challenge(fd, pk, "HMOTA-ADB1", by)) return 0;
    net_ntoa4(from, ip);
    fprintf(stderr, "update: adb over Wi-Fi asked for with the update key from %s\n", ip);
    adbwifi_ask(1, ip);
    for (int t = 0; t < 30; t++) {                      /* the firewall watcher looks every 5 s */
        if (adbwifi_granted()) { snprintf(ok, sizeof ok, "OK adb over Wi-Fi open for 30 min, for %s only", ip); reply(fd, ok); return 1; }
        usleep(500000);
    }
    reply(fd, "FAILED the firewall service did not answer (is hassmic_fw running?)");
    return 1;
}

/* 1 if the peer proved that it holds the update key, 0 if not (for the back-off) */
static int handle(int fd, uint32_t from)
{
    char line[128], dir[280]; unsigned long len = 0; uint8_t sig[64], pk[32], *b;
    long long t0 = net_mono_ms();
    /* One connection at a time, so each gets an end it cannot push out: a per-read timeout let a peer sending a byte
     * every 29 s hold the port, and with it the adb way back in, for ever */
    if (net_read_until(fd, line, sizeof line, "\n", t0 + HEAD_MS) <= 0) { reply(fd, "FAILED bad request"); return 0; }
    line[strcspn(line, "\n")] = 0;
    int adb = !strcmp(line, "HMOTA-ADB1");
    if (!adb && (sscanf(line, "HMOTA-PUSH1 %lu", &len) != 1 || !len || len > MAX_BUNDLE)) { reply(fd, "FAILED bad request"); return 0; }
    if (!load_key(pub_path(), pk)) { reply(fd, "FAILED this device has no update key (install-system.sh installs it)"); return 0; }
    if (adb) return adb_open(fd, pk, t0 + HEAD_MS + SIGN_MS, from);
    if (!(b = malloc(len))) { reply(fd, "FAILED out of memory"); return 0; }
    long long by = net_mono_ms() + BODY_MS + (long long)(len / MIN_RATE) * 1000;
    if (net_read_full_by(fd, sig, 64, by) != 64 || net_read_full_by(fd, b, len, by) != (ssize_t)len) {
        reply(fd, "FAILED upload incomplete or too slow"); free(b); return 0;
    }
    if (crypto_eddsa_check(sig, pk, b, len)) { reply(fd, "FAILED signature does not verify against this device's update key"); free(b); return 0; }

    pthread_mutex_lock(&handoff_lock);
    if (stage(b, len, sig, dir, sizeof dir)) reply(fd, "FAILED cannot store the bundle");
    else { fprintf(stderr, "update: %lu bytes received, signature good, handed to the installer\n", len); relay(fd, dir, "result", 60); }
    pthread_mutex_unlock(&handoff_lock);
    free(b);
    return 1;
}

/* The listener's own: refused before it is served, or served and its outcome counted */
static void serve(int c)
{
    static struct net_backoff backoff;
    struct sockaddr_in sa; socklen_t sl = sizeof sa; char ip[16], msg[120];
    uint32_t from = getpeername(c, (struct sockaddr *)&sa, &sl) ? 0 : net_peer4((struct sockaddr *)&sa, sl);
    long long left = net_backoff_left(&backoff, from, net_mono_ms());
    if (left) {                                     /* not logged: that is what a flood would fill the log with */
        int n = snprintf(msg, sizeof msg, "FAILED too many failed connections from this address, try again in %lld s\n", (left + 999) / 1000);
        write_all(c, msg, n);
        return;
    }
    net_backoff_result(&backoff, from, handle(c, from), net_mono_ms());
    if (net_backoff_left(&backoff, from, net_mono_ms())) {
        net_ntoa4(from, ip);
        fprintf(stderr, "update: %d failed connections from %s within %d s: turned away for %d s\n",
                NET_BACKOFF_FAILS, ip, NET_BACKOFF_MS / 1000, NET_BACKOFF_MS / 1000);
    }
}

static void *listener(void *arg)
{
    thread_name("ota push");
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("update: listen"); return NULL; }
    fprintf(stderr, "update: push port %d\n", port);
    for (;;) { int c = net_accept(ls); if (c < 0) break; serve(c); close(c); }       /* one at a time */
    return NULL;
}

int ota_start(int p)
{
    pthread_t t;
    port = p;
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
