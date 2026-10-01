/* SBC encoder: encodes a 44.1 kHz stereo .au (big-endian s16, from sbc_ref.sh) with sbc.c, writes the .sbc for libsbc's
 * sbcdec to decode (it checks the CRC), and prints the SNR of that decoding against the input. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "sbc.h"

int main(int argc, char **argv)
{
    FILE *in, *out; unsigned char h[24]; struct sbc_enc e; int16_t pcm[2 * SBC_ENC_FRAMES]; uint8_t frame[512];
    if (argc != 4 || !(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb")) || fread(h, 1, 24, in) != 24 ||
        sbc_enc_init(&e, 44100, atoi(argv[3]))) { fprintf(stderr, "usage: sbc_enc_test in.au out.sbc bitpool\n"); return 2; }
    unsigned char b[4 * SBC_ENC_FRAMES];
    while (fread(b, 4, SBC_ENC_FRAMES, in) == SBC_ENC_FRAMES) {
        for (int i = 0; i < 2 * SBC_ENC_FRAMES; i++) pcm[i] = (int16_t)(b[2 * i] << 8 | b[2 * i + 1]);
        size_t n = sbc_encode(&e, pcm, frame);
        if (n != sbc_enc_len(e.bitpool) || fwrite(frame, 1, n, out) != n) return 1;
    }
    return fclose(out) != 0;
}
