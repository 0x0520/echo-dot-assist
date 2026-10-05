/* The Echo's own settings through its stock tools: LED ring, volume (Echo or Bluetooth speaker), equalizer, LED
 * brightness, light sensor. */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include "a2dp.h"
#include "board.h"
#include "core_int.h"
#include "sendspin.h"

static int use_led = 1, use_volume = 1;            /* set before the threads start */

void hw_init(int led_on, int volume_keys)
{
    use_led = led_on && !access("/system/bin/ledctrl", X_OK);
    use_volume = volume_keys;
}

/* ---------------------------------------------------------------- LED ring */

void led(const char *op, const char *pattern)
{
    if (use_led && pattern) run("/system/bin/ledctrl", op, pattern);
}

void led_for(enum state from, enum state to)
{
    static const char *const pattern[] = { NULL, "ca-active-start", "active-thinking", "active-talking" };
    if (pattern[from]) led("-u", pattern[from]);
    if (pattern[to]) led("-s", pattern[to]);
    else if (from != IDLE) led("-s", "ca-active-end");
}

/* Do not disturb (core_dnd): switching it on shows Amazon's single purple pulse (do_not_disturb: 2 s fade in and out,
 * layer 2, nothing after its `loop` marker); switching it off shows nothing. */
static atomic_llong dnd_clear_at;
void led_dnd_pulse(void) { led("-s", "do_not_disturb"); atomic_store(&dnd_clear_at, mono_ms() + 2500); }

/* ---------------------------------------------------------------- volume */

/* Volume: 10 % per press like stock (3 of the ring's 30 steps on donut, board.volume_steps).  The volume_step-NN animations show 2 s and then loop
 * black forever, so the previous one has to be unset or they pile up in ledcontroller; a timer clears the last one. */
static int volume = -1;                      /* under lock: 0..100, read from the device on first use */
static char vol_pat[24];                     /* under lock */
static atomic_llong vol_clear_at;

static int read_prop_volume(const char *prop, int fallback)
{
    char out[1024], *line, *save; int v = fallback, x;
    char *argv[] = { "/system/bin/audio_manager_get_prop", (char *)prop, NULL };
    run_output(argv, out, sizeof out);
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (sscanf(line, "%d", &x) == 1 && x >= 0 && x <= 100) v = x;
    return v;
}

static int read_volume(void) { return read_prop_volume("MainVolume", 40); }

/* The mixer keeps one volume per stream type.  MainVolume is what the stock volume keys move and covers the Music and Earcon
 * streams; the TTS stream, which carries the assistant's replies, follows TTSVolume alone.  One knob for the user: both. */
static void set_prop_volume(const char *prop, int v)
{
    char val[8];
    snprintf(val, sizeof val, "%d", v);
    run("/system/bin/audio_manager_set_prop", prop, val);
}

/* lock held: the client, Music Assistant and a Bluetooth speaker hear of a new volume */
static void volume_tell(void)
{
    const struct proto *p = core_client();
    if (p && p->volume_changed) p->volume_changed(volume);
    if (core_sendspin_port) sendspin_volume_changed(volume);
    a2dp_volume_changed(volume);
}

/* Playing to a Bluetooth speaker (a2dp.c, btout.c) it has a volume of its own: what the buttons and Home Assistant move
 * meanwhile, starting from the speaker's when it tells (AVRCP absolute volume); the Echo's own comes back afterwards.
 * With absolute volume the speaker applies it and the mixer plays at full scale, so SBC gets the whole signal (at volume
 * 30 the mixer's curve, made for the Echo's small speaker, put it 38 dB down); without, the mixer applies it as before.
 * The Echo's own volume is kept in a file meanwhile, so that a hassmic that dies on the speaker does not take what the
 * mixer then says for it and play the Echo's speaker at the Bluetooth speaker's level, or at full scale. */
static int speaker_mode;                     /* under lock: SPEAKER_* */
static int echo_own_volume;                  /* under lock: the Echo's volume, set aside while speaker_mode */

static const char *own_volume_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume.speaker", d ? d : "/data/local/hassmic/state");
    return p;
}

int core_volume(void)
{
    if (volume < 0) {                        /* whatever it was left at: in line now */
        FILE *f = fopen(own_volume_path(), "r"); int v;
        if (f && fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) {
            volume = v; set_prop_volume("MainVolume", volume);
            fprintf(stderr, "volume: %d (the Echo's own, set aside for a Bluetooth speaker)\n", volume);
        } else volume = read_volume();
        if (f) fclose(f);
        unlink(own_volume_path());
        set_prop_volume("TTSVolume", volume);
    }
    return volume;
}

void core_speaker(int mode, int pct)
{
    pthread_mutex_lock(&core_lock);
    int was = core_volume();
    if (mode == speaker_mode) { pthread_mutex_unlock(&core_lock); return; }
    if (!speaker_mode) {
        echo_own_volume = was;
        FILE *f = fopen(own_volume_path(), "w"); if (f) { fprintf(f, "%d\n", was); fclose(f); }
    }
    if (mode == SPEAKER_NONE) volume = echo_own_volume;
    else if (mode == SPEAKER_ABSOLUTE) volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    speaker_mode = mode;
    int mix = mode == SPEAKER_ABSOLUTE ? 100 : volume;
    set_prop_volume("MainVolume", mix); set_prop_volume("TTSVolume", mix);
    if (mode == SPEAKER_NONE) unlink(own_volume_path());
    fprintf(stderr, "volume: %d, mixer at %d (%s)\n", volume, mix, mode == SPEAKER_NONE ? "the Echo's speaker" :
            mode == SPEAKER_ABSOLUTE ? "a Bluetooth speaker sets the volume" : "a Bluetooth speaker, the mixer sets the volume");
    if (volume != was) volume_tell();
    pthread_mutex_unlock(&core_lock);
}

void core_set_volume(int v)
{
    char pat[24]; int step;
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    step = volume * board.volume_steps / 100 ? volume * board.volume_steps / 100 : 1;
    snprintf(pat, sizeof pat, "volume_step-%02d", step);
    if (speaker_mode != SPEAKER_ABSOLUTE) { set_prop_volume("MainVolume", volume); set_prop_volume("TTSVolume", volume); }
    if (vol_pat[0] && strcmp(vol_pat, pat)) led("-u", vol_pat);
    led("-s", pat);
    strcpy(vol_pat, pat);
    atomic_store(&vol_clear_at, mono_ms() + 2500);
    fprintf(stderr, "volume: %d\n", volume);
    volume_tell();
}

/* Anything may move MainVolume behind our back (audio_manager_set_prop, a stock daemon, the stock keys when -V), and
 * TTSVolume does not follow by itself: replies would then play at the old volume.  Poll both every 2 s: a changed
 * MainVolume is the user's wish and is adopted (Home Assistant and Music Assistant are told, no LED), a strayed TTSVolume
 * is pulled back in line.
 * The mixer's global Mute silences every stream whatever the volumes say, and it persists across reboots: stock Alexa
 * ("Alexa, mute") can leave it set, and then nothing plays.  Nothing of ours uses it (the mic button is a hardware latch,
 * a player mute from Music Assistant is ours in software), so a set Mute is cleared.
 * Each read is a process (audio_manager_get_prop): three every 2 s, for ever, was most of what hassmic forked.  Now
 * MainVolume, the one people move, every 2 s; Mute and TTSVolume, which only a stock daemon moves, once a minute
 * (full), and TTSVolume is set without asking whenever MainVolume moved. */
static void volume_sync(int full)
{
    int main_v, tts_v = -1;
    if (full && read_prop_volume("Mute", 0) != 0) { fprintf(stderr, "speaker: global Mute was set, clearing it\n"); set_prop_volume("Mute", 0); }
    pthread_mutex_lock(&core_lock);
    int cur = core_volume();
    pthread_mutex_unlock(&core_lock);
    main_v = read_prop_volume("MainVolume", cur);
    if (full) tts_v = read_prop_volume("TTSVolume", -1);
    pthread_mutex_lock(&core_lock);
    if (speaker_mode == SPEAKER_ABSOLUTE) {                         /* 100 is ours: only a stray TTSVolume to mend */
        if (main_v == 100 && tts_v >= 0 && tts_v != 100) set_prop_volume("TTSVolume", 100);
    } else if (volume == cur) {                                     /* nobody set it meanwhile */
        if (main_v != cur) {
            volume = main_v;
            fprintf(stderr, "volume: %d (changed outside)\n", volume);
            volume_tell();
            set_prop_volume("TTSVolume", volume);
        } else if (tts_v >= 0 && tts_v != volume) set_prop_volume("TTSVolume", volume);
    }
    pthread_mutex_unlock(&core_lock);
}

void on_volume(int dir)
{
    if (!use_volume) return;
    pthread_mutex_lock(&core_lock);
    core_set_volume((core_volume() + 5) / 10 * 10 + dir * 10);
    sound_request(SND_VOLUME);
    pthread_mutex_unlock(&core_lock);
}

void *volume_led_thread(void *arg)
{
    int tick = 0;
    (void)arg;
    while (!core_quitting()) {
        if (tick++ % 10 == 0) volume_sync(tick % 300 == 1);         /* 2 s; full every minute, first at start */
        long long at = atomic_load(&vol_clear_at);
        if (at && mono_ms() >= at) {
            pthread_mutex_lock(&core_lock);
            if (atomic_load(&vol_clear_at) == at) { led("-u", vol_pat); vol_pat[0] = 0; atomic_store(&vol_clear_at, 0); }
            pthread_mutex_unlock(&core_lock);
        }
        at = atomic_load(&dnd_clear_at);                /* played out; unset it like the volume steps, so the next pulse starts clean */
        if (at && mono_ms() >= at && atomic_compare_exchange_strong(&dnd_clear_at, &at, 0)) {
            pthread_mutex_lock(&core_lock); led("-u", "do_not_disturb"); pthread_mutex_unlock(&core_lock);
        }
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- equalizer */

/* Speaker equalizer: the mixer's own user EQ (libasp "ASP/UserEq"), which stock set from the Alexa app through PuffinApp.
 * LIPC com.doppler.lasp takes the three bands as JSON, clamps each to -6..+6 (dB steps), applies them to everything the
 * mixer plays (music, replies and sounds alike, on the 3.5 mm jack too) and keeps them across reboots in
 * /data/misc/audio/audioCtrl.cfg.  So no state file of ours: read from the mixer once, then cached. */
static const char *const eq_names[3] = { "BASS", "MIDRANGE", "TREBLE" };
static int eq[3], eq_read;                          /* under lock */

int core_eq(int band)
{
    if (!eq_read) {
        char out[256], key[32], *s; int x;
        char *argv[] = { "/system/bin/lipc-get-prop", "-s", "com.doppler.lasp", "LASP_CMD_GET_USER_EQ_INFO", NULL };
        run_output(argv, out, sizeof out);          /* {"bands":[{"name":"BASS","level":0},{"name":"MIDRANGE",... */
        for (int i = 0; i < 3; i++) {
            snprintf(key, sizeof key, "\"%s\",\"level\":", eq_names[i]);
            if ((s = strstr(out, key)) && sscanf(s + strlen(key), "%d", &x) == 1) eq[i] = x < -6 ? -6 : x > 6 ? 6 : x;
        }
        eq_read = 1;
    }
    return eq[band];
}

void core_set_eq(int band, int db)
{
    char json[160];
    core_eq(band);                                  /* the other two bands as the mixer has them */
    eq[band] = db < -6 ? -6 : db > 6 ? 6 : db;
    snprintf(json, sizeof json, "{\"bands\":[{\"name\":\"%s\",\"level\":%d},{\"name\":\"%s\",\"level\":%d},{\"name\":\"%s\",\"level\":%d}]}",
             eq_names[0], eq[0], eq_names[1], eq[1], eq_names[2], eq[2]);
    char *argv[] = { "/system/bin/lipc-set-prop", "-s", "com.doppler.lasp", "LASP_CMD_SET_USER_EQ_INFO", json, NULL };
    run_argv(argv);
    fprintf(stderr, "equalizer: bass %d, mid %d, treble %d\n", eq[0], eq[1], eq[2]);
}

/* ---------------------------------------------------------------- LED brightness, light sensor */

/* LED brightness.  Stock's auto brightness is ledcontroller's, not the Alexa client's, and it starts it by itself at boot
 * (Echo Dot 2 set to 50 with auto off and rebooted: 9 again, for 40 lux; 2026-10-01).  It polls the light sensor through
 * the same HAL file as core_lux() (1 Hz when settled, 20 Hz while moving), smooths it over 3 s and maps 0..400 lux on a
 * straight line to 0..100 (donut: 0.26 per lux - 4, at least 0), ramping there in 3 s.  So "auto" here is ledcontroller
 * left alone, and a fixed level is ledctrl -a off -b N in one call: two calls could land in either order, and the running
 * engine would overwrite a level that came first.  Neither ledcontroller nor anything stock keeps the auto flag: our
 * settings file does (proto_esphome.c), and it is applied again at every start.  ledcontroller writes each level it shows
 * to persist.ledbrightness.bootup (auto steps too) and restores it at boot, so that property is what the ring shows. */
static int led_auto = 1, led_level = 80;            /* under lock.  80: ledcontroller's first-boot level */

int core_led_auto(int set)
{
    if (set >= 0 && set != led_auto) {
        if (!set) led_level = core_led_brightness(-1);
        led_auto = set;
        if (use_led) run("/system/bin/ledctrl", "-a", set ? "on" : "off");     /* off: the level stays where auto left it */
        fprintf(stderr, "LED brightness: %s\n", set ? "auto" : "fixed");
    }
    return led_auto;
}

int core_led_brightness(int set)
{
    if (set >= 0) {
        char n[8]; snprintf(n, sizeof n, "%d", set > 100 ? 100 : set);
        char *argv[] = { "/system/bin/ledctrl", "-a", "off", "-b", n, NULL };
        if (use_led) run_argv(argv);
        led_auto = 0; led_level = atoi(n);
        fprintf(stderr, "LED brightness: fixed at %d\n", led_level);
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";                   /* fixed: ours, the property may not have it yet (ledctrl runs apart) */
    if (led_auto && use_led && __system_property_get("persist.ledbrightness.bootup", v) > 0) return atoi(v);
#endif
    return led_level;
}

/* The light sensor as stock's HAL (libacehal_ambientLightSensor.so, its per-model "facade") reads it: a sysfs file the
 * kernel driver fills with calibrated lux, parsed with atof.  0..400 is all stock uses of it. */
float core_lux(void)
{
    const char *e = getenv("HASSMIC_LUX");         /* tests */
    const char *const *p = e ? (const char *const[]){ e, NULL } : board.light_sensor;
    for (; p && *p; p++) {
        char buf[32]; int fd = open(*p, O_RDONLY); ssize_t n;
        if (fd < 0) continue;
        n = read(fd, buf, sizeof buf - 1); close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        return (float)atof(buf);
    }
    return NAN;
}
