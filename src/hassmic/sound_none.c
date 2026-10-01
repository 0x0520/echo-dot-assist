/* No sound detector: PC test build.  HASSMIC_FAKE_SOUND=<Amazon type> (e.g. dogBark) makes every ~10 s window of the
 * audio fed while it is on report that type, as the stock detector would (tests/fake_ha_esphome.py). */
#include "sound.h"
#include <stdlib.h>
#include <string.h>

static sound_cb callback;
static const char *fake;
static uint64_t fed;
static int opened;

int sound_open(const char *const *types, int n, sound_cb cb)
{
    const char *e = getenv("HASSMIC_FAKE_SOUND");
    callback = cb; fake = NULL; fed = 0; opened = 1;
    for (int i = 0; e && i < n; i++) if (!strcmp(types[i], e)) fake = types[i];
    return 0;
}

void sound_feed(const int16_t *samples, size_t count)
{
    (void)samples;
    if (!opened || !fake) return;
    if ((fed + count) / 159680 > fed / 159680) callback(&fake, 1);       /* 9.98 s windows, as the model's */
    fed += count;
}

void sound_close(void) { opened = 0; }
