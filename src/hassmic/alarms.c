/* The alarm clock: see alarms.h.  State files, in the state directory like the others (written whole and renamed):
 *   alarms   one line per slot: "<slot> <on> <hh:mm:ss> <days> <done>"; done: UTC seconds up to which its occurrences
 *            are over (rang, skipped, or before it was set), -1 while it was set without a clock
 *   clock    "boot <kernel boot id>", "base <ms>" (UTC = boot clock + base), "tz <POSIX TZ>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "alarms.h"
#include "core_int.h"
#include "keyfile.h"
#include "threadname.h"

const char *const alarm_repeat_names[] = { "Once", "Every day", "Weekdays", "Weekends", "Mondays", "Tuesdays", "Wednesdays",
                                           "Thursdays", "Fridays", "Saturdays", "Sundays" };
const int alarm_repeat_days[] = { 0, ALARM_DAILY, ALARM_WEEKDAYS, ALARM_WEEKENDS, 1, 2, 4, 8, 16, 32, 64 };
const int alarm_nrepeat = sizeof alarm_repeat_days / sizeof *alarm_repeat_days;

static struct alarm_def slots[ALARM_SLOTS];
static long long done[ALARM_SLOTS], next_at[ALARM_SLOTS];
static int clock_ok;                        /* the wall clock is known: base_ms counts */
static long long base_ms;                   /* UTC in ms = boot clock + base_ms */
static char tz_str[96];
static struct tz tz;                        /* UTC until Home Assistant says */
static int ring_slot = -1;                  /* whose alarm rings (alarm_ringing() says whether it still does) */
static long long snooze_at = -1; static int snooze_slot;
/* An alarm clock set in the evening rings in the morning at whatever the volume was left at: turned down to 0 for the
 * night, it rang unheard.  So it rings at ALARM_MIN_VOLUME at least, and the volume goes back once it stops, unless
 * someone moved it meanwhile (then theirs stands).  Timers keep the volume as it is: they are set a moment before, at a
 * volume the user has just heard.  A hassmic that dies while it rings leaves the alarm's volume. */
static int boost_from = -1, boost_to = -1;

/* The boot clock rather than CLOCK_MONOTONIC: it goes on through a suspend, should the Echo ever do one */
static long long boot_ms(void) { struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static long long now_s(void) { return (boot_ms() + base_ms) / 1000; }

static void boot_id(char *out, size_t n)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    out[0] = 0;
    if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); }
    out[strcspn(out, "\r\n")] = 0;
}

static void save_alarms(void)
{
    struct keyfile k;
    if (!keyfile_write(&k, "alarms", "alarm clock")) return;
    for (int i = 0; i < ALARM_SLOTS; i++)
        fprintf(k.f, "%d %d %02d:%02d:%02d %d %lld\n", i, slots[i].on, slots[i].hour, slots[i].minute, slots[i].second, slots[i].days, done[i]);
    keyfile_commit(&k);
}

static void save_clock(void)
{
    struct keyfile k; char id[64];
    boot_id(id, sizeof id);
    if (!keyfile_write(&k, "clock", "alarm clock")) return;
    fprintf(k.f, "boot %s\nbase %lld\ntz %s\n", id[0] ? id : "-", base_ms, tz_str);
    keyfile_commit(&k);
}

static void schedule(int i)
{
    next_at[i] = -1;
    if (!clock_ok) return;
    long long now = now_s(), from = done[i] > now - ALARM_LATE_S ? done[i] : now - ALARM_LATE_S;
    next_at[i] = alarm_next(&slots[i], &tz, from);
}

static void set_tz(const char *s)
{
    if (tz_parse(s, &tz) < 0) fprintf(stderr, "alarm clock: time zone \"%s\" not understood, UTC it is\n", s);
    snprintf(tz_str, sizeof tz_str, "%.95s", s);
}

void alarms_init(void)
{
    char l[160], id[64], saved[64] = ""; FILE *f;
    for (int i = 0; i < ALARM_SLOTS; i++) { slots[i] = (struct alarm_def){ 0, 7, 0, 0, 0 }; done[i] = -1; next_at[i] = -1; }
    if ((f = keyfile_read("alarms"))) {
        while (fgets(l, sizeof l, f)) {
            int i, on, h, m, s, d; long long dn;
            if (sscanf(l, "%d %d %d:%d:%d %d %lld", &i, &on, &h, &m, &s, &d, &dn) != 7 || i < 0 || i >= ALARM_SLOTS
                || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) continue;
            slots[i] = (struct alarm_def){ on != 0, h, m, s, d & ALARM_DAILY }; done[i] = dn;
        }
        fclose(f);
    }
    if ((f = keyfile_read("clock"))) {
        long long b; int have_base = 0;
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (!strncmp(l, "boot ", 5)) snprintf(saved, sizeof saved, "%.63s", l + 5);
            else if (sscanf(l, "base %lld", &b) == 1) { base_ms = b; have_base = 1; }
            else if (!strncmp(l, "tz ", 3)) set_tz(l + 3);
        }
        fclose(f);
        boot_id(id, sizeof id);
        /* the boot clock started again with the kernel: the offset is only worth something in the boot it was taken in */
        clock_ok = have_base && id[0] && !strcmp(id, saved);
        fprintf(stderr, "alarm clock: %s\n", clock_ok ? "the time from Home Assistant, kept from before the restart"
                                                       : "no time since the Echo started; no alarm rings until Home Assistant has told it");
    }
    for (int i = 0; i < ALARM_SLOTS; i++) schedule(i);
}

/* Home Assistant sends whole seconds, cut off: the middle of that second is closest.  Less than 1.5 s off what the boot
 * clock says is taken for jitter (the request's round trip, the cut), not a correction: the boot clock drifts by
 * well under a second between the hourly asks. */
void alarms_time(long long epoch, const char *tzs)
{
    long long b = epoch * 1000 + 500 - boot_ms(), d = b - base_ms;
    int first = !clock_ok, moved = first || d >= 1500 || d <= -1500, tz_new = strcmp(tzs, tz_str) != 0;
    if (tz_new) set_tz(tzs);
    if (moved) base_ms = b;
    clock_ok = 1;
    if (!moved && !tz_new) return;
    char at[40]; iso_utc(now_s(), at, sizeof at);
    fprintf(stderr, "alarm clock: %s %s, time zone %s\n", first ? "time from Home Assistant:" : moved ? "time corrected by Home Assistant:" : "time zone changed:",
            at, tz_str[0] ? tz_str : "UTC");
    save_clock();
    int set_late = 0;
    for (int i = 0; i < ALARM_SLOTS; i++) {
        if (done[i] < 0) { done[i] = now_s(); set_late = 1; }     /* set while there was no clock: from now on */
        schedule(i);
    }
    if (set_late) save_alarms();
}

int alarms_clock(void) { return clock_ok; }
void alarms_get(int i, struct alarm_def *out) { *out = slots[i]; }

void alarms_set(int i, const struct alarm_def *a)
{
    if (i < 0 || i >= ALARM_SLOTS) return;
    slots[i] = *a;
    done[i] = clock_ok ? now_s() : -1;          /* a time already gone today is tomorrow's, not now */
    if (snooze_at >= 0 && snooze_slot == i) snooze_at = -1;
    schedule(i);
    save_alarms();
    char at[40] = "-"; if (next_at[i] >= 0) iso_utc(next_at[i], at, sizeof at);
    fprintf(stderr, "alarm clock: alarm %d %s %02d:%02d:%02d days %02x, next %s\n", i + 1, a->on ? "on" : "off", a->hour, a->minute, a->second, a->days, at);
}

int alarms_ringing(void) { return ring_slot >= 0 && alarm_ringing() ? ring_slot : -1; }

long long alarms_next(void)
{
    long long n = clock_ok ? snooze_at : -1;
    for (int i = 0; i < ALARM_SLOTS; i++) if (next_at[i] >= 0 && (n < 0 || next_at[i] < n)) n = next_at[i];
    return n;
}

static void volume_up(void)
{
    int v = core_volume();
    if (boost_to >= 0 || v >= ALARM_MIN_VOLUME) return;
    boost_from = v; boost_to = ALARM_MIN_VOLUME;
    fprintf(stderr, "alarm clock: volume %d, rings at %d\n", v, boost_to);
    core_set_volume(boost_to);
}

static void volume_back(void)
{
    if (boost_to < 0) return;
    if (core_volume() == boost_to) core_set_volume(boost_from);
    else fprintf(stderr, "alarm clock: volume moved while it rang, left at %d\n", core_volume());
    boost_from = boost_to = -1;
}

void alarms_stop(void)
{
    if (alarm_ringing()) core_alarm(0);
    ring_slot = -1; snooze_at = -1;
    volume_back();
}

int alarms_snooze(void)
{
    if (alarms_ringing() < 0) return 0;
    snooze_slot = ring_slot; snooze_at = now_s() + ALARM_SNOOZE_S;
    core_alarm(0);
    ring_slot = -1;
    volume_back();
    fprintf(stderr, "alarm clock: alarm %d snoozed for %d min\n", snooze_slot + 1, ALARM_SNOOZE_S / 60);
    return 1;
}

static void ring(int i)
{
    ring_slot = i;
    fprintf(stderr, "alarm clock: alarm %d rings\n", i + 1);
    volume_up();
    alarm_ring(ALARM_RING_S);
}

/* lock held.  -1: nothing; else the slot that started ringing (or -2: only state changed) */
static int tick(void)
{
    int rang = -1;
    if (ring_slot >= 0 && !alarm_ringing()) { ring_slot = -1; rang = -2; volume_back(); }  /* stopped: button, wake word, "stop", 10 min */
    if (!clock_ok) return rang;
    long long now = now_s();
    if (snooze_at >= 0 && now >= snooze_at) { snooze_at = -1; ring(snooze_slot); rang = snooze_slot; }
    for (int i = 0; i < ALARM_SLOTS; i++) {
        if (next_at[i] < 0 || now < next_at[i]) continue;
        done[i] = next_at[i];
        if (now - next_at[i] <= ALARM_LATE_S) { ring(i); rang = i; }
        else { fprintf(stderr, "alarm clock: alarm %d skipped, %lld min late\n", i + 1, (now - next_at[i]) / 60); if (rang == -1) rang = -2; }
        if (!slots[i].days) slots[i].on = 0;    /* once: done */
        schedule(i);
        save_alarms();
    }
    return rang;
}

void *alarm_thread(void *arg)
{
    thread_name("alarms");
    (void)arg;
    while (!core_quitting()) {
        usleep(500000);
        pthread_mutex_lock(&core_lock);
        int rang = tick();
        const struct proto *p = core_client();
        if (rang != -1 && p && p->alarms_changed) p->alarms_changed(rang);
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}
