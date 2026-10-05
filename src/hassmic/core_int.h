/* Inside the satellite core: what its files (main.c and the ones below) share and the protocols do not see (core.h).
 * Each function says which lock it needs, as core.h does; nothing here takes core_lock unless it says so. */
#ifndef CORE_INT_H
#define CORE_INT_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "core.h"
#include "sounds.h"

static inline long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* main.c: state machine, pipeline, mute, buttons, capture thread */
const struct proto *core_proto(void);           /* no lock: set before any thread starts */
const struct proto *core_client(void);          /* lock held: the protocol while a client is connected, else NULL */
int  core_quitting(void);                       /* no lock: the capture loop gave up, threads end */
void trigger(int touch);                        /* takes core_lock.  touch: the action button rather than the wake word */
void stop_word(void);                           /* takes core_lock: the "stop" keyword */
int  satellite_ready(void);                     /* lock held: connected and the server wants pipelines */

/* spawn.c: the stock tools, from any thread (several under core_lock: they vfork on the Echo) */
void run_argv(char *const argv[]);              /* fire and forget, stdout to /dev/null */
void run(const char *path, const char *a1, const char *a2);
void run_output(char *const argv[], char *buf, size_t n);   /* its stdout, killed after 1 s */

/* hwsettings.c: LED ring, volume, speaker, equalizer, LED brightness */
void hw_init(int led, int volume_keys);         /* main() before any thread: -L, -V; no ledctrl on the image = no ring */
void led(const char *op, const char *pattern);  /* lock held (or before the threads start) */
void led_for(enum state from, enum state to);   /* lock held */
void led_dnd_pulse(void);                       /* lock held: do not disturb's purple pulse, unset once played out */
void on_volume(int dir);                        /* takes core_lock: a volume button */
void *volume_led_thread(void *arg);

/* earcon.c: our own sounds (earcons, the alarm) and what of ours plays */
void sound_queue(enum sound s);                 /* any thread: played by the earcon thread */
void sound_request(enum sound s);               /* the same unless -E or Home Assistant's switch turned the sounds off */
void sound_unqueue_wake(void);                  /* any thread: a wake or touch sound asked for is not played after all */
int  alarm_ringing(void);                       /* any thread: a timer or the alarm clock rings */
void alarm_ring(int seconds);                   /* any thread: as core_alarm(1), for up to that long (alarms.c) */
void own_tts(int on);                           /* playback thread: a reply plays or stopped playing */
int  own_sound_within(long long ms);            /* any thread: something of ours plays or did within the last ms */
int  own_sound_hold(void);                      /* any thread: one of our sounds plays or is still in the mic stream */
void *earcon_thread(void *arg);

/* playback.c: the TTS queue */
void tts_cut(void);                             /* any thread: drop what is queued or playing; nothing when nothing is */
void tts_pending_spent(void);                   /* lock held: a cut kept for a stream that did not come is over */
void *playback_thread(void *arg);

/* mic.c: mic audio to the pipeline */
int  mic_streaming(void);                       /* any thread, a snapshot */
int  mic_stream_end(void);                      /* any thread: stop streaming; whether it streamed */
void mic_pipeline_new(void);                    /* lock held: a pipeline starts, before proto->start() */
void mic_stream_on(void);                       /* lock held: stream to it, and tell the front end a command is spoken */
void mic_stopped(void);                         /* lock held: the mic stream to the pipeline has stopped */
void mic_keyword(float db);                     /* lock held: the wake word was just heard at that level */
long long mic_keyword_ms(void);                 /* lock held: when (mono_ms) */
void mic_init(void);                            /* main() before any thread */
void mic_queue(const int16_t *pcm, size_t n, uint64_t at);  /* capture thread; at: ring index of pcm[0], 0: none */
void mic_queue_ring(uint64_t from);             /* capture thread: what the ring has from there on */
void listening(int on);                         /* lock held: the front end's utterance state (FINDINGS.md "Listening mode") */
void *mic_sender(void *arg);

/* wakewords.c: the wake word models; the capture thread loads and feeds them */
int  wake_words_init(const char *m_arg);        /* main() before any thread: 0 when no model loads */
void wake_words_poll(void);                     /* capture thread: Home Assistant's pick, a reset asked for */
void wake_words_feed(const int16_t *pcm, size_t n);         /* capture thread */
void wake_words_reset(void);                    /* any thread: wake_reset() before the next block */
void wake_word_active_name(char *out, size_t n);            /* lock held */
void wake_words_close(void);

/* wakedet.c: detections, the ring buffer of what the engine heard, the score, arbitration */
void on_wake(const char *keyword, uint64_t begin, uint64_t end);   /* detector thread (wake_cb) */
void wakedet_feed(const int16_t *pcm, size_t n);            /* capture thread: into the ring */
void wakedet_poll(void);                        /* capture thread: act on detections and decided arbitration rounds */
void wakedet_simulate(void);                    /* capture thread: SIGUSR1, a wake word in the last 0.6 s */
uint64_t ring_samples(void);                    /* capture thread: samples fed so far */
const int16_t *ring_chunk(uint64_t *from, uint64_t to, size_t *n);  /* capture thread: see wakedet.c */
void afe_done(void);                            /* any thread: the front end's utterance state is over */
int  wakedet_arb_start(int port);               /* main() before any thread: arb_start(); 0 when running */

/* detect.c: sound and whisper detection */
void detect_init(void);                         /* main() before any thread: the whisper model if there is one */
void detect_feed(const void *pcm, int bytes, long sample);  /* capture thread; sample: capture position of pcm */
void whisper_mark_eou(void);                    /* any thread: the end of speech came (not a cancel or timeout) */
#endif
