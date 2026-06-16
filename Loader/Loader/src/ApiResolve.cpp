#include "ApiResolve.h"

#include <winternl.h>
#include <cstring>

namespace apiresolve {

typedef struct _LDR_DATA_TABLE_ENTRY_FULL {
    LIST_ENTRY      InLoadOrderLinks;
    LIST_ENTRY      InMemoryOrderLinks;
    LIST_ENTRY      InInitializationOrderLinks;
    PVOID           DllBase;
    PVOID           EntryPoint;
    ULONG           SizeOfImage;
    UNICODE_STRING  FullDllName;
    UNICODE_STRING  BaseDllName;
} LDR_DATA_TABLE_ENTRY_FULL, *PLDR_DATA_TABLE_ENTRY_FULL;

static inline uint32_t ror13(uint32_t v) {
    return (v >> 13) | (v << (32 - 13));
}

uint32_t HashRt(const char* s) {
    uint32_t h = 0;
    while (s && *s) {
        h = ror13(h);
        h += (uint8_t)(*s);
        ++s;
    }
    return h;
}

uint32_t HashRtW(const wchar_t* s) {
    uint32_t h = 0;
    while (s && *s) {
        wchar_t c = *s;
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        h = ror13(h);
        h += (uint8_t)(c & 0xFF);
        h = ror13(h);
        h += (uint8_t)((c >> 8) & 0xFF);
        ++s;
    }
    return h;
}

static PEB* GetPeb() {
#if defined(_M_X64) || defined(__x86_64__)
    return (PEB*)__readgsqword(0x60);
#else
    return (PEB*)__readfsdword(0x30);
#endif
}

void* FindModule(uint32_t nameHashUppercaseW) {
    PEB* peb = GetPeb();
    if (!peb || !peb->Ldr) return nullptr;

    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    LIST_ENTRY* cur  = head->Flink;

    while (cur != head) {
        PLDR_DATA_TABLE_ENTRY_FULL e =
            CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY_FULL, InMemoryOrderLinks);

        if (e->BaseDllName.Buffer && e->BaseDllName.Length) {
            wchar_t tmp[128];
            size_t n = e->BaseDllName.Length / sizeof(wchar_t);
            if (n >= 128) n = 127;
            for (size_t i = 0; i < n; ++i) {
                wchar_t c = e->BaseDllName.Buffer[i];
                if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
                tmp[i] = c;
            }
            tmp[n] = 0;
            if (HashRtW(tmp) == nameHashUppercaseW) {
                return e->DllBase;
            }
        }
        cur = cur->Flink;
    }
    return nullptr;
}

void* GetExport(void* moduleBase, uint32_t apiNameHash) {
    if (!moduleBase) return nullptr;
    uint8_t* base = (uint8_t*)moduleBase;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    DWORD expRva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD expSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (!expRva || !expSize) return nullptr;

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + expRva);
    DWORD* names   = (DWORD*)(base + exp->AddressOfNames);
    WORD*  ordinals= (WORD*) (base + exp->AddressOfNameOrdinals);
    DWORD* funcs   = (DWORD*)(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = (const char*)(base + names[i]);
        if (HashRt(name) == apiNameHash) {
            WORD  ord   = ordinals[i];
            DWORD fnRva = funcs[ord];
            if (fnRva >= expRva && fnRva < expRva + expSize) {
                return nullptr;
            }
            return base + fnRva;
        }
    }
    return nullptr;
}

static void* s_ntdll    = nullptr;
static void* s_kernel32 = nullptr;

void* NtDll(uint32_t apiNameHash) {
    if (!s_ntdll) s_ntdll = FindModule(HashCt("NTDLL.DLL"));
    return GetExport(s_ntdll, apiNameHash);
}

void* Kernel32(uint32_t apiNameHash) {
    if (!s_kernel32) s_kernel32 = FindModule(HashCt("KERNEL32.DLL"));
    return GetExport(s_kernel32, apiNameHash);
}

static IMAGE_RESOURCE_DIRECTORY_ENTRY* findEntry(
    IMAGE_RESOURCE_DIRECTORY* dir, uint32_t id)
{
    IMAGE_RESOURCE_DIRECTORY_ENTRY* entries =
        (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(dir + 1);
    WORD total = (WORD)(dir->NumberOfNamedEntries + dir->NumberOfIdEntries);
    for (WORD i = 0; i < total; ++i) {
        if (entries[i].NameIsString) continue;
        if (entries[i].Id == id) return &entries[i];
    }
    return nullptr;
}

const uint8_t* FindOwnResource(int typeId, int resId, uint32_t* outSize) {
    if (outSize) *outSize = 0;

    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return nullptr;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    DWORD rsrcRva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
    DWORD rsrcSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size;
    if (!rsrcRva || !rsrcSize) return nullptr;

    IMAGE_RESOURCE_DIRECTORY* root = (IMAGE_RESOURCE_DIRECTORY*)(base + rsrcRva);

    IMAGE_RESOURCE_DIRECTORY_ENTRY* typeEntry = findEntry(root, (uint32_t)typeId);
    if (!typeEntry || !typeEntry->DataIsDirectory) return nullptr;

    IMAGE_RESOURCE_DIRECTORY* typeDir =
        (IMAGE_RESOURCE_DIRECTORY*)(base + rsrcRva + typeEntry->OffsetToDirectory);

    IMAGE_RESOURCE_DIRECTORY_ENTRY* idEntry = findEntry(typeDir, (uint32_t)resId);
    if (!idEntry || !idEntry->DataIsDirectory) return nullptr;

    IMAGE_RESOURCE_DIRECTORY* idDir =
        (IMAGE_RESOURCE_DIRECTORY*)(base + rsrcRva + idEntry->OffsetToDirectory);

    if (idDir->NumberOfNamedEntries + idDir->NumberOfIdEntries == 0) return nullptr;

    IMAGE_RESOURCE_DIRECTORY_ENTRY* langEntry =
        (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(idDir + 1);
    if (langEntry->DataIsDirectory) return nullptr;

    IMAGE_RESOURCE_DATA_ENTRY* data =
        (IMAGE_RESOURCE_DATA_ENTRY*)(base + rsrcRva + langEntry->OffsetToData);

    if (outSize) *outSize = data->Size;
    return base + data->OffsetToData;
}

uint32_t SeedC() {
    uint32_t a = HashRt("NtProtectVirtualMemory");
    uint32_t b = HashRt("RtlImageNtHeader");
    uint32_t c = HashRt("NtQueryInformationProcess");
    uint32_t d = HashRt("RtlDecodePointer");
    return (a ^ (b + 0x9E3779B9)) ^ ((c << 7) | (c >> 25)) ^ (d ^ 0xC0DEC0DE);
}

}
