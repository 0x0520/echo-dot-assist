#include "buttons.h"
#include "board.h"
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define SHORT_PRESS_MS 1000            /* longer holds belong to acebuttond: 5 s setup mode, 21 s factory reset */

static struct button_handler handler;
static int muted_state;                /* boards without a hardware latch: the software toggle */
static pthread_mutex_t muted_lock = PTHREAD_MUTEX_INITIALIZER;

int buttons_muted(void)
{
    if (!board.privacy_latch) {        /* no sysfs truth: what we last toggled to */
        pthread_mutex_lock(&muted_lock);
        int m = muted_state;
        pthread_mutex_unlock(&muted_lock);
        return m;
    }
    char c = '0'; int f = open(board.privacy_state, O_RDONLY);
    if (f < 0) return 0;
    if (read(f, &c, 1) != 1) c = '0';
    close(f);
    return c == '1';
}

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *reader(void *arg)
{
    int rfd = (int)(long)arg;
    struct input_event ev; long long action_down = 0;
    while (read(rfd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY) continue;
        switch (ev.code) {
        case KEY_HELP:
            if (ev.value == 1) action_down = now_ms();
            else if (ev.value == 0 && action_down && now_ms() - action_down < SHORT_PRESS_MS && handler.action) handler.action();
            break;
        case KEY_MUTE:
            if (ev.value == 0 && handler.mute_changed) {
                if (board.privacy_latch) { usleep(100000); handler.mute_changed(buttons_muted()); }
                else {                  /* a plain key: toggle and report */
                    pthread_mutex_lock(&muted_lock);
                    muted_state = !muted_state;
                    int m = muted_state;
                    pthread_mutex_unlock(&muted_lock);
                    handler.mute_changed(m);
                }
            }
            break;
        case KEY_VOLUMEUP:
        case KEY_VOLUMEDOWN:
            if (ev.value == 1 && handler.volume)       /* press only; key repeat (2) would run away */ handler.volume(ev.code == KEY_VOLUMEUP ? 1 : -1);
            break;
        }
    }
    fprintf(stderr, "buttons: reader stopped\n");
    return NULL;
}

/* The mute button is not on the keypad: it toggles a hardware latch, and the gpio-privacy driver reports the latch through
 * its own input device.  Whatever event arrives there, the truth is the sysfs state. */
static void *privacy_reader(void *arg)
{
    struct input_event ev; int pfd = (int)(long)arg, last = buttons_muted();
    while (read(pfd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_SYN) continue;
        usleep(50000);
        int now = buttons_muted();
        if (now != last && handler.mute_changed) handler.mute_changed(now);
        last = now;
    }
    fprintf(stderr, "buttons: privacy reader stopped\n");
    return NULL;
}

int buttons_start(const char *device, const struct button_handler *h)
{
    pthread_t t; int pfd, f2;
    handler = *h;
    int f1 = open(device, O_RDONLY);
    if (f1 < 0) return -1;
    if (board.keypad2 && (f2 = open(board.keypad2, O_RDONLY)) >= 0) {   /* keys split over two nodes (biscuit) */
        if (pthread_create(&t, NULL, reader, (void *)(long)f2)) close(f2);
        else pthread_detach(t);
    } else if (board.keypad2) {
        fprintf(stderr, "buttons: %s not available, its keys go unnoticed\n", board.keypad2);
    }
    if (!board.privacy_input) pfd = -1;
    else if ((pfd = open(board.privacy_input, O_RDONLY)) < 0) fprintf(stderr, "buttons: %s not available, mute button changes go unnoticed\n", board.privacy_input);
    else if (pthread_create(&t, NULL, privacy_reader, (void *)(long)pfd)) close(pfd);
    else pthread_detach(t);
    if (pthread_create(&t, NULL, reader, (void *)(long)f1)) return -1;
    pthread_detach(t);
    return 0;
}
