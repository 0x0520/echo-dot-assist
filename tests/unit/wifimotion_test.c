/* The Wi-Fi motion detector (wifimotion.c) on readings shaped like the Dot 3's (2026-10-01): a still room scatters
 * RCPI by about half a dB-step with an odd frame now and then; walking through the path moves it by several.  And the
 * levels per kind of frame in front of it (wm_kind_norm). */
#include "wifimotion.h"
#include <stdio.h>
#include <stdlib.h>

static int failed;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); failed |= !ok; }

static float still(void) { return 112 + (rand() % 3 - 1) * 0.6f; }

int main(void)
{
    struct wm_det d; long long t = 0; int on = 0, first = -1;
    srand(1);

    check(wm_parse_rcpi("RX Stat:\nRX SNR (dB)          = 33\nRCPI RX0             = 112\nRCPI RX1             = 255\n") == 112, "RCPI RX0 parsed");
    check(wm_parse_rcpi("RCPI RX0             = 255\n") == -1 && wm_parse_rcpi("No CSI Data") == -1, "no frame (255) and other answers: no reading");

    /* 5 min of a still room with an odd frame every 7 s: never motion */
    wm_reset(&d);
    for (int i = 0; i < 3000; i++, t += 100) on |= wm_feed(&d, i % 70 == 35 ? 124 : still(), t, WIFIMOTION_SENS_DEFAULT, 30000);
    check(!on, "still room with odd frames: no motion at the default sensitivity");

    /* walking: 10 s of levels jumping by up to 4 */
    for (int i = 0; i < 100; i++, t += 100)
        if (wm_feed(&d, 112 + (rand() % 9 - 4), t, WIFIMOTION_SENS_DEFAULT, 30000) && first < 0) first = i;
    check(first >= 0 && first <= 30, "walking: motion within 3 s");
    printf("     (after %.1f s)\n", first / 10.0);

    /* still again: on for the 30 s hold, then off */
    int at29 = 0, at31 = 0;
    for (int i = 0; i < 400; i++, t += 100) {
        int m = wm_feed(&d, still(), t, WIFIMOTION_SENS_DEFAULT, 30000);
        if (i == 270) at29 = m;
        if (i == 330) at31 = m;
    }
    check(at29 && !at31, "motion clears 30 s after the last movement");

    /* the most sensitive setting fires on a smaller movement the default does not */
    struct wm_det a, b; int ma = 0, mb = 0;
    wm_reset(&a); wm_reset(&b); srand(2);
    for (int i = 0; i < 300; i++, t += 100) {
        float v = i < 100 ? still() : 112 + (rand() % 5 - 2) * 0.55f;
        ma |= wm_feed(&a, v, t, WIFIMOTION_SENS_MAX, 30000) && i >= 100;
        mb |= wm_feed(&b, v, t, WIFIMOTION_SENS_DEFAULT, 30000);
    }
    check(ma && !mb, "sensitivity 10 sees a small movement, 5 does not");

    /* the Dot 3 on 5 GHz: broadcasts at 100, unicast at 93 (MCS 8) and 91 (MCS 9), mixed frame by frame */
    struct wm_kinds w = {0}; float v; int used = 0, early = 0; on = 0; first = -1;
    static const unsigned key[3] = { 0x1000b, 0x14008, 0x14009 }; static const int lvl[3] = { 100, 93, 91 };
    wm_reset(&d); srand(3);
    for (int i = 0; i < 3000; i++, t += 100) {
        int k = rand() % 10 < 2 ? 0 : 1 + rand() % 2, ok = wm_kind_norm(&w, lvl[k] + rand() % 2, key[k], t, &v);
        if (ok) { used++; on |= wm_feed(&d, v, t, WIFIMOTION_SENS_DEFAULT, 30000); } else early |= i > 30;
    }
    check(!on && used > 2900 && !early, "rates at their own power, nobody about: no motion, readings used once each rate has a level");
    for (int i = 0; i < 100; i++, t += 100) {
        int k = rand() % 10 < 2 ? 0 : 1 + rand() % 2;
        if (wm_kind_norm(&w, lvl[k] + rand() % 9 - 4, key[k], t, &v) && wm_feed(&d, v, t, WIFIMOTION_SENS_DEFAULT, 30000) && first < 0) first = i;
    }
    check(first >= 0 && first <= 30, "and walking through still shows");

    /* the Dot 2 and Echo 2: no rate, only broadcast (KIND 1) or not; broadcasts 8 over the rest */
    struct wm_kinds g = {0}; on = 0;
    wm_reset(&d);
    for (int i = 0; i < 3000; i++, t += 100) {
        int bc = rand() % 10 < 2;
        if (wm_kind_norm(&g, (bc ? 112 : 104) + rand() % 2, bc, t, &v)) on |= wm_feed(&d, v, t, WIFIMOTION_SENS_DEFAULT, 30000);
    }
    check(!on, "broadcasts at their own power, nobody about: no motion");
    return failed;
}
