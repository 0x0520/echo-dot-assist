/* Stand-in for the stock Bluetooth stack's side of the mixer's A2DP output (docs/re-a2dp-source.md): serves the two
 * abstract sockets audio.a2dp.default.so connects to, acks every control command and measures the PCM it is sent.
 * Then switch the mixer over by hand and back:
 *   a2dpprobe [-t secs] [-o out.raw] [-p] [-a] &
 *   time lipc-set-prop -s com.doppler.audiod A2DPSourceConnect 1:001122334455
 *   time lipc-set-prop -s com.doppler.audiod A2DPSourceConnect 0:001122334455
 * -p reads at 44.1 kHz stereo s16 the way Fluoride's media timer does; without it the mixer writes as fast as it may.
 * Abstract names have no file and no owner: only SELinux decides who may connect (any shell domain works).
 * -a also stands in for btmanagerd's AIPC service, which the mixer asks for the speaker's name on every switch (without
 * it each switch waits ~20 s for the connect to time out).  AIPC refuses uid 0: run it through runas, after root has
 * removed btmanagerd's /dev/aipc/0. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "aipc_api.h"

static const char *cmds[] = { "NONE", "CHECK_READY", "START", "STOP", "SUSPEND", "GET_AUDIO_CONFIG", "OFFLOAD_START",
                              "INC_ACL_PRIORITY", "DEC_ACL_PRIORITY" };

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static long long t0;

static int listen_abstract(const char *name)
{
    /* the HAL's copy of socket_local_client_connect: sun_path[0] = 0, then the name without its NUL */
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t n = strlen(name);
    memcpy(a.sun_path + 1, name, n);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + 1 + n) < 0 || listen(fd, 5) < 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        exit(1);
    }
    return fd;
}

static int aipc_handler(struct aipc_task *t)
{
    unsigned char *d = t->data; uint32_t len = t->len ? *t->len : 0; int32_t ok = 0;
    printf("%6lld aipc client %u (pid %d uid %d) type %u fid 0x%02x event 0x%04x len %u\n", ms() - t0, t->client_id, t->pid, t->uid, t->type, t->function_id, t->event_id, len);
    if (t->type == 0 && d && t->function_id == 0x0a) { printf("       "); for (uint32_t i = 0; i < len && i < 16; i++) printf("%02x", d[i]); printf("\n"); }
    if (t->type != 0 || !d) return 0;
    if (t->function_id == AIPC_BT_SESSION_OPEN && len >= 11) { uint32_t s = 1; memcpy(d + 1, &s, 4); memcpy(d + 7, &ok, 4); }
    else if (t->function_id == AIPC_BT_SESSION_CLOSE && len >= 9) memcpy(d + 5, &ok, 4);
    else if (t->function_id == AIPC_BT_GET_NAME && len >= 0x103) {
        printf("       name of %02x%02x%02x%02x%02x%02x\n", d[4], d[5], d[6], d[7], d[8], d[9]);
        memcpy(d, &ok, 4); memset(d + 10, 0, 249); strcpy((char *)d + 10, "Probe Speaker");
    } else if (t->function_id == AIPC_BT_A2DP_SRC_DISCONNECT && len >= 12) {
        printf("       disconnect %02x%02x%02x%02x%02x%02x\n", d[0], d[1], d[2], d[3], d[4], d[5]);
        memcpy(d + 8, &ok, 4);
    } else if (t->status) *t->status = -1;
    return 0;
}

/* The service as btmanagerd's: its directory and socket labelled btmanagerd_aipc_tmpfs, which the mixer may connect
 * to (one created plainly would be aipcd_tmpfs, which it may not).  The label for files this thread creates. */
static int aipc_serve(void)
{
    void *lib = dlopen("libace_aipc.so", RTLD_NOW);
    aceAipc_start_fn start = lib ? (aceAipc_start_fn)dlsym(lib, "aceAipc_start") : NULL;
    if (!start) { fprintf(stderr, "aipc: %s\n", dlerror()); return -1; }
    char path[64]; snprintf(path, sizeof path, "/proc/self/task/%ld/attr/fscreate", (long)syscall(SYS_gettid));
    int fd = open(path, O_WRONLY), h = -1;
    static const char con[] = "u:object_r:btmanagerd_aipc_tmpfs:s0";
    if (fd < 0 || write(fd, con, sizeof con) < 0) fprintf(stderr, "aipc: %s: %s\n", path, strerror(errno));
    static struct aipc_server_cfg cfg;
    cfg.uuid = AIPC_BT_UUID; cfg.handler = aipc_handler; cfg.max_payload = 0x2800; cfg.r10 = 10; cfg.r14 = 1; cfg.thread_option = 1;
    int r = start(&h, &cfg);
    if (fd >= 0) { if (write(fd, "", 0) < 0) {} close(fd); }
    printf("aipc: service %u %s (%d)\n", AIPC_BT_UUID, r ? "failed" : "up", r);
    return r;
}

int main(int argc, char **argv)
{
    int secs = 0, paced = 0, aipc = 0, o;
    FILE *out = NULL;
    while ((o = getopt(argc, argv, "t:o:pa")) != -1)
        if (o == 't') secs = atoi(optarg);
        else if (o == 'p') paced = 1;
        else if (o == 'a') aipc = 1;
        else if (o == 'o' && !(out = fopen(optarg, "wb"))) { perror(optarg); return 1; }
        else if (o != 'o') { fprintf(stderr, "usage: a2dpprobe [-t secs] [-o pcm.raw] [-p] [-a]\n"); return 2; }
    int lc = listen_abstract("/data/misc/bluedroid/.a2dp_ctrl"), ld = listen_abstract("/data/misc/bluedroid/.a2dp_data");
    int ctrl = -1, data = -1;
    long long bytes = 0, total = 0, tick = t0 = ms(), since = 0, taken = 0;
    int peak = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (aipc && aipc_serve()) return 1;
    printf("listening\n");
    while (!secs || ms() - t0 < secs * 1000LL) {
        int hold = paced && data >= 0 && taken >= (ms() - since) * 1764 / 10;
        struct pollfd p[4] = { { lc, POLLIN, 0 }, { ld, POLLIN, 0 }, { ctrl, POLLIN, 0 }, { hold ? -1 : data, POLLIN, 0 } };
        poll(p, 4, hold ? 5 : 200);
        if (p[0].revents) { if (ctrl >= 0) close(ctrl); ctrl = accept(lc, NULL, NULL); printf("%6lld ctrl connected\n", ms() - t0); }
        if (p[1].revents) { if (data >= 0) close(data); data = accept(ld, NULL, NULL); since = ms(); taken = 0; printf("%6lld data connected\n", ms() - t0); }
        if (ctrl >= 0 && p[2].revents) {
            unsigned char c, ack = 0;
            if (recv(ctrl, &c, 1, 0) != 1) { printf("%6lld ctrl closed\n", ms() - t0); close(ctrl); ctrl = -1; }
            else {
                printf("%6lld cmd %u %s\n", ms() - t0, c, c < sizeof cmds / sizeof *cmds ? cmds[c] : "?");
                send(ctrl, &ack, 1, MSG_NOSIGNAL);
                if (c == 5) { uint32_t rate = 44100; unsigned char ch = 2; send(ctrl, &rate, 4, MSG_NOSIGNAL); send(ctrl, &ch, 1, MSG_NOSIGNAL); }
            }
        }
        if (data >= 0 && p[3].revents) {
            int16_t buf[4096];
            ssize_t n = recv(data, buf, paced ? 1764 : sizeof buf, 0);
            if (n <= 0) { printf("%6lld data closed\n", ms() - t0); close(data); data = -1; }
            else {
                bytes += n; total += n; taken += n;
                for (ssize_t i = 0; i < n / 2; i++) { int v = abs(buf[i]); if (v > peak) peak = v; }
                if (out) fwrite(buf, 1, n, out);
            }
        }
        long long now = ms();
        if (now - tick >= 1000) {
            /* 44.1 kHz stereo s16 is 176400 bytes/s: anything else says the format guess is wrong */
            if (bytes || data >= 0) printf("%6lld %lld bytes/s, peak %d\n", now - t0, bytes * 1000 / (now - tick), peak);
            bytes = 0; peak = 0; tick = now;
        }
    }
    printf("%6lld done, %lld bytes\n", ms() - t0, total);
    if (out) fclose(out);
    return 0;
}
