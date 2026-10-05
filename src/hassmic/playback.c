/* The playback queue: replies (TTS) from the protocol to the speaker, on the playback thread. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "core_int.h"

/* Replies are numbered streams: core_tts_begin starts the next one.  A cut (barge-in, "stop", Home Assistant's media
 * stop) drops the stream that is queued or playing and everything before it; it can never outlast that stream, so the
 * next reply always plays.  It used to be one flag that only the end of a played stream cleared: a media stop while the
 * reply was still being fetched never got one, and every reply after it stayed silent until hassmic restarted.
 * With nothing queued, a media stop is for the fetch under way (proto_esphome.c): it is kept for the stream about to
 * begin, and spent when that begins or the pipeline ends without one. */
struct item { struct item *next; int kind; unsigned gen, rate, ch; size_t len; unsigned char data[]; };
enum { Q_START, Q_DATA, Q_STOP };
/* Wyoming has no authentication and Home Assistant sends TTS faster than it plays: without a bound the queue took a
 * whole reply, or anything a client cared to send, into memory.  At 22050 Hz mono this is 12 s ahead of the speaker
 * (ESPHome's fetch holds back at 256 KiB by itself); a producer that finds it full waits, and drops after TTS_WAIT_MS of
 * a playback that does not move. */
#define TTS_QUEUE_MAX (1024 * 1024)
#define TTS_WAIT_MS   5000

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;   /* inside core_lock where both are taken */
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER, q_space = PTHREAD_COND_INITIALIZER;
static struct item *q_head, *q_tail;
static size_t q_bytes;
static unsigned tts_gen;            /* under q_lock: the newest stream begun */
static unsigned tts_done;           /* under q_lock: the newest stream whose end the playback thread has played out */
static unsigned tts_cut_to;         /* under q_lock: streams up to this one are dropped */
static int tts_cut_next;            /* under q_lock: and the one about to begin */

static int tts_cutting(void) { return tts_cut_next || (tts_cut_to == tts_gen && tts_done != tts_gen); }    /* q_lock held */

static void q_push(int kind, unsigned rate, unsigned ch, const void *data, size_t len)
{
    struct item *it = malloc(sizeof *it + len);
    if (!it) return;
    it->next = NULL; it->kind = kind; it->rate = rate; it->ch = ch; it->len = len;
    if (len) memcpy(it->data, data, len);
    pthread_mutex_lock(&q_lock);
    if (kind == Q_START) { if (tts_cut_next) { tts_cut_to = tts_gen + 1; tts_cut_next = 0; } tts_gen++; }
    it->gen = tts_gen;
    if (kind == Q_DATA) {           /* back pressure: never with core_lock held (core.h), so the wait stops no one else */
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += TTS_WAIT_MS / 1000;
        while (q_bytes + len > TTS_QUEUE_MAX && it->gen > tts_cut_to)
            if (pthread_cond_timedwait(&q_space, &q_lock, &until)) {
                fprintf(stderr, "play: playback stuck, %zu bytes queued: TTS audio dropped\n", q_bytes);
                break;
            }
        /* also when a newer stream began meanwhile: a producer that waited here belongs to a reply that is over */
        if (it->gen <= tts_cut_to || it->gen != tts_gen || q_bytes + len > TTS_QUEUE_MAX) { pthread_mutex_unlock(&q_lock); free(it); return; }
    }
    if (q_tail) q_tail->next = it; else q_head = it;
    q_tail = it; q_bytes += len;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

void tts_cut(void)
{
    pthread_mutex_lock(&q_lock);
    if (tts_done != tts_gen) { tts_cut_to = tts_gen; pthread_cond_broadcast(&q_space); }
    pthread_mutex_unlock(&q_lock);
}

void tts_pending_spent(void)
{
    pthread_mutex_lock(&q_lock); tts_cut_next = 0; pthread_mutex_unlock(&q_lock);
}

void *playback_thread(void *arg)
{
    int open = 0;
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head) pthread_cond_wait(&q_cond, &q_lock);
        struct item *it = q_head;
        q_head = it->next; if (!q_head) q_tail = NULL;
        q_bytes -= it->len;
        int drop = it->kind != Q_STOP && it->gen <= tts_cut_to;
        pthread_cond_broadcast(&q_space);
        pthread_mutex_unlock(&q_lock);

        if (drop) {
            if (open) { play_close(0); open = 0; }
            free(it);
            continue;
        }
        switch (it->kind) {
        case Q_START:
            if (open) play_close(1);        /* e.g. announcement chime followed by the message */
            open = play_open(it->rate, it->ch) == 0;
            if (open) own_tts(1);
            if (!open) fprintf(stderr, "play: open %u Hz x%u failed\n", it->rate, it->ch);
            break;
        case Q_DATA:
            if (open && play_write(it->data, it->len) < 0) { play_close(0); open = 0; }
            break;
        case Q_STOP:
            if (open) { play_close(1); open = 0; }
            own_tts(0);
            /* The stream counts as over only under core_lock, together with the state going back to IDLE: a wake word in
             * between would have seen SPEAKING and a stream already ended, cut nothing and flagged nothing, and the
             * barge-in it starts is the restart below. */
            pthread_mutex_lock(&core_lock);
            pthread_mutex_lock(&q_lock); tts_done = it->gen; pthread_cond_broadcast(&q_space); pthread_mutex_unlock(&q_lock);
            if (core_proto()->played) core_proto()->played();      /* also without a client: modules reset their state here */
            core_pipeline_finish();
            pthread_mutex_unlock(&core_lock);
            break;
        }
        free(it);
    }
    return NULL;
}

void core_tts_begin(unsigned rate, unsigned channels)
{
    mic_stream_end();
    mic_stopped();
    core_set_state(SPEAKING);
    q_push(Q_START, rate, channels, NULL, 0);
}

void core_tts_data(const void *pcm, size_t len) { q_push(Q_DATA, 0, 0, pcm, len); }
void core_tts_end(void) { q_push(Q_STOP, 0, 0, NULL, 0); }
int  core_tts_flushing(void) { pthread_mutex_lock(&q_lock); int r = tts_cutting(); pthread_mutex_unlock(&q_lock); return r; }

void core_tts_flush(void)
{
    pthread_mutex_lock(&q_lock);
    if (tts_done != tts_gen) tts_cut_to = tts_gen; else tts_cut_next = 1;
    pthread_cond_broadcast(&q_space);
    pthread_mutex_unlock(&q_lock);
}

size_t core_tts_queued(void)
{
    pthread_mutex_lock(&q_lock); size_t n = q_bytes; pthread_mutex_unlock(&q_lock);
    return n;
}
