#include "AntiVM.h"
#include "Util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <vector>
#include <string>

#pragma comment(lib, "iphlpapi.lib")

namespace antivm {

static bool cpuidHypervisorBit() {
    int regs[4]{};
    __cpuid(regs, 1);
    return (regs[2] & (1 << 31)) != 0;
}

static std::string cpuidHypervisorBrand() {
    int regs[4]{};
    __cpuid(regs, 0x40000000);
    if (regs[0] < 0x40000000) return {};
    char buf[16] = { 0 };
    *((int*)(buf + 0))  = regs[1];
    *((int*)(buf + 4))  = regs[2];
    *((int*)(buf + 8))  = regs[3];
    return std::string(buf, 12);
}

static bool moduleLoaded(const wchar_t* name) {
    return GetModuleHandleW(name) != nullptr;
}

static bool fileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool registryHasKey(HKEY root, const wchar_t* sub) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return true;
    }
    return false;
}

static bool descriptionLooksVirtual(const wchar_t* desc) {
    if (!desc) return false;
    std::wstring d = desc;
    for (auto& c : d) c = (wchar_t)towlower(c);
    static const wchar_t* needles[] = {
        L"hyper-v",
        L"virtual ethernet",
        L"vethernet",
        L"vmware",
        L"vmnet",
        L"virtualbox",
        L"vbox",
        L"wsl",
        L"loopback",
        L"tap-windows",
        L"tap adapter",
        L"tunnel adapter",
        L"bluetooth",
        L"miniport",
        L"virtual adapter",
        L"docker",
        L"npcap",
        L"pseudo-interface",
    };
    for (auto* n : needles) if (d.find(n) != std::wstring::npos) return true;
    return false;
}

static bool macLooksVirtual() {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr, nullptr, &bufLen);
    if (!bufLen) return false;
    std::vector<BYTE> buf(bufLen);
    auto* p = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr, p, &bufLen) != NO_ERROR) return false;

    for (auto* a = p; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->IfType == IF_TYPE_TUNNEL) continue;
        if (a->PhysicalAddressLength != 6) continue;
        if (a->OperStatus != IfOperStatusUp) continue;
        if (descriptionLooksVirtual(a->Description)) continue;
        if (descriptionLooksVirtual(a->FriendlyName)) continue;
        const BYTE* m = a->PhysicalAddress;
        if (m[0] == 0x00 && m[1] == 0x05 && m[2] == 0x69) return true;
        if (m[0] == 0x00 && m[1] == 0x0C && m[2] == 0x29) return true;
        if (m[0] == 0x00 && m[1] == 0x1C && m[2] == 0x14) return true;
        if (m[0] == 0x00 && m[1] == 0x50 && m[2] == 0x56) return true;
        if (m[0] == 0x08 && m[1] == 0x00 && m[2] == 0x27) return true;
        if (m[0] == 0x00 && m[1] == 0x16 && m[2] == 0x3E) return true;
        if (m[0] == 0x00 && m[1] == 0x1C && m[2] == 0x42) return true;
        if (m[0] == 0x00 && m[1] == 0x15 && m[2] == 0x5D) return true;
    }
    return false;
}

Result check() {
    Result r;
    auto fail = [&](const char* m, const std::string& d = "") {
        r.detected = true; r.method = m; r.detail = d;
    };

    if (cpuidHypervisorBit()) {
        auto brand = cpuidHypervisorBrand();
        std::string lo = util::toLower(brand);
        if (lo.find("microsoft") == std::string::npos) {
            fail("cpuid.hypervisor", brand);
            return r;
        }
    }

    if (macLooksVirtual())           { fail("mac.oui");        return r; }

    if (moduleLoaded(L"vboxservice.dll") || moduleLoaded(L"vboxhook.dll")) {
        fail("module", "vbox-guest-loaded"); return r;
    }
    struct { const wchar_t* path; const char* tag; } guestOnlyDrivers[] = {
        { L"C:\\windows\\System32\\drivers\\VBoxMouse.sys", "VBoxMouse" },
        { L"C:\\windows\\System32\\drivers\\VBoxGuest.sys", "VBoxGuest" },
        { L"C:\\windows\\System32\\drivers\\VBoxSF.sys",    "VBoxSF" },
        { L"C:\\windows\\System32\\drivers\\VBoxVideo.sys", "VBoxVideo" },
    };
    for (const auto& d : guestOnlyDrivers) {
        if (fileExists(d.path)) { fail("driver-file", d.tag); return r; }
    }
    if (registryHasKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oracle\\VirtualBox Guest Additions")) {
        fail("registry", "vbox-guest-additions"); return r;
    }

    return r;
}

}
