#pragma once

#include <Windows.h>
#include <vector>
#include <cstdint>

namespace crypto {

bool aesGcmEncrypt(const uint8_t* key, size_t keyLen,
                   const uint8_t* iv,  size_t ivLen,
                   const uint8_t* aad, size_t aadLen,
                   const uint8_t* pt,  size_t ptLen,
                   std::vector<uint8_t>& ct,
                   uint8_t tag[16]);

__declspec(noinline) bool aesGcmDecrypt(const uint8_t* key, size_t keyLen,
                   const uint8_t* iv,  size_t ivLen,
                   const uint8_t* aad, size_t aadLen,
                   const uint8_t* ct,  size_t ctLen,
                   const uint8_t  tag[16],
                   std::vector<uint8_t>& pt);

bool sha256(const uint8_t* data, size_t len, uint8_t out[32]);

}
