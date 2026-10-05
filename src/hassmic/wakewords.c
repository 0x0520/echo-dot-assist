/* Wake word models
 * The firmware only has "Alexa"; other keywords are model sets fetched from Amazon once (README, "Another wake word")
 * and kept in <models>/<keyword>-<language>/pryon.manifest.  All are offered to Home Assistant, which shows them in the
 * satellite's wake word select; its pick is kept in state/wake_word and loaded live by the capture thread.  -m names
 * the model to use until Home Assistant has picked one (before 2026-09-25 it was the only way, and Home Assistant was
 * told "Alexa" whatever -m said). */
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "board.h"
#include "core_int.h"
#include "wake.h"

#define MAX_WAKE_WORDS 16
static struct core_wake_word wake_words[MAX_WAKE_WORDS];
static int n_wake_words, wake_active;               /* under core_lock once running */
static atomic_int wake_switch;                      /* capture thread: load wake_words[wake_active] */
static atomic_int wake_reset_pending;               /* capture thread: wake_reset() before the next block */
static int wake_ok;                                 /* capture thread (main() before it starts): a model is loaded */

static const char *models_dir(void) { const char *e = getenv("HASSMIC_MODELS"); return e ? e : "/data/local/hassmic/models"; }
static const char *wake_word_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/wake_word", d ? d : "/data/local/hassmic/state");
    return p;
}

/* id "echo-de" -> name "Echo", language "de"; "hey_disney-en-US" -> "Hey Disney", "en" */
static int wake_word_add(const char *id, const char *manifest)
{
    for (int i = 0; i < n_wake_words; i++) if (!strcmp(wake_words[i].manifest, manifest)) return i;
    if (n_wake_words == MAX_WAKE_WORDS) return -1;
    struct core_wake_word *w = &wake_words[n_wake_words];
    const char *dash = strchr(id, '-');
    int k = dash ? (int)(dash - id) : (int)strlen(id);
    snprintf(w->id, sizeof w->id, "%s", id); snprintf(w->manifest, sizeof w->manifest, "%s", manifest);
    snprintf(w->name, sizeof w->name, "%.*s", k, id);
    for (char *c = w->name; *c; c++) {
        if (*c == '_') *c = ' ';
        *c = (char)(c == w->name || c[-1] == ' ' ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    }
    snprintf(w->lang, sizeof w->lang, "%s", dash ? dash + 1 : "en");
    w->lang[strcspn(w->lang, "-_")] = 0;
    return n_wake_words++;
}

static void wake_words_scan(const char *m_arg)
{
    char path[512], saved[64] = ""; DIR *d; struct dirent *e; FILE *f; int def = 0;
    wake_word_add(board.wake_id, board.wake_manifest);      /* the firmware's own: always there */
    if ((d = opendir(models_dir()))) {
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s/pryon.manifest", models_dir(), e->d_name);
            if (!access(path, R_OK)) wake_word_add(e->d_name, path);
        }
        closedir(d);
    }
    if (m_arg) {                                    /* named after its directory, like the ones found above */
        char dir[256], *slash; snprintf(dir, sizeof dir, "%s", m_arg);
        if ((slash = strrchr(dir, '/'))) *slash = 0;
        slash = strrchr(dir, '/');
        int i = wake_word_add(slash ? slash + 1 : dir, m_arg);
        if (i >= 0) def = i;
    }
    wake_active = def;
    if ((f = fopen(wake_word_path(), "r"))) {
        if (fscanf(f, "%63s", saved) == 1) for (int i = 0; i < n_wake_words; i++) if (!strcmp(wake_words[i].id, saved)) wake_active = i;
        fclose(f);
    }
    for (int i = 0; i < n_wake_words; i++)
        fprintf(stderr, "wake word: %s \"%s\" (%s)%s\n", wake_words[i].id, wake_words[i].name, wake_words[i].lang, i == wake_active ? ", active" : "");
}

int core_wake_words(const struct core_wake_word **list) { *list = wake_words; return n_wake_words; }

int core_wake_word(int set)
{
    if (set >= 0 && set < n_wake_words && set != wake_active) {
        FILE *f = fopen(wake_word_path(), "w");
        wake_active = set;
        if (f) { fprintf(f, "%s\n", wake_words[set].id); fclose(f); } else fprintf(stderr, "wake word: cannot write %s\n", wake_word_path());
        atomic_store(&wake_switch, 1);
    }
    return wake_active;
}

void wake_word_active_name(char *out, size_t n) { snprintf(out, n, "%s", wake_words[wake_active].name); }

/* Capture thread, or main() before it starts.  A model that does not load falls back to the stock one, and that is
 * what Home Assistant is told from then on (it used to be shown the one that failed).  0: none loaded, no wake word */
static int wake_load(int i)
{
    if (wake_open(wake_words[i].manifest, on_wake) == 0) { fprintf(stderr, "wake word: \"%s\" loaded\n", wake_words[i].name); return 1; }
    fprintf(stderr, "wake word: cannot load %s\n", wake_words[i].manifest);
    if (i == 0) return 0;
    if (wake_open(wake_words[0].manifest, on_wake)) { fprintf(stderr, "wake word: cannot load %s either\n", wake_words[0].manifest); return 0; }
    fprintf(stderr, "wake word: back to \"%s\"\n", wake_words[0].name);
    pthread_mutex_lock(&core_lock);
    if (wake_active == i) wake_active = 0;          /* not a pick Home Assistant made meanwhile: that loads next */
    pthread_mutex_unlock(&core_lock);
    return 1;
}

int wake_words_init(const char *m_arg)
{
    wake_words_scan(m_arg);
    return wake_ok = wake_load(wake_active);
}

void wake_words_poll(void)
{
    if (atomic_exchange(&wake_switch, 0)) {        /* Home Assistant picked another wake word */
        pthread_mutex_lock(&core_lock); int i = wake_active; pthread_mutex_unlock(&core_lock);
        if (wake_ok) wake_close();
        wake_ok = wake_load(i);
    }
    if (atomic_exchange(&wake_reset_pending, 0) && wake_ok) wake_reset();
}

void wake_words_feed(const int16_t *pcm, size_t n) { if (wake_ok) wake_feed(pcm, n); }
void wake_words_reset(void) { atomic_store(&wake_reset_pending, 1); }
void wake_words_close(void) { wake_close(); }
