/*
 * pryon_test - run Amazon's stock wake-word model over a raw 16 kHz mono s16le file (or stdin).
 *
 *   pryon_test [-m manifest] [-c chunk_samples] [-x speed] [-p name=value ...] [file.raw|file.wav]
 *
 * -p pushes a client property before the audio (e.g. -p AlarmState=1): the model then uses its lower accept threshold.
 *
 * Audio is pushed at real-time speed times -x (default 1): the decoder drops audio when its backlog exceeds ~7 s.
 *
 * Prints every result callback, so the meaning of detectionType can be learned.
 */
#include <stdio.h>
#include <zlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pryon_api.h"

#define DEFAULT_MANIFEST "/system/local/models/keyword/en-US/ALEXA/pryon.manifest"

static int hits;

static void on_log(int level, const char *tag, const char *msg)
{
    fprintf(stderr, "pryon[%d] %s: %s\n", level, tag ? tag : "", msg ? msg : "");
}

static void on_result(const char *decoderId, PryonEnumeratedResult *r)
{
    hits++;
    printf("RESULT decoder=%s keyword=%s type=%d begin=%llu end=%llu (%.2fs..%.2fs) meta=%u\n",
           decoderId, r->keyword ? r->keyword : "(null)", r->detectionType,
           (unsigned long long)r->beginSampleIndex, (unsigned long long)r->endSampleIndex,
           r->beginSampleIndex / 16000.0, r->endSampleIndex / 16000.0, r->metadataSize);
    /* the metadata: a header that starts "JSON_GZ_AND_FP", a gzip stream with JSON, a fingerprint */
    const unsigned char *m = r->metadata; size_t at = 0;
    while (m && at + 3 < r->metadataSize && at < 64 && !(m[at] == 0x1f && m[at + 1] == 0x8b && m[at + 2] == 8)) at++;
    if (m && at < 64 && at + 3 < r->metadataSize) {
        static char js[1 << 17]; z_stream z; memset(&z, 0, sizeof z);
        z.next_in = (Bytef *)(m + at); z.avail_in = r->metadataSize - at; z.next_out = (Bytef *)js; z.avail_out = sizeof js - 1;
        if (inflateInit2(&z, 16 + MAX_WBITS) == Z_OK) { inflate(&z, Z_FINISH); inflateEnd(&z); }
        js[sizeof js - 1 - z.avail_out] = 0;
        printf("META header %u bytes, %s\n", (unsigned)at, js);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *manifest = DEFAULT_MANIFEST; unsigned chunk = 800; double speed = 1; int o, rc;
    PryonClientProperty props[8]; PryonClientEvent events[8]; unsigned nprops = 0;
    while ((o = getopt(argc, argv, "m:c:x:p:")) != -1) switch (o) {
        case 'm': manifest = optarg; break;
        case 'c': chunk = atoi(optarg); break;
        case 'x': speed = atof(optarg); break;
        case 'p': {
            char *eq = strchr(optarg, '=');
            if (!eq || nprops == sizeof props / sizeof *props) { fprintf(stderr, "-p name=value\n"); return 2; }
            *eq = 0; props[nprops].name = optarg; props[nprops].value = atoll(eq + 1);
            events[nprops].one = 1; events[nprops].property = &props[nprops]; nprops++;
            break;
        }
        default: fprintf(stderr, "usage: pryon_test [-m manifest] [-c chunk_samples] [-x speed] [-p name=value] [file]\n"); return 2;
    }
    FILE *in = optind < argc ? fopen(argv[optind], "rb") : stdin;
    if (!in) { perror("open"); return 1; }
    if (optind < argc && strlen(argv[optind]) > 4 && !strcmp(argv[optind] + strlen(argv[optind]) - 4, ".wav"))
        fseek(in, 44, SEEK_SET);        /* canonical header only */

    const char *attrs = PryonApi_GetAttributes();
    fprintf(stderr, "attributes: %s\n", attrs ? attrs : "(null)");
    PryonApi_SetLoggingCallback(on_log);
    PryonApi_SetEnumeratedResultCallback(on_result);

    if ((rc = PryonModelSet_New("ms", manifest, ""))) { fprintf(stderr, "PryonModelSet_New -> %d\n", rc); return 1; }
    PryonMultichannelAudioFormat fmt;
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    fprintf(stderr, "format: enc=%d rate=%d bits=%d ch=%d type0=%d\n",
            fmt.encoding, fmt.sampleRate, fmt.bitsPerSample, fmt.numChannels, fmt.channelTypes[0]);
    if ((rc = PryonDecoder_NewSpotterAudioDecoder("dec", "ms", "pryon", fmt, "{}"))) {
        fprintf(stderr, "PryonDecoder_NewSpotterAudioDecoder -> %d\n", rc); return 1;
    }
    if (nprops) {
        rc = PryonDecoder_PushClientEvents("dec", events, nprops);
        fprintf(stderr, "PryonDecoder_PushClientEvents(%u) -> %d\n", nprops, rc);
        if (rc) return 1;
    }

    int16_t *buf = malloc(chunk * sizeof *buf); uint64_t idx = 0; size_t n;
    while ((n = fread(buf, sizeof *buf, chunk, in)) > 0) {
        if ((rc = PryonDecoder_PushAudioEventSamples("dec", idx, buf, n))) {
            fprintf(stderr, "push @%llu -> %d\n", (unsigned long long)idx, rc); break;
        }
        idx += n;
        usleep((useconds_t)(n * 1e6 / 16000 / speed));
    }
    PryonDecoder_BacklogWait("dec", -1);
    sleep(2);
    fprintf(stderr, "pushed %llu samples (%.1fs), %d callbacks\n", (unsigned long long)idx, idx / 16000.0, hits);
    PryonDecoder_Delete("dec");
    PryonModelSet_Delete("ms");
    return hits ? 0 : 3;
}
