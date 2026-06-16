#include "ChaCha20.h"

#include <cstring>

namespace chacha20 {

static inline uint32_t rotl32(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

static inline uint32_t le32(const uint8_t* p) {
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

#define QR(a,b,c,d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d,  8); \
    c += d; b ^= c; b = rotl32(b,  7);

static void block(const uint32_t input[16], uint8_t out[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = input[i];

    for (int i = 0; i < 10; ++i) {
        QR(x[ 0], x[ 4], x[ 8], x[12]);
        QR(x[ 1], x[ 5], x[ 9], x[13]);
        QR(x[ 2], x[ 6], x[10], x[14]);
        QR(x[ 3], x[ 7], x[11], x[15]);

        QR(x[ 0], x[ 5], x[10], x[15]);
        QR(x[ 1], x[ 6], x[11], x[12]);
        QR(x[ 2], x[ 7], x[ 8], x[13]);
        QR(x[ 3], x[ 4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t v = x[i] + input[i];
        out[i * 4 + 0] = (uint8_t)(v       & 0xFF);
        out[i * 4 + 1] = (uint8_t)((v >> 8)  & 0xFF);
        out[i * 4 + 2] = (uint8_t)((v >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
}

void XCrypt(const uint8_t key[32],
            const uint8_t nonce[12],
            uint32_t counter,
            const uint8_t* in,
            uint8_t* out,
            size_t len)
{
    uint32_t st[16];
    st[0] = 0x61707865; // "expa"
    st[1] = 0x3320646E; // "nd 3"
    st[2] = 0x79622D32; // "2-by"
    st[3] = 0x6B206574; // "te k"

    for (int i = 0; i < 8; ++i) st[4 + i] = le32(key + i * 4);

    st[12] = counter;
    st[13] = le32(nonce + 0);
    st[14] = le32(nonce + 4);
    st[15] = le32(nonce + 8);

    uint8_t stream[64];
    size_t off = 0;
    while (off < len) {
        block(st, stream);
        st[12]++;

        size_t take = len - off;
        if (take > 64) take = 64;
        for (size_t i = 0; i < take; ++i) {
            out[off + i] = in[off + i] ^ stream[i];
        }
        off += take;
    }

    volatile uint8_t* z = stream;
    for (int i = 0; i < 64; ++i) z[i] = 0;
}

#undef QR

}
