/*
 * hassmic - Home Assistant voice satellite for Amazon Echo devices (first: Echo Dot 3, donut; see devices/).
 *
 * Replaces PuffinApp as the client of Amazon's `mixer` daemon: reads the post-AEC/beamformer stream,
 * runs the stock "Alexa" wake word locally, and speaks the ESPHome native API or Wyoming to Home Assistant.
 *
 *   hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m pryon.manifest] [-b input-device] [-L] [-E] [-V] [-S]
 *     -o port  push update port (default 28929, 0 = off; see scripts/ota-push.sh)
 *     -a port  wake word arbitration between Echos, UDP (default 28930, 0 = off; see arb.c)
 *     -z port  Sendspin player port (default 28928, 0 = off)
 *     -T       print the Sendspin pairing token (paste it into Music Assistant to pair) and exit
 *     -L no LED ring   -E no earcon on wake   -V leave the volume buttons alone   -S print the avahi service file and exit
 *
 * Default ports 26053 (ESPHome) and 16700 (Wyoming): the stock firewall only admits inbound TCP 16384-32767.
 *
 * This file is the satellite core: the state machine, the pipeline, mute, buttons and the capture thread.  The rest of
 * the core is in the files core_int.h lists (playback.c, mic.c, wakewords.c, wakedet.c, earcon.c, hwsettings.c, ...).
 */
#include <ctype.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "a2dp.h"
#include "arb.h"
#include "buttons.h"
#include "netio.h"
#include "core_int.h"
#include "ota.h"
#include "sendspin.h"
#include "board.h"

#define PIPELINE_TIMEOUT_MS 30000   /* in LISTENING or THINKING before giving up */

static const char *const state_names[] = { "idle", "listening", "thinking", "speaking" };
static int use_bt_announce = 1, use_bt = 1;

const char *core_name;                  /* -n, else board.default_name */

/* The name is UTF-8.  Latin-1 letters (U+00C0..U+00FF, lead byte 0xC3) are spelled out, the German way for the umlauts
 * ("Küchen Echo" -> "kuechen-echo"); anything else that is not a letter or digit separates words.  Each byte of "ü"
 * used to become a dash ("k--chen-echo"), which Home Assistant showed as the host name next to the friendly name. */
static const char *latin1_ascii(unsigned char c)        /* second byte after 0xC3, upper and lower case alike */
{
    static const char *const t[32] = {
        "a", "a", "a", "a", "ae", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",        /* À..Ï */
        "d", "n", "o", "o", "o", "o", "oe", NULL, "o", "u", "u", "u", "ue", "y", "th", "ss" };    /* Ð..ß (× is no letter) */
    if (c == 0xBF) return "y";                                                                  /* ÿ */
    if (c == 0xB7) return NULL;                                                                 /* ÷ */
    return c >= 0x80 && c <= 0xBF ? t[(c - 0x80) & 0x1F] : NULL;
}

const char *core_node_name(void)
{
    static char n[64]; size_t j = 0; int dash = 0;
    for (const unsigned char *s = (const unsigned char *)core_name; *s; s++) {
        const char *add = NULL; char one[2] = { 0, 0 };
        if (isalnum(*s)) { one[0] = (char)tolower(*s); add = one; }
        else if (*s == 0xC3 && s[1]) add = latin1_ascii(*++s);
        else while ((s[1] & 0xC0) == 0x80) s++;          /* other UTF-8 characters: skip their continuation bytes */
        if (!add) { dash = j > 0; continue; }            /* separators collapse to one dash, none at the start */
        if (dash && j < sizeof n - 1) n[j++] = '-';
        dash = 0;
        for (; *add && j < sizeof n - 1; add++) n[j++] = *add;
    }
    n[j] = 0;
    if (!j) snprintf(n, sizeof n, "echo");              /* a name with no Latin letter at all ("日本") */
    return n;
}
static int ota_port = 28929;                        /* 0 = no push updates */
static int arb_port = 28930;                        /* 0 = no wake word arbitration */
int core_local_wake = 1, core_port, core_sendspin_port = 28928;       /* 0 = Sendspin off */
static const struct proto *proto = &proto_esphome;

pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static int connected, satellite_running;
static _Atomic(enum state) state;              /* changed under core_lock; the audio threads peek without it (core.h) */
/* The capture thread checks the pipeline timeout on every block without core_lock (a write to a stalled client may hold
 * it for seconds): a copy of the state and since when, monotonic, written with it */
static atomic_int state_seen;
static atomic_llong state_since_ms;
static atomic_int trigger_pending, button_pending, stop_pending, quit;
static atomic_int dump_toggle;                      /* SIGTTIN: start / stop writing the processed mic stream to a file */
static int soft_mute;                               /* under lock: mute switch from Home Assistant */
static int dnd;                                     /* under lock: do not disturb */
static int barge_in;                                /* under lock: start a new pipeline once the current one has ended
                                                      * (wake word during a reply, or the server asked to continue the conversation) */

const struct proto *core_proto(void) { return proto; }
const struct proto *core_client(void) { return connected ? proto : NULL; }
int core_quitting(void) { return atomic_load(&quit); }
int satellite_ready(void) { return connected && satellite_running; }

/* ---------------------------------------------------------------- state (call with lock held) */

enum state core_state(void) { return state; }

void core_set_state(enum state s)
{
    if (s == state) return;
    fprintf(stderr, "state: %s -> %s\n", state_names[state], state_names[s]);
    led_for(state, s);
    state = s;
    atomic_store(&state_since_ms, mono_ms()); atomic_store(&state_seen, s);
}

static long long wake_cut_ms;    /* when the wake word last cut a reply or an alarm: a "stop" right behind it belongs to that */
static int quiet_abort;

static void pipeline_start(void)
{
    quiet_abort = 0;
    mic_pipeline_new();
    proto->start();
    mic_stream_on();
    if (core_local_wake) core_set_state(LISTENING);
}

int core_bt_announce(int set) { if (set >= 0) use_bt_announce = set; return use_bt_announce; }

/* Models whose controller does not answer to our bring-up yet (biscuit's MT8163 combo) run with -B: no A2DP sink, no
 * Bluetooth proxy; the stock stack may keep the radio. */
int core_bluetooth(int set) { if (set >= 0) use_bt = set; return use_bt; }

/* Stock Alexa played its Bluetooth chime and said "Now connected to <name>".  The chime is on the image; there is no TTS
 * engine on it, so the words come from Home Assistant (the protocol asks it) and only while it is connected. */
void core_bt_device(const char *name, int on)
{
    pthread_mutex_lock(&core_lock);
    if (use_bt_announce) {
        sound_queue(on ? SND_BT_ON : SND_BT_OFF);
        if (connected && proto->bt_device) proto->bt_device(name, on);
    }
    pthread_mutex_unlock(&core_lock);
}

/* While the Echo is discoverable the ring runs Amazon's blue device search chaser: `scone-setup` (stock's
 * "discovery-in-progress"), frame for frame the same as `btpair-setup` but listed in layer_config_common.json (layer 4,
 * below listening/thinking/talking, so a voice command still shows on top).  It loops until unset. */
void core_bt_pairing(int on)
{
    pthread_mutex_lock(&core_lock);
    led(on ? "-s" : "-u", "scone-setup");
    pthread_mutex_unlock(&core_lock);
}

/* Do not disturb, like stock: announcements from Home Assistant are dropped, while the wake word, replies, timers, music
 * and Bluetooth connection messages carry on.  Switching it on shows Amazon's purple pulse (led_dnd_pulse). */
int core_dnd(int set)
{
    if (set >= 0 && set != dnd) {
        dnd = set;
        fprintf(stderr, "do not disturb: %d\n", set);
        if (set && satellite_running) led_dnd_pulse();     /* quiet when restored at start */
    }
    return dnd;
}

int core_muted(void) { return soft_mute || buttons_muted(); }

static void mute_update(int was, int sound)   /* lock held: ring, sound + Home Assistant follow the effective state */
{
    int now = core_muted();
    if (now == was) return;
    if (sound) sound_request(now ? SND_MICS_OFF : SND_MICS_ON);
    if (now) { if (state == LISTENING) core_pipeline_finish(); led("-s", "mics-off_on"); }
    else { led("-u", "mics-off_on"); led("-s", "mics-off_end"); }
    if (connected && proto->mute_changed) proto->mute_changed(now);
}

int core_soft_mute(int set)
{
    if (set >= 0 && set != soft_mute) {
        int was = core_muted();
        soft_mute = set;
        fprintf(stderr, "soft mute: %d%s\n", set, !set && buttons_muted() ? " (hardware latch still on: only the button releases it)" : "");
        mute_update(was, satellite_running);            /* quiet when the saved setting is restored at start */
        if (!set && buttons_muted() && connected && proto->mute_changed) proto->mute_changed(1);   /* switch bounces back */
    }
    return soft_mute;
}

void core_restart_after(void) { barge_in = 1; }

void core_pipeline_finish(void)
{
    tts_pending_spent();
    if (mic_stream_end() && connected && proto->stop) proto->stop();
    mic_stopped();
    core_set_state(IDLE);
    /* A reset puts the engine back to sleep: "<wake word>, stop" would lose its "stop".  Done by the capture thread, which
     * feeds the decoder: this runs on any thread */
    if (mono_ms() - mic_keyword_ms() > 3000) wake_words_reset();
    if (!connected) barge_in = 0;
    if ((barge_in || !core_local_wake) && satellite_running && connected) {
        if (barge_in) sound_request(SND_WAKE);
        barge_in = 0;
        pipeline_start();
    }
}

void core_link(int up, int ready)
{
    int was = satellite_running;
    connected = up; satellite_running = up && ready;
    if (!satellite_running) core_pipeline_finish();
    else if (!was && !core_local_wake && state == IDLE) pipeline_start();
}

void core_error(void)
{
    if (quiet_abort) { quiet_abort = 0; return; }      /* Home Assistant's complaint about a pipeline "stop" ended: not news */
    if (state != SPEAKING) { led("-s", "anim_start_error_short"); core_pipeline_finish(); }
}

void trigger(int touch)
{
    pthread_mutex_lock(&core_lock);
    if (alarm_ringing()) {
        core_alarm(0);
        wake_cut_ms = mono_ms();
    } else if ((state == THINKING || (touch && state == LISTENING)) && connected && proto->cancel) {
        /* As the center button of a Voice PE: a misheard command is stopped before its tool calls run, not only its
         * reply.  Same message as ESPHome's voice_assistant.stop; Home Assistant cancels the run's task (the LLM, and
         * the tool calls it has not made yet).  The wake word does the same and then listens again, as during a reply */
        fprintf(stderr, "%s: pipeline cancelled\n", touch ? "button" : "wake word");
        barge_in = 0;
        mic_stream_end();
        proto->cancel();
        core_pipeline_finish();
        quiet_abort = 1;
        if (!touch && state == IDLE && satellite_running && !core_muted()) {
            wake_cut_ms = mono_ms();
            sound_request(SND_WAKE);
            pipeline_start();
        }
    } else if (!connected || !satellite_running || core_muted()) {
        /* nothing to talk to, or privacy latch on */
    } else if (state == IDLE) {
        sound_request(touch ? SND_TOUCH : SND_WAKE);
        pipeline_start();
    } else if (state == SPEAKING && !core_tts_flushing()) {                /* not already being cut.  barge_in alone does not
                                                                             * say that: continue-conversation sets it too */
        fprintf(stderr, "barge-in\n");
        wake_cut_ms = mono_ms();
        barge_in = 1;
        tts_cut();                          /* playback thread drops TTS up to audio-stop, then we restart */
    }
    pthread_mutex_unlock(&core_lock);
}

/* "Stop", the second keyword of Amazon's wake word models.  The engine reports it only right behind the wake word
 * ("<wake word>, stop"; on its own it is never detected, also not with a changed op.cfg.json: tried).
 * It ends what is making noise and never starts anything:
 * a ringing alarm, a reply being spoken (also one that would listen again afterwards).  Said as "<wake word>, stop", the wake
 * word has already cut the reply and opened a new pipeline by the time "stop" is recognised: that pipeline is dropped again.
 * Only then: "<wake word>, stop the music" out of silence is a command for Home Assistant, not for us. */
void stop_word(void)
{
    pthread_mutex_lock(&core_lock);
    if (alarm_ringing()) {
        core_alarm(0);
    } else if (state == SPEAKING) {
        fprintf(stderr, "stop: reply cut\n");
        barge_in = 0;
        sound_unqueue_wake();
        tts_cut();
    } else if (state == LISTENING && mono_ms() - wake_cut_ms < 4000) {
        fprintf(stderr, "stop: pipeline dropped\n");
        sound_unqueue_wake();
        barge_in = 0;
        core_pipeline_finish();
        quiet_abort = 1;
    }
    pthread_mutex_unlock(&core_lock);
}

/* ---------------------------------------------------------------- buttons */

/* Action button: cancel a running pipeline; else pause what plays (Bluetooth device first, then Sendspin), or resume
 * what the button paused; else talk */
static void on_action(void)
{
    pthread_mutex_lock(&core_lock);
    int busy = state == LISTENING || state == THINKING;
    pthread_mutex_unlock(&core_lock);
    if (!busy && (a2dp_button(0) || (core_sendspin_port && sendspin_button()) || a2dp_button(1))) return;
    atomic_store(&trigger_pending, 2);                  /* 2: touch */
}

static void on_mute(int muted)          /* hardware latch changed (button) */
{
    static int last = 0;
    fprintf(stderr, "mic mute button: %d\n", muted);
    pthread_mutex_lock(&core_lock);
    int was = last || soft_mute; last = muted;
    if (!muted && soft_mute) { soft_mute = 0; fprintf(stderr, "soft mute: 0 (released with the button)\n"); }
    mute_update(was, 1);
    pthread_mutex_unlock(&core_lock);
}

/* ---------------------------------------------------------------- capture */

/* Self test of the version that runs: started, capture open, wake word engine loaded, ports bound (all before the thread
 * starts), and then a second of microphone audio through the capture loop.  That is as far as a broken build gets
 * before it is noticed at all; root then makes the update that runs the factory copy, the one the Echo falls back to
 * (ota_healthy, main.sh), so that the fallback is never older than the last version that worked. */
static atomic_long cap_bytes;

static void *selftest_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 300 && !atomic_load(&quit); i++) {          /* 30 s */
        if (atomic_load(&cap_bytes) >= CAP_RATE * 2) { fprintf(stderr, "self test: passed\n"); ota_healthy(); return NULL; }
        usleep(100000);
    }
    fprintf(stderr, "self test: no audio from the mixer within 30 s: this version does not become the fallback\n");
    return NULL;
}

static void *capture_thread(void *arg)
{
    FILE *dump = NULL;
    (void)arg;
    while (!atomic_load(&quit)) {
        const void *pcm; int n = cap_read(&pcm);
        if (n < 0) { fprintf(stderr, "capture: fatal\n"); atomic_store(&quit, 1); break; }
        if (atomic_exchange(&button_pending, 0)) on_action();      /* SIGUSR2: action button, for tests on the PC */
        { int t = atomic_exchange(&trigger_pending, 0);               /* 1: SIGUSR1 plays a wake word of the last 0.6 s */
          if (t == 2) trigger(1); else if (t == 1) wakedet_simulate(); }
        if (atomic_exchange(&stop_pending, 0)) stop_word();        /* SIGHUP: the "stop" keyword, for tests on the PC */
        if (n == 0) continue;
        atomic_fetch_add(&cap_bytes, n);

        /* What the wake word hears (post-AEC micAsr), for listening on the PC.  The mixer feeds the mic only to its one
         * micAsr client, so this is the only way to record it while hassmic runs: kill -TTIN <pid> starts, again stops. */
        if (atomic_exchange(&dump_toggle, 0)) {
            if (dump) { fprintf(stderr, "capture dump: off, %ld bytes\n", ftell(dump)); fclose(dump); dump = NULL; }
            else {
                char path[256]; const char *dir = getenv("HASSMIC_STATE");
                snprintf(path, sizeof path, "%s/capture.raw", dir ? dir : "/data/local/hassmic/state");
                dump = fopen(path, "wb");
                fprintf(stderr, "capture dump: %s %s (16 kHz mono s16le) from capture sample %ld\n", dump ? "on" : "cannot write", path,
                        atomic_load(&cap_bytes) / 2 - n / 2);
            }
        }
        if (dump) fwrite(pcm, 1, n, dump);

        if (core_local_wake) { wake_words_poll(); wakedet_feed(pcm, n / 2); wake_words_feed(pcm, n / 2); }
        detect_feed(pcm, n, atomic_load(&cap_bytes) / 2 - n / 2);
        if (mic_streaming()) mic_queue(pcm, n / 2, core_local_wake ? ring_samples() - n / 2 : 0);    /* mic_sender sends it */
        wakedet_poll();

        int st = atomic_load(&state_seen);              /* no core_lock for every block: see state_seen */
        if (core_local_wake && (st == LISTENING || st == THINKING) && mono_ms() - atomic_load(&state_since_ms) > PIPELINE_TIMEOUT_MS) {
            pthread_mutex_lock(&core_lock);
            if ((state == LISTENING || state == THINKING) && mono_ms() - atomic_load(&state_since_ms) > PIPELINE_TIMEOUT_MS) {
                fprintf(stderr, "pipeline timeout\n");
                core_pipeline_finish();
            }
            pthread_mutex_unlock(&core_lock);
        }
    }
    return NULL;
}

static void *serve_thread(void *arg) { int c = (int)(long)arg; proto->serve(c); close(c); return NULL; }

static void on_usr1(int s) { (void)s; atomic_store(&trigger_pending, 1); }
static void on_usr2(int s) { (void)s; atomic_store(&button_pending, 1); }
static void on_hup(int s) { (void)s; atomic_store(&stop_pending, 1); }
static void on_ttin(int s) { (void)s; atomic_store(&dump_toggle, 1); }

int main(int argc, char **argv)
{
    const char *manifest = NULL, *input = board.keypad; int port = 0, print_mdns = 0, o, use_led = 1, use_volume = 1;
    core_name = board.default_name;
    mic_init();
    while ((o = getopt(argc, argv, "P:p:n:w:m:b:z:o:a:LEVSTB")) != -1) switch (o) {
        case 'P': proto = !strcmp(optarg, "wyoming") ? &proto_wyoming : &proto_esphome; break;
        case 'p': port = atoi(optarg); break;
        case 'n': core_name = optarg; break;
        case 'w': core_local_wake = strcmp(optarg, "remote") != 0; break;
        case 'm': manifest = optarg; break;
        case 'b': input = optarg; break;
        case 'z': core_sendspin_port = atoi(optarg); break;
        case 'o': ota_port = atoi(optarg); break;
        case 'a': arb_port = atoi(optarg); break;
        case 'L': use_led = 0; break;
        case 'E': core_wake_sound(0); break;
        case 'V': use_volume = 0; break;
        case 'S': print_mdns = 1; break;
        case 'B': use_bt = 0; break;
        case 'T': { char tok[160]; sendspin_init(); sendspin_pairing_token(tok, sizeof tok); puts(tok); return 0; }
        default: fprintf(stderr, "usage: hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m manifest] [-b input-device] [-z port] [-o port] [-a port] [-L] [-E] [-V] [-S]\n"); return 2;
    }
    core_port = port ? port : proto->port;
    if (print_mdns) { proto->print_mdns(); return 0; }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN); signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2); signal(SIGHUP, on_hup); signal(SIGTTIN, on_ttin);
    hw_init(use_led, use_volume);
    led("-u", "scone-setup");           /* a restart inside the pairing window: the window is gone, its chaser would loop on */

    if (cap_open() < 0) { fprintf(stderr, "cannot open capture (is PuffinApp still running?)\n"); return 1; }
    pthread_mutex_lock(&core_lock); listening(0); pthread_mutex_unlock(&core_lock);
    if (core_local_wake && !wake_words_init(manifest)) { fprintf(stderr, "cannot load a wake word model\n"); return 1; }
    detect_init();
    if (arb_port && core_local_wake && proto->arb_send && wakedet_arb_start(arb_port)) fprintf(stderr, "arbitration: not available\n");
    /* Read once now, so that their first use (Home Assistant's first look, a volume button) does not run the mixer's tools
     * under core_lock */
    pthread_mutex_lock(&core_lock); core_volume(); core_eq(0); pthread_mutex_unlock(&core_lock);

    /* Without any one of these the satellite only looks alive: better that init starts it again */
    static void *(*const core_threads[])(void *) = { capture_thread, mic_sender, playback_thread, earcon_thread, volume_led_thread };
    for (size_t i = 0; i < sizeof core_threads / sizeof *core_threads; i++) {
        pthread_t t; int e = pthread_create(&t, NULL, core_threads[i], NULL);
        if (e) { fprintf(stderr, "cannot start a core thread: %s\n", strerror(e)); return 1; }
        pthread_detach(t);
    }

    static const struct button_handler buttons = { on_action, on_mute, on_volume };
    if (buttons_start(input, &buttons) < 0) fprintf(stderr, "buttons: %s not available\n", input);
    else if (buttons_muted()) on_mute(1);

    if (core_sendspin_port) sendspin_start(core_sendspin_port);
    if (use_bt) a2dp_start(NULL);
    if (ota_port) ota_start(ota_port);

    int ls = net_listen(core_port);
    if (ls < 0) { perror("listen"); return 1; }
    fprintf(stderr, "hassmic " VERSION " (" BUILD ") %s on %d, wake=%s\n", proto->id, core_port, core_local_wake ? "local" : "remote");
    /* Also without the push port: the passed self test is what keeps an installed update from being undone after three
     * boots (main.sh), online updates included. */
    { pthread_t st; if (!pthread_create(&st, NULL, selftest_thread, NULL)) pthread_detach(st); }
    while (!atomic_load(&quit)) {
        int c = net_accept(ls);
        if (c < 0) break;
        /* A link that went away must not hold a client slot (or core_lock, in a blocked write) for ever:
         * writes give up after 5 s, keepalive notices a dead peer within ~25 s.  Both make serve() return. */
        { struct timeval tv = { 5, 0 }; int on = 1, idle = 10, intvl = 5, cnt = 3;
          setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle); setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt); }
        if (proto->threaded) {
            pthread_t t;
            if (pthread_create(&t, NULL, serve_thread, (void *)(long)c)) close(c); else pthread_detach(t);
        } else { proto->serve(c); close(c); }
    }
    cap_close();
    if (core_local_wake) wake_words_close();
    return 1;
}
