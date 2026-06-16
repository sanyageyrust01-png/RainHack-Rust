#include "Crypto.h"
#include "Util.h"
#include "VMProtectSDK.h"

#include <bcrypt.h>
#include <ntstatus.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L)
#endif

namespace crypto {

struct AesGcmKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~AesGcmKey() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool init(const uint8_t* keyBytes, size_t keyLen) {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) return false;
        if (BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)keyBytes, (ULONG)keyLen, 0) != 0) return false;
        return true;
    }
};

bool aesGcmEncrypt(const uint8_t* key, size_t keyLen,
                   const uint8_t* iv,  size_t ivLen,
                   const uint8_t* aad, size_t aadLen,
                   const uint8_t* pt,  size_t ptLen,
                   std::vector<uint8_t>& ct,
                   uint8_t tag[16]) {
    AesGcmKey k;
    if (!k.init(key, keyLen)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)iv;
    info.cbNonce = (ULONG)ivLen;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = tag;
    info.cbTag = 16;

    ct.resize(ptLen);
    ULONG cb = 0;
    NTSTATUS s = BCryptEncrypt(k.key, (PUCHAR)pt, (ULONG)ptLen, &info, nullptr, 0,
                               ct.data(), (ULONG)ct.size(), &cb, 0);
    if (s != 0) return false;
    ct.resize(cb);
    return true;
}

__declspec(noinline) bool aesGcmDecrypt(const uint8_t* key, size_t keyLen,
                   const uint8_t* iv,  size_t ivLen,
                   const uint8_t* aad, size_t aadLen,
                   const uint8_t* ct,  size_t ctLen,
                   const uint8_t  tag[16],
                   std::vector<uint8_t>& pt) {
    VMP_BEGIN_VIRTUALIZATION("crypto.aesGcmDecrypt");
    AesGcmKey k;
    if (!k.init(key, keyLen)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)iv;
    info.cbNonce = (ULONG)ivLen;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = (ULONG)aadLen;
    info.pbTag = (PUCHAR)tag;
    info.cbTag = 16;

    pt.resize(ctLen);
    ULONG cb = 0;
    NTSTATUS s = BCryptDecrypt(k.key, (PUCHAR)ct, (ULONG)ctLen, &info, nullptr, 0,
                               pt.data(), (ULONG)pt.size(), &cb, 0);
    if (s != 0) return false;
    pt.resize(cb);
    return true;
}

bool sha256(const uint8_t* data, size_t len, uint8_t outDigest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    do {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) break;
        if (data && len) {
            if (BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) != 0) break;
        }
        if (BCryptFinishHash(h, outDigest, 32, 0) != 0) break;
        ok = true;
    } while (false);
    if (h)   BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}
