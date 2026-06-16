#include "aes_gcm.h"

static const UCHAR g_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const UCHAR g_rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

#define AES_ROUNDS 14
#define AES_KEYSCHED_WORDS 60

static UCHAR XtimeMul(UCHAR a) {
    return (UCHAR)((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

static void KeyExpansion256(const UCHAR Key[32], UCHAR RoundKeys[AES_KEYSCHED_WORDS * 4]) {
    ULONG i;
    UCHAR temp[4];

    RtlCopyMemory(RoundKeys, Key, 32);

    for (i = 8; i < AES_KEYSCHED_WORDS; ++i) {
        temp[0] = RoundKeys[(i - 1) * 4 + 0];
        temp[1] = RoundKeys[(i - 1) * 4 + 1];
        temp[2] = RoundKeys[(i - 1) * 4 + 2];
        temp[3] = RoundKeys[(i - 1) * 4 + 3];

        if ((i % 8) == 0) {
            UCHAR t = temp[0];
            temp[0] = (UCHAR)(g_sbox[temp[1]] ^ g_rcon[i / 8]);
            temp[1] = g_sbox[temp[2]];
            temp[2] = g_sbox[temp[3]];
            temp[3] = g_sbox[t];
        } else if ((i % 8) == 4) {
            temp[0] = g_sbox[temp[0]];
            temp[1] = g_sbox[temp[1]];
            temp[2] = g_sbox[temp[2]];
            temp[3] = g_sbox[temp[3]];
        }

        RoundKeys[i * 4 + 0] = (UCHAR)(RoundKeys[(i - 8) * 4 + 0] ^ temp[0]);
        RoundKeys[i * 4 + 1] = (UCHAR)(RoundKeys[(i - 8) * 4 + 1] ^ temp[1]);
        RoundKeys[i * 4 + 2] = (UCHAR)(RoundKeys[(i - 8) * 4 + 2] ^ temp[2]);
        RoundKeys[i * 4 + 3] = (UCHAR)(RoundKeys[(i - 8) * 4 + 3] ^ temp[3]);
    }
}

static void AddRoundKey(UCHAR State[16], const UCHAR* RoundKey) {
    ULONG i;
    for (i = 0; i < 16; ++i) State[i] ^= RoundKey[i];
}

static void SubBytes(UCHAR State[16]) {
    ULONG i;
    for (i = 0; i < 16; ++i) State[i] = g_sbox[State[i]];
}

static void ShiftRows(UCHAR State[16]) {
    UCHAR t;
    t = State[1]; State[1] = State[5]; State[5] = State[9];  State[9]  = State[13]; State[13] = t;
    t = State[2]; State[2] = State[10]; State[10] = t;
    t = State[6]; State[6] = State[14]; State[14] = t;
    t = State[15]; State[15] = State[11]; State[11] = State[7]; State[7] = State[3]; State[3] = t;
}

static void MixColumns(UCHAR State[16]) {
    ULONG c;
    for (c = 0; c < 4; ++c) {
        UCHAR a0 = State[c * 4 + 0];
        UCHAR a1 = State[c * 4 + 1];
        UCHAR a2 = State[c * 4 + 2];
        UCHAR a3 = State[c * 4 + 3];
        UCHAR x = (UCHAR)(a0 ^ a1 ^ a2 ^ a3);
        UCHAR t = a0;
        State[c * 4 + 0] ^= (UCHAR)(x ^ XtimeMul((UCHAR)(a0 ^ a1)));
        State[c * 4 + 1] ^= (UCHAR)(x ^ XtimeMul((UCHAR)(a1 ^ a2)));
        State[c * 4 + 2] ^= (UCHAR)(x ^ XtimeMul((UCHAR)(a2 ^ a3)));
        State[c * 4 + 3] ^= (UCHAR)(x ^ XtimeMul((UCHAR)(a3 ^ t)));
    }
}

static void AesEncryptBlock(const UCHAR* RoundKeys, const UCHAR In[16], UCHAR Out[16]) {
    UCHAR state[16];
    ULONG round;

    RtlCopyMemory(state, In, 16);
    AddRoundKey(state, RoundKeys);

    for (round = 1; round < AES_ROUNDS; ++round) {
        SubBytes(state);
        ShiftRows(state);
        MixColumns(state);
        AddRoundKey(state, RoundKeys + round * 16);
    }

    SubBytes(state);
    ShiftRows(state);
    AddRoundKey(state, RoundKeys + AES_ROUNDS * 16);

    RtlCopyMemory(Out, state, 16);
}

static void GfMul128(const UCHAR X[16], const UCHAR Y[16], UCHAR Z[16]) {
    UCHAR v[16];
    UCHAR z[16];
    ULONG i;
    int bit;

    RtlZeroMemory(z, 16);
    RtlCopyMemory(v, Y, 16);

    for (i = 0; i < 16; ++i) {
        for (bit = 7; bit >= 0; --bit) {
            if ((X[i] >> bit) & 1) {
                ULONG j;
                for (j = 0; j < 16; ++j) z[j] ^= v[j];
            }
            {
                int lsb = v[15] & 1;
                int j;
                for (j = 15; j > 0; --j) {
                    v[j] = (UCHAR)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
                }
                v[0] >>= 1;
                if (lsb) v[0] ^= 0xe1;
            }
        }
    }

    RtlCopyMemory(Z, z, 16);
}

static void GhashUpdate(const UCHAR H[16], UCHAR Y[16], const UCHAR* Data, ULONG Len) {
    ULONG i;
    UCHAR block[16];

    while (Len >= 16) {
        for (i = 0; i < 16; ++i) Y[i] ^= Data[i];
        GfMul128(Y, H, Y);
        Data += 16;
        Len -= 16;
    }

    if (Len > 0) {
        RtlZeroMemory(block, 16);
        RtlCopyMemory(block, Data, Len);
        for (i = 0; i < 16; ++i) Y[i] ^= block[i];
        GfMul128(Y, H, Y);
    }
}

static void IncrementCounter(UCHAR Ctr[16]) {
    int i;
    for (i = 15; i >= 12; --i) {
        Ctr[i] = (UCHAR)(Ctr[i] + 1);
        if (Ctr[i] != 0) return;
    }
}

static void Be64(UCHAR* Out, ULONGLONG Value) {
    Out[0] = (UCHAR)(Value >> 56);
    Out[1] = (UCHAR)(Value >> 48);
    Out[2] = (UCHAR)(Value >> 40);
    Out[3] = (UCHAR)(Value >> 32);
    Out[4] = (UCHAR)(Value >> 24);
    Out[5] = (UCHAR)(Value >> 16);
    Out[6] = (UCHAR)(Value >> 8);
    Out[7] = (UCHAR)(Value);
}

NTSTATUS AesGcmDecrypt(
    const UCHAR* Key,
    const UCHAR* Iv,
    const UCHAR* Aad, ULONG AadLen,
    const UCHAR* CipherText, ULONG CipherLen,
    const UCHAR  Tag[AES_GCM_TAG_SIZE],
    UCHAR*       PlainTextOut)
{
    UCHAR roundKeys[AES_KEYSCHED_WORDS * 4];
    UCHAR H[16];
    UCHAR J0[16];
    UCHAR Ctr[16];
    UCHAR S[16];
    UCHAR computedTag[16];
    UCHAR lenBlock[16];
    UCHAR keystream[16];
    UCHAR zero[16];
    ULONG i;
    ULONG remaining;
    UCHAR diff;

    if (Key == NULL || Iv == NULL || CipherText == NULL || Tag == NULL || PlainTextOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeyExpansion256(Key, roundKeys);

    RtlZeroMemory(zero, 16);
    AesEncryptBlock(roundKeys, zero, H);

    RtlZeroMemory(J0, 16);
    RtlCopyMemory(J0, Iv, AES_GCM_IV_SIZE);
    J0[15] = 0x01;

    RtlZeroMemory(S, 16);

    if (Aad && AadLen) {
        GhashUpdate(H, S, Aad, AadLen);
    }

    if (CipherLen) {
        GhashUpdate(H, S, CipherText, CipherLen);
    }

    Be64(lenBlock + 0, (ULONGLONG)AadLen * 8);
    Be64(lenBlock + 8, (ULONGLONG)CipherLen * 8);
    GhashUpdate(H, S, lenBlock, 16);

    AesEncryptBlock(roundKeys, J0, computedTag);
    for (i = 0; i < 16; ++i) computedTag[i] ^= S[i];

    diff = 0;
    for (i = 0; i < AES_GCM_TAG_SIZE; ++i) diff |= (UCHAR)(computedTag[i] ^ Tag[i]);

    if (diff != 0) {
        RtlSecureZeroMemory(roundKeys, sizeof(roundKeys));
        RtlSecureZeroMemory(H, sizeof(H));
        RtlSecureZeroMemory(computedTag, sizeof(computedTag));
        return STATUS_AUTH_TAG_MISMATCH;
    }

    RtlCopyMemory(Ctr, J0, 16);
    IncrementCounter(Ctr);

    remaining = CipherLen;
    while (remaining > 0) {
        ULONG take = remaining > 16 ? 16 : remaining;
        AesEncryptBlock(roundKeys, Ctr, keystream);
        for (i = 0; i < take; ++i) {
            PlainTextOut[i] = (UCHAR)(CipherText[i] ^ keystream[i]);
        }
        CipherText += take;
        PlainTextOut += take;
        remaining -= take;
        IncrementCounter(Ctr);
    }

    RtlSecureZeroMemory(roundKeys, sizeof(roundKeys));
    RtlSecureZeroMemory(H, sizeof(H));
    RtlSecureZeroMemory(keystream, sizeof(keystream));
    RtlSecureZeroMemory(computedTag, sizeof(computedTag));
    RtlSecureZeroMemory(S, sizeof(S));
    RtlSecureZeroMemory(J0, sizeof(J0));
    RtlSecureZeroMemory(Ctr, sizeof(Ctr));

    return STATUS_SUCCESS;
}
