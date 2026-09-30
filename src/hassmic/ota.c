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
 *
 * Once the owner has tried a pushed update and approves it (ota-push.sh asks), it becomes the factory copy on the system
 * partition, the one the Echo falls back to.  Same challenge, over the request line with the version the owner tried,
 * so the answer approves that version and no other.  Root's installer loop checks that it is the installed update and
 * that the running hassmic is its binary, then writes it (scripts/system/sysinstall.sh).
 *   -> "HMOTA-FACTORY1 <version>\n"
 *   <- "NONCE <64 hex digits>\n"
 *   -> <64 byte signature over "HMOTA-FACTORY1 <version>\n" + the 32 nonce bytes>
 *   <- one line: "OK ..." once written | "FAILED <why>"
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
#include "netio.h"

#define MAX_BUNDLE (16u << 20)
static int port;

static const char *pub_path(void) { const char *e = getenv("HASSMIC_UPDATE_PUB"); return e ? e : "/system/hassmic/update.pub"; }
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

static int load_pub(uint8_t pk[32])
{
    FILE *f = fopen(pub_path(), "rb");
    int ok = f && fread(pk, 1, 32, f) == 32;
    if (f) fclose(f);
    return ok;
}

/* 0 if the peer signs "<line>\n" + a fresh nonce with the update key; otherwise it has been answered already. */
static int challenge(int fd, const uint8_t pk[32], const char *line)
{
    uint8_t msg[160], sig[64]; char hex[80]; size_t ll = strlen(line); int r = open("/dev/urandom", O_RDONLY);
    memcpy(msg, line, ll); msg[ll++] = '\n';
    if (r < 0 || read(r, msg + ll, 32) != 32) { if (r >= 0) close(r); reply(fd, "FAILED no randomness"); return -1; }
    close(r);
    int n = sprintf(hex, "NONCE ");
    for (int i = 0; i < 32; i++) n += sprintf(hex + n, "%02x", msg[ll + i]);
    hex[n++] = '\n';
    if (write_all(fd, hex, n) || read_full(fd, sig, 64) != 64) { reply(fd, "FAILED no signature"); return -1; }
    if (crypto_eddsa_check(sig, pk, msg, ll + 32)) { reply(fd, "FAILED signature does not verify against this device's update key"); return -1; }
    return 0;
}

/* Wait for root's answer in DIR/NAME (the installer loop looks every 2 s) and relay it. */
static void relay(int fd, const char *dir, const char *name, int secs)
{
    char path[300]; FILE *f; snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int t = 0; t < secs * 2; t++) {
        if ((f = fopen(path, "r"))) { char res[256] = ""; if (fgets(res, sizeof res, f)) res[strcspn(res, "\n")] = 0; fclose(f); reply(fd, res[0] ? res : "FAILED empty result"); return; }
        usleep(500000);
    }
    reply(fd, "FAILED the installer did not answer (is the hassmic_fw service running?)");
}

static void factory(int fd, const uint8_t pk[32], const char *line, const char *version)
{
    char dir[280], path[300];
    if (challenge(fd, pk, line)) return;
    fprintf(stderr, "update: %s approved as the factory copy with the update key\n", version);
    snprintf(dir, sizeof dir, "%s/ota", state_dir()); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/factory-result", dir); unlink(path);
    if (store(dir, "factory", version, strlen(version))) { reply(fd, "FAILED cannot store the request"); return; }
    relay(fd, dir, "factory-result", 120);
}

static void adb_open(int fd, const uint8_t pk[32])
{
    if (challenge(fd, pk, "HMOTA-ADB1")) return;
    fprintf(stderr, "update: adb over Wi-Fi asked for with the update key\n");
    adbwifi_ask(1);
    for (int t = 0; t < 30; t++) {                      /* the firewall watcher looks every 5 s */
        if (adbwifi_granted()) { reply(fd, "OK adb over Wi-Fi open for 30 min"); return; }
        usleep(500000);
    }
    reply(fd, "FAILED the firewall service did not answer (is hassmic_fw running?)");
}

static void handle(int fd)
{
    char line[128], version[64] = "", dir[280], path[300]; size_t i = 0; unsigned long len = 0; uint8_t sig[64], pk[32], *b; int used = 0;
    struct timeval tv = { 30, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (i < sizeof line - 1 && read(fd, line + i, 1) == 1 && line[i] != '\n') i++;
    line[i] = 0;
    int adb = !strcmp(line, "HMOTA-ADB1");
    /* versions as ota-push.sh makes them (0.3.0+aa353bb-dirty): nothing that means anything to the shell reading them */
    int fac = !strncmp(line, "HMOTA-FACTORY1 ", 15) && sscanf(line + 15, "%63[A-Za-z0-9.+_-]%n", version, &used) == 1 && !line[15 + used];
    if (!adb && !fac && (sscanf(line, "HMOTA-PUSH1 %lu", &len) != 1 || !len || len > MAX_BUNDLE)) { reply(fd, "FAILED bad request"); return; }
    if (!load_pub(pk)) { reply(fd, "FAILED this device has no update key (install-system.sh installs it)"); return; }
    if (adb) { adb_open(fd, pk); return; }
    if (fac) { factory(fd, pk, line, version); return; }
    if (!(b = malloc(len))) { reply(fd, "FAILED out of memory"); return; }
    if (read_full(fd, sig, 64) != 64 || read_full(fd, b, len) != (ssize_t)len) { reply(fd, "FAILED upload incomplete"); free(b); return; }
    if (crypto_eddsa_check(sig, pk, b, len)) { reply(fd, "FAILED signature does not verify against this device's update key"); free(b); return; }

    snprintf(dir, sizeof dir, "%s/ota", state_dir()); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/result", dir); unlink(path);
    if (store(dir, "bundle", b, len) || store(dir, "bundle.sig", sig, 64) || store(dir, "request", "1\n", 2)) { reply(fd, "FAILED cannot store the bundle"); free(b); return; }
    free(b);
    fprintf(stderr, "update: %lu bytes received, signature good, handed to the installer\n", len);
    relay(fd, dir, "result", 60);
}

static void *listener(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("update: listen"); return NULL; }
    fprintf(stderr, "update: push port %d\n", port);
    for (;;) { int c = net_accept(ls); if (c < 0) break; handle(c); close(c); }      /* one at a time */
    return NULL;
}

int ota_start(int p)
{
    pthread_t t;
    port = p;
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
