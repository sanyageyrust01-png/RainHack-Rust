#include "Vdb.h"
#include "Util.h"
#include "VMProtectSDK.h"

#include <Windows.h>
#include <winternl.h>

#pragma comment(lib, "ntdll.lib")

namespace vdb {

extern "C" NTSTATUS NTAPI NtQuerySystemInformation(
    SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

#define SYSTEM_CODE_INTEGRITY_INFORMATION_CLASS ((SYSTEM_INFORMATION_CLASS)103)

struct SYSTEM_CODEINTEGRITY_INFORMATION {
    ULONG Length;
    ULONG CodeIntegrityOptions;
};

#ifndef CODEINTEGRITY_OPTION_ENABLED
#define CODEINTEGRITY_OPTION_ENABLED                  0x0001
#endif
#ifndef CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED
#define CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED        0x0400
#endif

static bool readDword(HKEY root, const wchar_t* sub, const wchar_t* val, DWORD& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = sizeof(out);
    LONG r = RegQueryValueExW(h, val, nullptr, &type, (LPBYTE)&out, &sz);
    RegCloseKey(h);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

static bool writeDword(HKEY root, const wchar_t* sub, const wchar_t* val, DWORD data) {
    HKEY h = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &h, &disp) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, val, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

__declspec(noinline) Status check() {
    VMP_BEGIN_MUTATION("vdb.check");
    Status s;

    DWORD val = 0;
    if (readDword(HKEY_LOCAL_MACHINE,
                  L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
                  L"VulnerableDriverBlocklistEnable", val)) {
        s.blocklistEnabled = (val != 0);
    } else {
        s.blocklistEnabled = true;
    }

    SYSTEM_CODEINTEGRITY_INFORMATION ci{};
    ci.Length = sizeof(ci);
    NTSTATUS st = NtQuerySystemInformation(
        SYSTEM_CODE_INTEGRITY_INFORMATION_CLASS, &ci, sizeof(ci), nullptr);
    if (st == 0) {
        s.dseEnabled  = (ci.CodeIntegrityOptions & CODEINTEGRITY_OPTION_ENABLED) != 0;
        s.hvciEnabled = (ci.CodeIntegrityOptions & CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED) != 0;
    }

    char buf[160];
    _snprintf_s(buf, _TRUNCATE,
        "vdb=%d hvci=%d dse=%d ci=0x%lx",
        s.blocklistEnabled ? 1 : 0,
        s.hvciEnabled ? 1 : 0,
        s.dseEnabled ? 1 : 0,
        (unsigned long)ci.CodeIntegrityOptions);
    s.detail = buf;
    return s;
}

__declspec(noinline) bool disable(std::string& err) {
    VMP_BEGIN_MUTATION("vdb.disable");
    bool ok = true;
    if (!writeDword(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
                    L"VulnerableDriverBlocklistEnable", 0)) {
        err = "registry write VDB failed (run as admin?)";
        ok = false;
    }
    if (!writeDword(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                    L"Enabled", 0)) {
        if (ok) err = "registry write HVCI failed";
        ok = false;
    }
    return ok;
}

bool scheduleReboot(unsigned seconds, std::string& err) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        err = "OpenProcessToken failed";
        return false;
    }
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        CloseHandle(token);
        err = "LookupPrivilegeValue failed";
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);

    if (!InitiateSystemShutdownExW(nullptr,
            (LPWSTR)L"RainHack: applying VDB / HVCI changes",
            seconds, TRUE, TRUE,
            SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED)) {
        DWORD le = GetLastError();
        char buf[64];
        _snprintf_s(buf, _TRUNCATE, "InitiateSystemShutdown failed: %lu", le);
        err = buf;
        return false;
    }
    return true;
}

}
