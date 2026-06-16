#include "Resources.h"
#include "Crypto.h"
#include "Util.h"
#include "ApiResolve.h"
#include "ChaCha20.h"

#include <Windows.h>
#include <cstring>
#include <mutex>

#define RT_OPAQUE_BLOB   256
#define IDR_CHEAT_BLOB   0x7A1B
#define IDR_DRIVER_BLOB  0x3E9F
#define IDR_KDMAPPER_BLOB 0x5C2D

#define BLOB_MAGIC_0 'R'
#define BLOB_MAGIC_1 'H'
#define BLOB_MAGIC_2 'v'
#define BLOB_MAGIC_3 '3'

#define BLOB_HEADER_SIZE (4 + 12 + 16 + 12 + 4 + 4)

namespace resources {

static const uint8_t kKeyA[8] = { 0x12, 0xA4, 0x77, 0xC9, 0x3E, 0xB1, 0x58, 0x6F };
static const uint8_t kKeyB[8] = { 0x83, 0x55, 0xE0, 0x29, 0xDD, 0x47, 0x91, 0x3A };
static const uint8_t kKeyC[8] = { 0x6E, 0xCB, 0x14, 0x77, 0xA8, 0x02, 0x59, 0xBC };
static const uint8_t kKeyD[8] = { 0xF1, 0x36, 0x8B, 0x4D, 0x07, 0x90, 0xE3, 0x52 };

static const uint8_t kMaskA[8] = { 0x68, 0x9B, 0xC3, 0x0B, 0xDF, 0xB8, 0x05, 0xE7 };
static const uint8_t kMaskB[8] = { 0xCC, 0xF7, 0x37, 0x42, 0x0C, 0x76, 0x80, 0x22 };
static const uint8_t kMaskC[8] = { 0x0E, 0x69, 0xC3, 0x1C, 0x62, 0x71, 0x57, 0xE5 };
static const uint8_t kMaskD[8] = { 0x5D, 0x33, 0x7A, 0x06, 0xC6, 0xE3, 0x12, 0x65 };

static std::once_flag g_keyOnce;
static uint8_t        g_sessionKey[32] = { 0 };

static void buildSessionKey() {
    for (int i = 0; i < 8; ++i) {
        g_sessionKey[ 0 + i] = (uint8_t)(kKeyA[i] ^ kMaskA[i]);
        g_sessionKey[ 8 + i] = (uint8_t)(kKeyB[i] ^ kMaskB[i]);
        g_sessionKey[16 + i] = (uint8_t)(kKeyC[i] ^ kMaskC[i]);
        g_sessionKey[24 + i] = (uint8_t)(kKeyD[i] ^ kMaskD[i]);
    }
}

const uint8_t* cheatSessionKey() {
    std::call_once(g_keyOnce, buildSessionKey);
    return g_sessionKey;
}

size_t cheatSessionKeyLen() { return 32; }

static const uint8_t kOuterAa[8] = { 0x3F, 0x9C, 0x14, 0x77, 0xA2, 0x0B, 0xDE, 0x51 };
static const uint8_t kOuterAb[8] = { 0xC7, 0x55, 0xE0, 0x29, 0xDD, 0x47, 0x91, 0x3A };
static const uint8_t kOuterAc[8] = { 0x6E, 0xCB, 0x14, 0x77, 0xA8, 0x02, 0x59, 0xBC };
static const uint8_t kOuterAd[8] = { 0xF1, 0x36, 0x8B, 0x4D, 0x07, 0x90, 0xE3, 0x52 };

static const uint8_t kOuterAmA[8] = { 0x11, 0x22, 0x44, 0x66, 0x88, 0xAA, 0xCC, 0xEE };
static const uint8_t kOuterAmB[8] = { 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10 };
static const uint8_t kOuterAmC[8] = { 0x5A, 0xA5, 0x3C, 0xC3, 0x69, 0x96, 0x71, 0x8E };
static const uint8_t kOuterAmD[8] = { 0xB3, 0x4D, 0x72, 0x61, 0x55, 0xEE, 0xF0, 0x11 };

static uint8_t g_outerA[32] = { 0 };
static uint8_t g_outerB[32] = { 0 };
static std::once_flag g_outerOnce;

static void buildOuterKeys() {
    for (int i = 0; i < 8; ++i) {
        g_outerA[ 0 + i] = (uint8_t)(kOuterAa[i] ^ kOuterAmA[i]);
        g_outerA[ 8 + i] = (uint8_t)(kOuterAb[i] ^ kOuterAmB[i]);
        g_outerA[16 + i] = (uint8_t)(kOuterAc[i] ^ kOuterAmC[i]);
        g_outerA[24 + i] = (uint8_t)(kOuterAd[i] ^ kOuterAmD[i]);
    }

    volatile uint8_t k[32];
    k[ 0] = (uint8_t)(0x17 ^ 0x40);
    k[ 1] = (uint8_t)((0xB2 + 0x11) & 0xFF);
    k[ 2] = (uint8_t)(0xC3 ^ 0x2A);
    k[ 3] = (uint8_t)(((0x5F << 1) | (0x5F >> 7)) & 0xFF);
    k[ 4] = (uint8_t)(0x7E ^ 0x19);
    k[ 5] = (uint8_t)(0x88 + 0x07);
    k[ 6] = (uint8_t)(0x91 ^ 0xAA);
    k[ 7] = (uint8_t)(0x0F ^ 0xF0);
    k[ 8] = (uint8_t)(0xA1 ^ 0x1A);
    k[ 9] = (uint8_t)(0x22 + 0x33);
    k[10] = (uint8_t)(0xBB ^ 0x44);
    k[11] = (uint8_t)(0xCC ^ 0xDD);
    k[12] = (uint8_t)(0x12 ^ 0x77);
    k[13] = (uint8_t)(0x84 + 0x05);
    k[14] = (uint8_t)(0x65 ^ 0x89);
    k[15] = (uint8_t)(0xEF ^ 0x10);
    k[16] = (uint8_t)(0x3C ^ 0x51);
    k[17] = (uint8_t)(0xD4 + 0x09);
    k[18] = (uint8_t)(0x79 ^ 0xB2);
    k[19] = (uint8_t)(0x46 ^ 0x2A);
    k[20] = (uint8_t)(0x8D ^ 0x71);
    k[21] = (uint8_t)(0x1F + 0x33);
    k[22] = (uint8_t)(0xE7 ^ 0xC5);
    k[23] = (uint8_t)(0x58 ^ 0x39);
    k[24] = (uint8_t)(0x62 ^ 0xB8);
    k[25] = (uint8_t)(0xAD + 0x0C);
    k[26] = (uint8_t)(0x30 ^ 0x4F);
    k[27] = (uint8_t)(0xF2 ^ 0x8E);
    k[28] = (uint8_t)(0x09 ^ 0x73);
    k[29] = (uint8_t)(0xC4 + 0x1A);
    k[30] = (uint8_t)(0x5E ^ 0x27);
    k[31] = (uint8_t)(0xB1 ^ 0x0D);
    for (int i = 0; i < 32; ++i) g_outerB[i] = k[i];
}

static void xorStream(uint8_t* buf, size_t len, uint32_t seed) {
    uint32_t s = seed ? seed : 0xDEADBEEF;
    for (size_t i = 0; i < len; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        buf[i] ^= (uint8_t)(s & 0xFF);
    }
}

static bool loadResourceOpaque(int resId, std::vector<uint8_t>& out) {
    uint32_t sz = 0;
    const uint8_t* p = apiresolve::FindOwnResource(RT_OPAQUE_BLOB, resId, &sz);

    if (!p || !sz) {
        HMODULE self = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resId),
                                  MAKEINTRESOURCEW(RT_OPAQUE_BLOB));
        if (!res) {
            util::logf("res 0x%X: walk+FindResource failed (le=%lu)",
                       resId, GetLastError());
            return false;
        }
        DWORD resSz = SizeofResource(self, res);
        HGLOBAL glob = LoadResource(self, res);
        if (!glob || !resSz) {
            util::logf("res 0x%X: LoadResource failed", resId);
            return false;
        }
        const void* data = LockResource(glob);
        if (!data) {
            util::logf("res 0x%X: LockResource failed", resId);
            return false;
        }
        out.assign((const uint8_t*)data, (const uint8_t*)data + resSz);
        return true;
    }
    out.assign(p, p + sz);
    return true;
}

static bool decryptOuter3Layer(const std::vector<uint8_t>& blob,
                               std::vector<uint8_t>& out) {
    if (blob.size() < BLOB_HEADER_SIZE) {
        util::logf("blob: too small (%zu)", blob.size());
        return false;
    }
    if (blob[0] != BLOB_MAGIC_0 || blob[1] != BLOB_MAGIC_1 ||
        blob[2] != BLOB_MAGIC_2 || blob[3] != BLOB_MAGIC_3) {
        util::logf("blob: bad magic");
        return false;
    }

    const uint8_t* iv_A   = blob.data() + 4;
    const uint8_t* tag_A  = blob.data() + 4 + 12;
    const uint8_t* nonceB = blob.data() + 4 + 12 + 16;
    uint32_t saltC = *(const uint32_t*)(blob.data() + 4 + 12 + 16 + 12);
    uint32_t origLen = *(const uint32_t*)(blob.data() + 4 + 12 + 16 + 12 + 4);
    const uint8_t* ct_A = blob.data() + BLOB_HEADER_SIZE;
    size_t ct_A_len = blob.size() - BLOB_HEADER_SIZE;

    std::call_once(g_outerOnce, buildOuterKeys);

    std::vector<uint8_t> mid;
    if (!crypto::aesGcmDecrypt(g_outerA, 32, iv_A, 12, nullptr, 0,
                               ct_A, ct_A_len, tag_A, mid)) {
        util::logf("blob: AES-GCM outer decrypt failed");
        return false;
    }

    std::vector<uint8_t> inner(mid.size());
    chacha20::XCrypt(g_outerB, nonceB, 0, mid.data(), inner.data(), mid.size());
    SecureZeroMemory(mid.data(), mid.size());

    uint32_t seed = apiresolve::SeedC() ^ saltC;
    xorStream(inner.data(), inner.size(), seed);

    if (origLen > inner.size()) {
        util::logf("blob: origLen(%u) > inner(%zu)", origLen, inner.size());
        SecureZeroMemory(inner.data(), inner.size());
        return false;
    }

    out.assign(inner.begin(), inner.begin() + origLen);
    SecureZeroMemory(inner.data(), inner.size());
    return true;
}

bool loadCheatBlob(std::vector<uint8_t>& outEncryptedBlob) {
    std::vector<uint8_t> outer;
    if (!loadResourceOpaque(IDR_CHEAT_BLOB, outer)) return false;

    bool ok = decryptOuter3Layer(outer, outEncryptedBlob);
    SecureZeroMemory(outer.data(), outer.size());

    if (!ok) {
        util::logf("cheat blob: outer decrypt failed");
        outEncryptedBlob.clear();
        return false;
    }
    if (outEncryptedBlob.size() < 12 + 16 + 1) {
        util::logf("cheat blob: inner too small (%zu)", outEncryptedBlob.size());
        outEncryptedBlob.clear();
        return false;
    }
    return true;
}

bool loadDriverSys(std::vector<uint8_t>& outDecrypted) {
    std::vector<uint8_t> outer;
    if (!loadResourceOpaque(IDR_DRIVER_BLOB, outer)) return false;

    bool ok = decryptOuter3Layer(outer, outDecrypted);
    SecureZeroMemory(outer.data(), outer.size());

    if (!ok) {
        util::logf("driver blob: outer decrypt failed");
        return false;
    }
    return true;
}

bool loadKdmapperExe(std::vector<uint8_t>& outDecrypted) {
    std::vector<uint8_t> outer;
    if (!loadResourceOpaque(IDR_KDMAPPER_BLOB, outer)) return false;

    bool ok = decryptOuter3Layer(outer, outDecrypted);
    SecureZeroMemory(outer.data(), outer.size());

    if (!ok) {
        util::logf("kdmapper blob: outer decrypt failed");
        return false;
    }
    if (outDecrypted.size() < 64 ||
        outDecrypted[0] != 'M' || outDecrypted[1] != 'Z') {
        util::logf("kdmapper blob: not an MZ (size=%zu)", outDecrypted.size());
        SecureZeroMemory(outDecrypted.data(), outDecrypted.size());
        outDecrypted.clear();
        return false;
    }
    return true;
}

}
