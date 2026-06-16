#include "AntiDebug.h"
#include "Util.h"

#include <intrin.h>
#include <winternl.h>

#pragma comment(lib, "ntdll.lib")

namespace antidebug {

static const PROCESSINFOCLASS kProcessDebugPort         = (PROCESSINFOCLASS)0x07;
static const PROCESSINFOCLASS kProcessDebugObjectHandle = (PROCESSINFOCLASS)0x1E;
static const PROCESSINFOCLASS kProcessDebugFlags        = (PROCESSINFOCLASS)0x1F;
static const THREADINFOCLASS  kHideThreadFromDebugger   = (THREADINFOCLASS)0x11;
static const SYSTEM_INFORMATION_CLASS kSystemKernelDebuggerInformation = (SYSTEM_INFORMATION_CLASS)0x23;

static bool checkPebBeingDebugged() {
#ifdef _M_X64
    auto peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return false;
    return peb[0x02] != 0;
#else
    auto peb = (BYTE*)__readfsdword(0x30);
    if (!peb) return false;
    return peb[0x02] != 0;
#endif
}

static bool checkPebNtGlobalFlag() {
#ifdef _M_X64
    auto peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return false;
    DWORD flag = *(DWORD*)(peb + 0xBC);
    return (flag & 0x70) == 0x70;
#else
    auto peb = (BYTE*)__readfsdword(0x30);
    if (!peb) return false;
    DWORD flag = *(DWORD*)(peb + 0x68);
    return (flag & 0x70) == 0x70;
#endif
}

static bool checkPebHeapFlags() {
#ifdef _M_X64
    auto peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return false;
    auto heap = *(BYTE**)(peb + 0x30);
    if (!heap) return false;
    DWORD f1 = *(DWORD*)(heap + 0x70);
    DWORD f2 = *(DWORD*)(heap + 0x74);
    if ((f1 & ~2u) != 0) return true;
    if (f2 != 0 && (f2 & 0x40000060) == 0) return true;
    return false;
#else
    return false;
#endif
}

static bool checkRemoteDebugger() {
    BOOL p = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &p);
    return p != FALSE;
}

static bool checkNtQueryDebugPort() {
    DWORD_PTR port = 0;
    if (NtQueryInformationProcess(GetCurrentProcess(), kProcessDebugPort, &port, sizeof(port), nullptr) == 0) {
        if (port != 0) return true;
    }
    return false;
}

static bool checkNtQueryDebugObject() {
    HANDLE h = nullptr;
    if (NtQueryInformationProcess(GetCurrentProcess(), kProcessDebugObjectHandle, &h, sizeof(h), nullptr) == 0) {
        if (h != nullptr) return true;
    }
    return false;
}

static bool checkNtQueryDebugFlags() {
    ULONG flag = 0;
    if (NtQueryInformationProcess(GetCurrentProcess(), kProcessDebugFlags, &flag, sizeof(flag), nullptr) == 0) {
        if (flag == 0) return true;
    }
    return false;
}

static bool checkKernelDebugger() {
    struct { BOOLEAN DebuggerEnabled; BOOLEAN DebuggerNotPresent; } info{};
    NTSTATUS s = NtQuerySystemInformation(kSystemKernelDebuggerInformation, &info, sizeof(info), nullptr);
    if (s != 0) return false;
    return info.DebuggerEnabled && !info.DebuggerNotPresent;
}

static bool checkHardwareBreakpoints() {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &ctx)) return false;
    return ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3;
}

static bool checkCloseHandleException() {
    __try {
        NtClose((HANDLE)(ULONG_PTR)0xDEADBEEF);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}

static bool checkOutputDebugStringTrick() {
    SetLastError(0xC0DEBA5E);
    OutputDebugStringA("rh");
    return GetLastError() == 0;
}

static bool checkRdtscTiming() {
    auto t1 = __rdtsc();
    for (volatile int i = 0; i < 1000; ++i) {}
    auto t2 = __rdtsc();
    return (t2 - t1) > 0x4000000ull;
}

static bool checkSwitchDesktop() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t inputName[128] = {};
    DWORD needed = 0;
    GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed);
    CloseDesktop(input);

    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    wchar_t curName[128] = {};
    GetUserObjectInformationW(cur, UOI_NAME, curName, sizeof(curName), &needed);

    return _wcsicmp(inputName, curName) != 0;
}

bool isDebuggerPresentBasic() {
    return IsDebuggerPresent() || checkRemoteDebugger() || checkPebBeingDebugged();
}

void hideThreadFromDebugger() {
    NtSetInformationThread(GetCurrentThread(), kHideThreadFromDebugger, nullptr, 0);
}

Result check() {
    Result r;
    auto fail = [&](const char* m, const char* d = "") {
        r.detected = true;
        r.method = m;
        r.detail = d;
    };

    if (IsDebuggerPresent())          { fail("IsDebuggerPresent"); return r; }
    if (checkRemoteDebugger())        { fail("CheckRemoteDebuggerPresent"); return r; }
    if (checkPebBeingDebugged())      { fail("PEB.BeingDebugged"); return r; }
    if (checkPebNtGlobalFlag())       { fail("PEB.NtGlobalFlag"); return r; }
    if (checkPebHeapFlags())          { fail("PEB.HeapFlags"); return r; }
    if (checkNtQueryDebugPort())      { fail("NtQueryProcessDebugPort"); return r; }
    if (checkNtQueryDebugObject())    { fail("NtQueryProcessDebugObject"); return r; }
    if (checkNtQueryDebugFlags())     { fail("NtQueryProcessDebugFlags"); return r; }
    if (checkHardwareBreakpoints())   { fail("HW-BP"); return r; }
    if (checkOutputDebugStringTrick()){ fail("OutputDebugString"); return r; }
    if (checkCloseHandleException())  { fail("NtClose-bad-handle"); return r; }
    if (checkKernelDebugger())        { fail("KernelDebuggerEnabled"); return r; }
    if (checkRdtscTiming())           { fail("RDTSC-timing"); return r; }
    if (checkSwitchDesktop())         { fail("ForeignDesktop"); return r; }

    return r;
}

}
