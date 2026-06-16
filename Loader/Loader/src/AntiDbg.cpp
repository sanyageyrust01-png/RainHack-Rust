#include "AntiDbg.h"
#include "ApiResolve.h"
#include "Util.h"

#include <Windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstring>

namespace antidbg {

typedef NTSTATUS (NTAPI* fn_NtQueryInformationProcess)(
    HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI* fn_NtSetInformationThread)(
    HANDLE, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI* fn_NtClose)(HANDLE);

static PEB* peb() {
#if defined(_M_X64) || defined(__x86_64__)
    return (PEB*)__readgsqword(0x60);
#else
    return (PEB*)__readfsdword(0x30);
#endif
}

static bool checkPeb() {
    uint8_t* p = (uint8_t*)peb();
    if (!p) return true;
    if (p[0x02]) return false;

#if defined(_M_X64) || defined(__x86_64__)
    DWORD ntFlag = *(DWORD*)(p + 0xBC);
    uint8_t* heap = *(uint8_t**)(p + 0x30);
    size_t hfOff = 0x70, hForceOff = 0x74;
#else
    DWORD ntFlag = *(DWORD*)(p + 0x68);
    uint8_t* heap = *(uint8_t**)(p + 0x18);
    size_t hfOff = 0x40, hForceOff = 0x44;
#endif
    if (ntFlag & 0x70) return false;

    if (heap) {
        DWORD hFlags = *(DWORD*)(heap + hfOff);
        DWORD hForce = *(DWORD*)(heap + hForceOff);
        if ((hFlags & ~(DWORD)2) != 0 || hForce != 0) return false;
    }
    return true;
}

static bool checkDebugPort() {
    auto fn = (fn_NtQueryInformationProcess)apiresolve::NtDll(
        apiresolve::HashCt("NtQueryInformationProcess"));
    if (!fn) return true;

    HANDLE port = nullptr;
    ULONG ret = 0;
    if (fn(GetCurrentProcess(), (PROCESSINFOCLASS)7,
           &port, sizeof(port), &ret) == 0) {
        if (port != nullptr) return false;
    }

    HANDLE dbgObj = nullptr;
    if (fn(GetCurrentProcess(), (PROCESSINFOCLASS)0x1E,
           &dbgObj, sizeof(dbgObj), &ret) == 0) {
        if (dbgObj != nullptr) return false;
    }

    ULONG dbgFlags = 0;
    if (fn(GetCurrentProcess(), (PROCESSINFOCLASS)0x1F,
           &dbgFlags, sizeof(dbgFlags), &ret) == 0) {
        if (dbgFlags == 0) return false;
    }
    return true;
}

static bool checkRemoteDebugger() {
    BOOL dbg = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &dbg);
    return !dbg;
}

static bool checkDrRegs() {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &ctx)) return true;
    if (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3) return false;
    return true;
}

bool VerifyHard() {
    bool ok = true;
    if (!checkPeb())            { util::logf("antidbg: PEB tripped");   ok = false; }
    if (!checkDebugPort())      { util::logf("antidbg: debug port");    ok = false; }
    if (!checkRemoteDebugger()) { util::logf("antidbg: remote dbg");    ok = false; }
    if (!checkDrRegs())         { util::logf("antidbg: HW bp present"); ok = false; }
    return ok;
}

void TrySoftHardening() {
    auto setInfo = (fn_NtSetInformationThread)apiresolve::NtDll(
        apiresolve::HashCt("NtSetInformationThread"));
    if (setInfo) {
        setInfo(GetCurrentThread(), 0x11, nullptr, 0);
    }

    typedef struct _LDR_DATA_TABLE_ENTRY_FULL {
        LIST_ENTRY     InLoadOrderLinks;
        LIST_ENTRY     InMemoryOrderLinks;
        LIST_ENTRY     InInitializationOrderLinks;
        PVOID          DllBase;
        PVOID          EntryPoint;
        ULONG          SizeOfImage;
        UNICODE_STRING FullDllName;
        UNICODE_STRING BaseDllName;
    } LDR_ENTRY, *PLDR_ENTRY;

    static const uint32_t kSuspect[] = {
        apiresolve::HashCt("SCYLLAHIDE.DLL"),
        apiresolve::HashCt("FRIDA-GADGET.DLL"),
        apiresolve::HashCt("FRIDA-AGENT.DLL"),
        apiresolve::HashCt("WINLICENSE.DLL"),
        apiresolve::HashCt("HYPERHIDE.DLL"),
    };

    PEB* p = peb();
    if (!p || !p->Ldr) return;

    LIST_ENTRY* head = &p->Ldr->InMemoryOrderModuleList;
    LIST_ENTRY* cur  = head->Flink;
    while (cur && cur != head) {
        PLDR_ENTRY e = CONTAINING_RECORD(cur, LDR_ENTRY, InMemoryOrderLinks);
        if (e->BaseDllName.Buffer && e->BaseDllName.Length) {
            wchar_t up[128]; size_t n = e->BaseDllName.Length / sizeof(wchar_t);
            if (n >= 128) n = 127;
            for (size_t i = 0; i < n; ++i) {
                wchar_t c = e->BaseDllName.Buffer[i];
                if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
                up[i] = c;
            }
            up[n] = 0;
            uint32_t h = apiresolve::HashRtW(up);
            for (uint32_t s : kSuspect) {
                if (h == s) {
                    util::logf("antidbg: suspect module present (h=%08X)", h);
                }
            }
        }
        cur = cur->Flink;
    }
}

}
