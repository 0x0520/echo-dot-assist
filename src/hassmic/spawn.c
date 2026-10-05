/* The stock tools the core runs: ledctrl, audio_manager_*, lipc-*. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* pipe2 on the PC */
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "core_int.h"

/* In a child before exec: plain group aipc.  The real group 3990 that runas -r gives us is only for hassmic's own sockets
 * (net.c); a shell would make it the effective group, and AIPC and the mixer refuse that. */
static void child_ids(void) { gid_t e = getegid(); setregid(e, e); }

/* bionic API 24 has no posix_spawn; SIGCHLD is ignored: no zombie.  Several of these run under core_lock (LED ring,
 * volume), so on the Echo they vfork: the parent thread waits only until the exec instead of having the whole address
 * space of a process with a dozen threads copied first.  What the child does before the exec is safe there: bionic's
 * getegid, setregid, setpgid, open, dup2 and execv are system calls or thin wrappers around them (no lock, no malloc;
 * setregid changes the calling task's ids only), and the child never returns from the function that forked.  Not on the
 * PC: glibc's setregid asks every thread of the process to change its ids too (setxid), which a vfork child, sharing the
 * parent's memory but not its threads, must not do. */
#ifdef __ANDROID__
#define spawn() vfork()
#else
#define spawn() fork()
#endif

void run_argv(char *const argv[])
{
    if (spawn() == 0) {
        child_ids();
        int nul = open("/dev/null", O_WRONLY | O_CLOEXEC); /* ledctrl and audio_manager_set_prop chat on stdout: two log lines */
        if (nul >= 0) dup2(nul, 1);                     /* per LED change otherwise.  Errors (stderr) still reach the log */
        execv(argv[0], argv);
        _exit(127);
    }
}

void run(const char *path, const char *a1, const char *a2)
{
    char *argv[] = { (char *)path, (char *)a1, (char *)a2, NULL };
    run_argv(argv);
}

/* Its stdout into buf (NUL-terminated, cut at n - 1); empty when it cannot run (PC build).  It gets timeout_ms to close
 * its stdout: these run on the capture thread (afe_score) and under core_lock (first reads of volume and equalizer), and
 * a lipc tool that hangs on a busy daemon would otherwise make the wake word deaf or stop the whole core.  Then its
 * process group goes (afe_score's shell has two tools under it).  No waitpid: SIGCHLD is ignored, so the kernel reaps it,
 * and a child stuck in the kernel would hang the wait just the same.
 * The pipe is close-on-exec: a tool forked meanwhile by another thread would otherwise hold its write end, and the read
 * here would wait for that one to end too. */
#define RUN_TIMEOUT_MS 1000             /* the slowest seen: two lipc calls in 150 ms */
static void run_output_ms(char *const argv[], char *buf, size_t n, int timeout_ms)
{
    int p[2]; size_t len = 0; pid_t pid;
    buf[0] = 0;
    if (pipe2(p, O_CLOEXEC)) return;
    if ((pid = spawn()) == 0) {         /* not popen: its shell would undo child_ids() */
        child_ids();
        setpgid(0, 0);
        int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
        dup2(p[1], 1); if (nul >= 0) dup2(nul, 2);  /* the copies keep no close-on-exec */
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return; }
    setpgid(pid, pid);                  /* fork on the PC: the child may not have got there yet */
    for (long long end = mono_ms() + timeout_ms; len + 1 < n; ) {
        long long left = end - mono_ms();
        if (left <= 0) {
            fprintf(stderr, "run: %s %s gave no answer within %d ms, killed\n", argv[0], argv[1] ? argv[1] : "", timeout_ms);
            if (kill(-pid, SIGKILL)) kill(pid, SIGKILL);
            break;
        }
        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, (int)left);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t k = read(p[0], buf + len, n - 1 - len);
        if (k > 0) len += (size_t)k;
        else if (k == 0 || errno != EINTR) break;
    }
    buf[len] = 0;
    close(p[0]);
}
void run_output(char *const argv[], char *buf, size_t n) { run_output_ms(argv, buf, n, RUN_TIMEOUT_MS); }
void core_run(char *const argv[], char *out, size_t n) { run_output_ms(argv, out, n, 2 * RUN_TIMEOUT_MS); }
