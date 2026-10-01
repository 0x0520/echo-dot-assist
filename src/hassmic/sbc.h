/* SBC (A2DP specification, appendix B / section 12): decoder for the Bluetooth speaker, encoder for playing to one. */
#ifndef SBC_H
#define SBC_H
#include <stddef.h>
#include <stdint.h>

struct sbc {
    unsigned rate, channels;                    /* of the last frame decoded */
    int nsb;                                    /* subbands the filter state belongs to; 0 = fresh */
    float v[2][160];                            /* synthesis filter state per channel (10 * 2M values, newest first) */
};

void sbc_init(struct sbc *s);
/* One frame from p (n bytes available).  Writes blocks * subbands samples per channel, interleaved, to out (room for
 * 16 * 8 * 2).  Returns the frame's length in bytes and sets *samples (per channel); 0 = not a whole valid frame. */
size_t sbc_decode(struct sbc *s, const uint8_t *p, size_t n, int16_t *out, unsigned *samples);

/* Encoder: the one configuration every SBC sink must decode well, 16 blocks, 8 subbands, joint stereo, loudness.
 * A frame takes SBC_ENC_FRAMES stereo frames (interleaved s16) and writes sbc_enc_len(bitpool) bytes. */
#define SBC_ENC_FRAMES 128
struct sbc_enc {
    int fs, bitpool;                            /* sampling frequency index (0 16 kHz .. 3 48 kHz), 2..250 (a sink caps it) */
    float x[2][80];                             /* analysis filter state per channel, newest first */
};
int    sbc_enc_init(struct sbc_enc *e, unsigned rate, int bitpool);   /* -1: rate is not an SBC one */
size_t sbc_enc_len(int bitpool);
size_t sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out);
#endif
