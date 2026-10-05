/* Sound detection and whisper detection on the mic stream, beside the wake word; fed by the capture thread. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "core_int.h"
#include "sound.h"
#include "whisper.h"

/* ---------------------------------------------------------------- sound detection
 * The stock detector (sound.h, docs/re-aed.md), off unless Home Assistant switches it on: then a second decoder runs on
 * the mic stream beside the wake word.  Home Assistant gets the types the model can tell apart.  On every test window
 * smokeAlarm, smokeSiren and carbonMonoxideSiren scored the same, and so did cough and runningWater: one event each.
 * humanPresence is left out: it fires on any talk, TV, knock or alarm clock, every window while someone is about.
 * Stock checks each hit in Amazon's cloud before anyone is told; nothing here can, so a window in which the Echo itself
 * made sound is dropped: echo cancellation leaves enough of a timer ringing to pass for a beeping appliance. */
static const struct { const char *amazon, *event; } sound_map[] = {
    { "smokeAlarm", "smoke_or_co_alarm" }, { "smokeSiren", "smoke_or_co_alarm" }, { "carbonMonoxideSiren", "smoke_or_co_alarm" },
    { "glassBreak", "glass_break" }, { "dogBark", "dog_bark" }, { "babyCry", "baby_cry" }, { "snore", "snoring" },
    { "cough", "cough" }, { "waterSounds", "water" }, { "beepingAppliance", "beeping_appliance" },
};
#define SOUND_MAP (int)(sizeof sound_map / sizeof sound_map[0])
const char *const core_sound_events[] = { "smoke_or_co_alarm", "glass_break", "dog_bark", "baby_cry", "snoring", "cough",
                                          "water", "beeping_appliance" };
const int core_sound_nevents = sizeof core_sound_events / sizeof core_sound_events[0];
#define SOUND_WINDOW_MS 11000               /* a scoring window (9.98 s) and the decoder's lag behind it */
static atomic_int sound_want;               /* the switch; the capture thread opens and closes the decoder to match */
static int sound_running;                   /* capture thread: the decoder is open; only it opens, feeds and closes it */

int core_sound(int set)
{
    if (set >= 0 && set != atomic_load(&sound_want)) {
        atomic_store(&sound_want, set);
        fprintf(stderr, "sound detection: switched %s\n", set ? "on" : "off");
    }
    return atomic_load(&sound_want);
}

static void on_sound(const char *const *types, int n)    /* detector thread */
{
    if (own_sound_within(SOUND_WINDOW_MS)) {
        fprintf(stderr, "sound: dropped, the Echo played something in that window\n");
        return;
    }
    const char *sent[SOUND_MAP]; int ns = 0;
    pthread_mutex_lock(&core_lock);
    const struct proto *p = core_client();
    if (atomic_load(&sound_want) && !core_muted() && p && p->sound)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < SOUND_MAP; j++) {
                if (strcmp(types[i], sound_map[j].amazon)) continue;
                int dup = 0;
                for (int k = 0; k < ns; k++) dup |= sent[k] == sound_map[j].event;
                if (!dup) { sent[ns++] = sound_map[j].event; p->sound(sound_map[j].event); }
            }
    pthread_mutex_unlock(&core_lock);
}

/* ---------------------------------------------------------------- whisper detection
 * Whisper detection (whisper.h, docs/re-whisper.md): each request's mic audio, from the start of streaming to Home
 * Assistant's VAD end, is scored once; the binary sensor says whether the last one was whispered, for the conversation
 * agent's prompt template.  The result comes within milliseconds of the end of speech, while speech to text still
 * runs, so it is in Home Assistant before the agent's prompt is rendered.  Without the DAVS model there is no sensor. */
static atomic_int whisper_eou;              /* core_mic_off: the end of speech came (not a cancel or timeout) */
static atomic_int whisper_last = -2;        /* core_whispered */
static int whisper_on;                      /* capture thread: a whisper detector takes this request's audio */

int core_whispered(void) { return atomic_load(&whisper_last); }
void whisper_mark_eou(void) { atomic_store(&whisper_eou, 1); }

static void on_whisper(int whispered, int confidence, int threshold)    /* detector thread */
{
    fprintf(stderr, "whisper: %s (confidence %d, threshold %d)\n", whispered ? "whispered" : "not whispered", confidence, threshold);
    atomic_store(&whisper_last, whispered);
    pthread_mutex_lock(&core_lock);
    const struct proto *p = core_client();
    if (p && p->whispered) p->whispered(whispered);
    pthread_mutex_unlock(&core_lock);
}

void detect_init(void)
{
    if (whisper_open(on_whisper) == 0) atomic_store(&whisper_last, -1);
    else fprintf(stderr, "whisper: no model, no whisper detection (scripts/artifacts.sh installs it)\n");
}

void detect_feed(const void *pcm, int bytes, long sample)
{
    if (atomic_load(&sound_want) != sound_running) {           /* Home Assistant switched sound detection */
        if (!sound_running) {
            const char *types[SOUND_MAP]; for (int i = 0; i < SOUND_MAP; i++) types[i] = sound_map[i].amazon;
            if (sound_open(types, SOUND_MAP, on_sound) == 0) sound_running = 1;
            else { fprintf(stderr, "sound detection: cannot start\n"); atomic_store(&sound_want, 0); }
        } else { sound_close(); sound_running = 0; }
    }
    if (sound_running) sound_feed(pcm, bytes / 2);
    if (atomic_load(&whisper_last) != -2) {     /* a model: what streams to the pipeline is one request */
        int s = mic_streaming();
        if (s && !whisper_on) {             /* the position lines it up with the capture dump's */
            atomic_store(&whisper_eou, 0); whisper_begin(); whisper_on = 1;
            fprintf(stderr, "whisper: request from capture sample %ld\n", sample);
        }
        if (whisper_on && s) whisper_feed(pcm, bytes / 2);
        if (whisper_on && !s) { whisper_end(atomic_exchange(&whisper_eou, 0)); whisper_on = 0; }
    }
}
