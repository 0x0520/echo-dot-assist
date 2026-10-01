/*
 * aed_test - run Amazon's stock acoustic event detector (Alexa Guard sounds) over raw 16 kHz mono s16le audio.
 *
 *   aed_test [-m manifest] [-x speed] [-t type,type,...] [-j] [file.raw|file.wav]
 *
 * -t enables only the listed types (default: all twelve in AED.json); -j prints each window's full JSON.
 *
 * Detection is off until the client properties PuffinApp pushes are set (PryonAcousticEventManager 0x44066c):
 * "AcousticEventDetectionEnabled" and "aed_<type>_enabled". The decoder scores a window of ~10 s and reports once
 * per window, so a sound can take up to 10 s to show up. Audio is pushed at real-time speed times -x (default 1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pryon_api.h"

#define DEFAULT_MANIFEST "/system/local/models/AED/pryon.manifest"

static const char *TYPES[] = { "humanPresence", "smokeAlarm", "glassBreak", "dogBark", "babyCry", "snore", "cough",
                               "waterSounds", "beepingAppliance", "smokeSiren", "carbonMonoxideSiren", "runningWater" };
#define NTYPES (sizeof TYPES / sizeof *TYPES)

static int windows, fulljson;

static void on_log(int level, const char *tag, const char *msg)
{
    if (level <= 3) fprintf(stderr, "pryon[%d] %s: %s\n", level, tag ? tag : "", msg ? msg : "");
}

/* Pulls "<key>":<number or bool> out of a type's object (obj points just past its "{"). The object ends where the
 * next type's begins: smokeSiren carries a JSON string with braces in it ("detectionDescriptorProfile"). */
static const char *field(const char *obj, const char *key, char *out, size_t n)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *end = strstr(obj, ":{\"aed_type\""), *p = strstr(obj, pat);
    if (!p || (end && p > end)) return NULL;
    p += strlen(pat);
    size_t i = 0; while (p[i] && p[i] != ',' && p[i] != '}' && i + 1 < n) { out[i] = p[i]; i++; }
    out[i] = 0;
    return out;
}

static void on_result(const char *decoderId, PryonAcousticEventResult *r)
{
    static const char *kind[] = { "nothing", "near miss", "DETECTED" };
    windows++;
    printf("window %d decoder=%s end=%.2fs result=%s\n", windows, decoderId, r->sampleIndex / 16000.0,
           r->resultType >= 0 && r->resultType <= 2 ? kind[r->resultType] : "?");
    for (unsigned t = 0; r->json && t < NTYPES; t++) {
        char pat[48], score[16], det[8], nm[8]; snprintf(pat, sizeof pat, "\"%s\":{", TYPES[t]);
        const char *o = strstr(r->json, pat);
        if (o) o += strlen(pat);
        if (!o || !field(o, "score", score, sizeof score)) continue;
        field(o, "detected", det, sizeof det); field(o, "nearMiss", nm, sizeof nm);
        printf("  %-20s %s%s%s\n", TYPES[t], score, !strcmp(det, "true") ? "  detected" : "",
               !strcmp(nm, "true") ? "  near miss" : "");
    }
    if (fulljson && r->json) printf("  %s\n", r->json);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *manifest = DEFAULT_MANIFEST, *only = NULL;
    double speed = 1;
    int o, rc;
    while ((o = getopt(argc, argv, "m:x:t:j")) != -1) switch (o) {
        case 'm': manifest = optarg; break;
        case 'x': speed = atof(optarg); if (speed <= 0) speed = 1; break;
        case 't': only = optarg; break;
        case 'j': fulljson = 1; break;
        default: fprintf(stderr, "usage: aed_test [-m manifest] [-x speed] [-t type,...] [-j] [file]\n"); return 2;
    }
    FILE *in = optind < argc ? fopen(argv[optind], "rb") : stdin;
    if (!in) { perror("open"); return 1; }
    if (optind < argc && strlen(argv[optind]) > 4 && !strcmp(argv[optind] + strlen(argv[optind]) - 4, ".wav"))
        fseek(in, 44, SEEK_SET);        /* canonical header only */

    PryonApi_SetLoggingCallback(on_log);
    PryonApi_SetAcousticEventDetectionResultCallback(on_result);
    if ((rc = PryonModelSet_New("aed_ms", manifest, ""))) { fprintf(stderr, "PryonModelSet_New -> %d\n", rc); return 1; }
    PryonMultichannelAudioFormat fmt;
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    if ((rc = PryonDecoder_NewMultichannelAudioDecoder("aed", "aed_ms", "pryon", NULL, 0, 0, 0, 0, 0, fmt))) {
        fprintf(stderr, "PryonDecoder_NewMultichannelAudioDecoder -> %d\n", rc); return 1;
    }

    static char names[NTYPES][48];
    PryonClientProperty props[NTYPES + 1]; PryonClientEvent events[NTYPES + 1]; uint32_t n = 0;
    props[n].name = "AcousticEventDetectionEnabled"; props[n].value = 1; n++;
    for (unsigned t = 0; t < NTYPES; t++) {
        if (only) {     /* whole word in the comma list */
            const char *p = strstr(only, TYPES[t]); size_t l = strlen(TYPES[t]);
            if (!p || (p > only && p[-1] != ',') || (p[l] && p[l] != ',')) continue;
        }
        snprintf(names[t], sizeof names[t], "aed_%s_enabled", TYPES[t]);
        props[n].name = names[t]; props[n].value = 1; n++;
    }
    for (uint32_t i = 0; i < n; i++) { events[i].one = 1; events[i].property = &props[i]; }
    if ((rc = PryonDecoder_PushClientEvents("aed", events, n))) { fprintf(stderr, "PushClientEvents -> %d\n", rc); return 1; }

    int16_t buf[1600]; uint64_t idx = 0; size_t got;
    while ((got = fread(buf, sizeof *buf, 1600, in)) > 0) {
        if ((rc = PryonDecoder_PushAudioEventSamples("aed", idx, buf, got))) {
            fprintf(stderr, "push @%llu -> %d\n", (unsigned long long)idx, rc); break;
        }
        idx += got;
        usleep((useconds_t)(got * 1e6 / 16000 / speed));
    }
    PryonDecoder_BacklogWait("aed", -1);
    sleep(2);
    fprintf(stderr, "pushed %.1fs, %d windows (a window is reported only once it is complete)\n", idx / 16000.0, windows);
    PryonDecoder_Delete("aed");
    PryonModelSet_Delete("aed_ms");
    return 0;
}
