#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace apiresolve {

constexpr uint32_t HashCt(const char* s) {
    uint32_t h = 0;
    while (*s) {
        h = (h >> 13) | (h << (32 - 13));
        h += (uint8_t)(*s);
        ++s;
    }
    return h;
}

uint32_t HashRt(const char* s);
uint32_t HashRtW(const wchar_t* s);

void* FindModule(uint32_t nameHashUppercaseW);
void* GetExport(void* moduleBase, uint32_t apiNameHash);

void* NtDll(uint32_t apiNameHash);
void* Kernel32(uint32_t apiNameHash);

const uint8_t* FindOwnResource(int typeId, int resId, uint32_t* outSize);

uint32_t SeedC();

}
