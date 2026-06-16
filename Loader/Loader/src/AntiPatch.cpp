#include "AntiPatch.h"

#include "Util.h"

#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace antipatch {

struct Region {
    const uint8_t* base;
    size_t         size;
    uint8_t        hash[32];
    char           name[16];
};

static std::vector<Region> g_regions;
static std::string         g_lastFault;
static std::atomic<bool>   g_initOk{ false };

static bool sha256(const void* data, size_t size, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE  alg = nullptr;
    BCRYPT_HASH_HANDLE h   = nullptr;
    NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (s != 0) return false;
    s = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (s != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    bool ok = (BCryptHashData(h, (PUCHAR)data, (ULONG)size, 0) == 0);
    if (ok) ok = (BCryptFinishHash(h, out, 32, 0) == 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool init() {
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return false;
    auto dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (PIMAGE_NT_HEADERS)((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto sec = IMAGE_FIRST_SECTION(nt);
    g_regions.clear();

    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (sec->Misc.VirtualSize == 0) continue;

        Region r{};
        r.base = (uint8_t*)mod + sec->VirtualAddress;
        r.size = sec->Misc.VirtualSize;
        memset(r.name, 0, sizeof(r.name));
        memcpy(r.name, sec->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (!sha256(r.base, r.size, r.hash)) {
            g_regions.clear();
            return false;
        }
        g_regions.push_back(r);
    }

    if (g_regions.empty()) return false;
    g_initOk.store(true);
    return true;
}

bool verify() {
    if (!g_initOk.load()) {
        g_lastFault = "not-initialized";
        return false;
    }
    uint8_t cur[32];
    for (const auto& r : g_regions) {
        if (!sha256(r.base, r.size, cur)) {
            g_lastFault = "hash-fail";
            return false;
        }
        if (memcmp(cur, r.hash, 32) != 0) {
            char buf[24] = {0};
            size_t nlen = strnlen_s(r.name, sizeof(r.name));
            memcpy(buf, r.name, nlen);
            g_lastFault.assign(buf);
            return false;
        }
    }
    return true;
}

const std::string& lastFault() { return g_lastFault; }

}
