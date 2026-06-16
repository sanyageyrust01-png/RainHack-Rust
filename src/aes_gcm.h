#pragma once

#include <ntifs.h>

#define AES_GCM_KEY_SIZE   32
#define AES_GCM_IV_SIZE    12
#define AES_GCM_TAG_SIZE   16
#define AES_GCM_BLOCK_SIZE 16

NTSTATUS AesGcmDecrypt(
    const UCHAR* Key,
    const UCHAR* Iv,
    const UCHAR* Aad, ULONG AadLen,
    const UCHAR* CipherText, ULONG CipherLen,
    const UCHAR  Tag[AES_GCM_TAG_SIZE],
    UCHAR*       PlainTextOut);
