/* Our own sounds: earcons and the alarm, played by the earcon thread; and what of ours plays at all, which the wake word
 * threshold, sound detection and the mic gain go by. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* M_PI */
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "a2dp.h"
#include "audio.h"
#include "core_int.h"
#include "sendspin.h"
#include "wake.h"
#include "threadname.h"

static int use_earcon = 1;                          /* under lock (or before the threads start) */
static atomic_int sounds_pending;
/* The earcon thread sleeps on ear_cond until a sound or the alarm wants it.  Whoever sets either signals it under
 * ear_lock (a leaf lock: taken under core_lock too) */
static pthread_mutex_t ear_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ear_cond = PTHREAD_COND_INITIALIZER;
static void ear_wake(void) { pthread_mutex_lock(&ear_lock); pthread_cond_signal(&ear_cond); pthread_mutex_unlock(&ear_lock); }
void sound_queue(enum sound s) { atomic_fetch_or(&sounds_pending, 1 << s); ear_wake(); }              /* played by the earcon thread */
void sound_request(enum sound s) { if (use_earcon) sound_queue(s); }
void sound_unqueue_wake(void) { atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH)); }

int core_wake_sound(int set) { if (set >= 0) use_earcon = set; return use_earcon; }

static atomic_int alarm_on;
static atomic_llong alarm_limit_ms;                 /* how long the alarm asked for last rings at most ... */
static atomic_int alarm_renew;                      /* ... from when the earcon thread sees this (it keeps the longer) */
static atomic_int tts_on, music_on;                 /* something plays: the wake word model lowers its threshold then.
                                                    * music_on: MUSIC_* bits */
static atomic_int earcon_sounding;                  /* one of our sounds plays ... */
static atomic_llong earcon_heard_until;             /* ... and is still in the mic stream until then (mono_ms) */

int alarm_ringing(void) { return atomic_load(&alarm_on); }

/* Amazon's keyword models accept the wake word at a lower score while the device itself makes noise (kw.cfg.json:
 * "AlarmState" 1 cuts the ECHO threshold from 0.75 to 0.45, "AudioPlayerState" / "audio_playback" 1 to 0.70), because
 * that is when the user shouts over it and a false accept costs little. */
static atomic_llong own_sound_ms;                   /* last time something of ours started or stopped playing */

static void playback_hint(void)
{
    int alarm = atomic_load(&alarm_on), music = atomic_load(&music_on) != 0, tts = atomic_load(&tts_on);
    atomic_store(&own_sound_ms, mono_ms());
    wake_property("AlarmState", alarm);
    wake_property("AudioPlayerState", music);
    wake_property("audio_playback", alarm || music || tts);
}

void own_tts(int on) { atomic_store(&tts_on, on); playback_hint(); }

int own_sound_within(long long ms)
{
    long long now = mono_ms();
    return atomic_load(&alarm_on) || atomic_load(&music_on) || atomic_load(&tts_on) || atomic_load(&earcon_sounding)
        || atomic_load(&sounds_pending) || now - atomic_load(&own_sound_ms) < ms
        || now - atomic_load(&earcon_heard_until) < ms;
}

int own_sound_hold(void)
{
    return atomic_load(&sounds_pending) || atomic_load(&earcon_sounding) || mono_ms() < atomic_load(&earcon_heard_until);
}

static void alarm_set(int on, long long limit_ms)
{
    if (on) { atomic_store(&alarm_limit_ms, limit_ms); atomic_store(&alarm_renew, 1); }
    if (atomic_exchange(&alarm_on, on) == on) { if (on) ear_wake(); return; }
    ear_wake();
    fprintf(stderr, "alarm: %s\n", on ? "ringing" : "off");
    led(on ? "-s" : "-u", "active_timer");
    playback_hint();
}

void core_alarm(int on) { alarm_set(on, 60000); }                          /* a timer: a minute, as stock */
void alarm_ring(int seconds) { alarm_set(1, seconds * 1000LL); }

/* The newest music source wins: a Bluetooth device that starts pauses the Sendspin group (the controller role; the
 * whole group, since a player cannot tell whether it has the group to itself), a Sendspin stream that starts pauses the
 * Bluetooth device (AVRCP; without it the device only goes unheard until Sendspin stops).  No automatic resume. */
void core_music(int source, int on)
{
    int was = on ? atomic_fetch_or(&music_on, source) : atomic_fetch_and(&music_on, ~source);
    if (on && !(was & source)) {
        if (source == MUSIC_BLUETOOTH && was & MUSIC_SENDSPIN && core_sendspin_port) sendspin_pause();
        if (source == MUSIC_SENDSPIN && was & MUSIC_BLUETOOTH) a2dp_pause();
    }
    if (!on && source == MUSIC_SENDSPIN && was & MUSIC_SENDSPIN) a2dp_unyield();
    if (!was != !atomic_load(&music_on)) playback_hint();
}

void *earcon_thread(void *arg)
{
    thread_name("earcon");
    enum { RATE = 48000, N = RATE * 12 / 100 };
    static short tone[N];
    static const char *const snd_names[SND_COUNT] = { "wake", "touch", "mics off", "mics on", "volume", "bluetooth connected",
                                                                "bluetooth disconnected" };
    (void)arg;
    for (int i = 0; i < N; i++) {           /* 120 ms rising two-tone blip with 10 ms fades */
        double f = i < N / 2 ? 880.0 : 1320.0, env = fmin(1.0, fmin(i, N - i) / (RATE * 0.01));
        tone[i] = (short)(6000 * env * sin(2 * M_PI * f * i / RATE));
    }
    for (long long alarm_end = 0;;) {
        /* Amazon's own sounds where the image has them; the generated blip stands in for the wake and touch sounds otherwise */
        for (int p = atomic_exchange(&sounds_pending, 0), s = 0; p && s < SND_COUNT; s++) {
            const short *pcm; size_t n; unsigned rate;
            if (!(p & 1 << s)) continue;
            atomic_store(&earcon_sounding, 1);
            if (sound_get((enum sound)s, &pcm, &n, &rate)) { fprintf(stderr, "sound: %s\n", snd_names[s]); play_earcon(pcm, n, rate); }
            else if (s == SND_WAKE || s == SND_TOUCH) play_earcon(tone, N, RATE);
            atomic_store(&earcon_heard_until, mono_ms() + 250);         /* speaker to mic stream: 85 ms, and the room's tail */
            atomic_store(&earcon_sounding, 0);
        }
        long long wait_until = 0;                       /* 0: until something is asked for */
        if (atomic_load(&alarm_on)) {                   /* timer or alarm clock: triple blip every 1.2 s, for its time */
            /* a timer finishing while the alarm clock rings must not cut that short, nor the other way round */
            if (atomic_exchange(&alarm_renew, 0)) { long long e = mono_ms() + atomic_load(&alarm_limit_ms); if (e > alarm_end) alarm_end = e; }
            if (mono_ms() > alarm_end) core_alarm(0);
            else { for (int k = 0; k < 3; k++) play_earcon(tone, N, RATE); wait_until = mono_ms() + 800; }
        } else alarm_end = 0;
        /* It polled every 20 ms, for ever, and a sound asked for while the alarm rang waited out the 800 ms pause */
        pthread_mutex_lock(&ear_lock);
        while (!atomic_load(&sounds_pending) && (wait_until ? mono_ms() < wait_until : !atomic_load(&alarm_on))) {
            if (!wait_until) { pthread_cond_wait(&ear_cond, &ear_lock); continue; }
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            long long ns = ts.tv_nsec + (wait_until - mono_ms()) * 1000000LL;
            ts.tv_sec += ns / 1000000000; ts.tv_nsec = ns % 1000000000;
            if (pthread_cond_timedwait(&ear_cond, &ear_lock, &ts) == ETIMEDOUT) break;
            if (!atomic_load(&alarm_on)) break;     /* switched off: no pause to wait out */
        }
        pthread_mutex_unlock(&ear_lock);
    }
    return NULL;
}
