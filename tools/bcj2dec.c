/* bcj2dec - undo 7-Zip's BCJ2 x86 filter (LZMA SDK 9.20 algorithm, public
 * domain). Used to unpack installers for the VM tests without running them:
 * Edge Update (Omaha) stores its payload LZMA-compressed and BCJ2-filtered,
 * as 5 little-endian sizes (output, main, call, jump, range coder) followed
 * by the four streams.
 *
 *   cc -O2 -o out/bcj2dec tools/bcj2dec.c
 *   bcj2dec <in: the LZMA-decompressed payload> <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define TOP (1u << 24)
#define BITS 11
#define TOTAL (1u << BITS)
#define MOVE 5
#define IsJ(b0, b1) (((b1) & 0xFE) == 0xE8 || ((b0) == 0x0F && ((b1) & 0xF0) == 0x80))

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint8_t *in, *out;
    uint32_t sz[5];
    const uint8_t *b0, *b1, *b2, *rc, *rcEnd;
    size_t s1, s2, inPos = 0, outPos = 0, outSize, size0;
    uint16_t p[2 + 256];
    uint32_t range = 0xFFFFFFFF, code = 0;
    uint8_t prev = 0;
    int i;

    if (argc != 3) { fprintf(stderr, "usage: bcj2dec <in> <out>\n"); return 2; }
    if (!(f = fopen(argv[1], "rb"))) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    in = malloc(n);
    if (!in || fread(in, 1, n, f) != (size_t)n || n < 20) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
    for (i = 0; i < 5; i++) sz[i] = in[4*i] | in[4*i+1] << 8 | in[4*i+2] << 16 | (uint32_t)in[4*i+3] << 24;
    if (20ull + sz[1] + sz[2] + sz[3] + sz[4] != (unsigned long long)n) { fprintf(stderr, "sizes don't add up: not a BCJ2 payload\n"); return 1; }
    outSize = sz[0]; size0 = sz[1]; s1 = sz[2]; s2 = sz[3];
    b0 = in + 20; b1 = b0 + sz[1]; b2 = b1 + sz[2]; rc = b2 + sz[3]; rcEnd = rc + sz[4];
    out = malloc(outSize ? outSize : 1);
    if (!out) { fprintf(stderr, "out of memory\n"); return 1; }
    for (i = 0; i < 2 + 256; i++) p[i] = TOTAL >> 1;
    for (i = 0; i < 5; i++) { if (rc == rcEnd) goto bad; code = code << 8 | *rc++; }
#define NORM if (range < TOP) { if (rc == rcEnd) goto bad; range <<= 8; code = code << 8 | *rc++; }
    if (outSize == 0) goto done;
    for (;;) {
        uint8_t b;
        uint16_t *prob;
        uint32_t bound, ttt;
        size_t limit = size0 - inPos;
        if (outSize - outPos < limit) limit = outSize - outPos;
        while (limit) {
            b = b0[inPos];
            out[outPos++] = b;
            if (IsJ(prev, b)) break;
            inPos++; prev = b; limit--;
        }
        if (!limit || outPos == outSize) break;
        b = b0[inPos++];
        prob = b == 0xE8 ? p + prev : b == 0xE9 ? p + 256 : p + 257;
        ttt = *prob; bound = (range >> BITS) * ttt;
        if (code < bound) {
            range = bound; *prob = (uint16_t)(ttt + ((TOTAL - ttt) >> MOVE)); NORM;
            prev = b;
        } else {
            const uint8_t *v;
            uint32_t dest;
            range -= bound; code -= bound; *prob = (uint16_t)(ttt - (ttt >> MOVE)); NORM;
            if (b == 0xE8) { if (s1 < 4) goto bad; v = b1; b1 += 4; s1 -= 4; }
            else           { if (s2 < 4) goto bad; v = b2; b2 += 4; s2 -= 4; }
            dest = ((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) - (uint32_t)(outPos + 4);
            for (i = 0; i < 4; i++) { out[outPos++] = (uint8_t)(dest >> (8 * i)); if (outPos == outSize) goto done; }
            prev = (uint8_t)(dest >> 24);
        }
    }
done:
    if (outPos != outSize) { fprintf(stderr, "short output: %zu of %zu\n", outPos, outSize); return 1; }
    if (!(f = fopen(argv[2], "wb")) || fwrite(out, 1, outSize, f) != outSize || fclose(f)) { perror(argv[2]); return 1; }
    return 0;
bad:
    fprintf(stderr, "corrupt BCJ2 data\n");
    return 1;
}
