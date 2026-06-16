#include "Ban.h"
#include "Util.h"
#include "Crypto.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <strsafe.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ban {

static const uint8_t kSentinel[16] = {
    0x52, 0x48, 0x42, 0x4E, 0x2D, 0x76, 0x31, 0x00,
    0xA9, 0x4C, 0xD3, 0x71, 0x6F, 0x28, 0xE5, 0x1B
};

struct Slot {
    int          dirKind;
    const wchar_t* sub;
};

static const Slot kSlots[20] = {
    { 0, L"" },
    { 0, L"" },
    { 1, L"" },
    { 1, L"Microsoft\\Windows\\Themes" },
    { 1, L"Microsoft\\Windows\\Recent" },
    { 2, L"" },
    { 2, L"Microsoft" },
    { 2, L"Microsoft\\Windows\\PowerShell" },
    { 3, L"" },
    { 3, L"Microsoft\\Crypto" },
    { 3, L"Microsoft\\Diagnosis" },
    { 3, L"Microsoft\\Windows\\Caches" },
    { 1, L"Microsoft\\CLR_v4.0_32" },
    { 1, L"Microsoft\\Internet Explorer" },
    { 0, L"" },
    { 2, L"Microsoft\\CryptnetUrlCache" },
    { 3, L"Microsoft\\Network" },
    { 1, L"Microsoft\\Vault" },
    { 0, L"" },
    { 2, L"Microsoft\\SystemCertificates" }
};

static const wchar_t* kSalts[20] = {
    L"r1n.s7", L"x9k.q3", L"a4f.tq", L"v8m.zd", L"j2y.le",
    L"hb6.uo", L"d5n.kc", L"p3e.fr", L"w8a.ix", L"q7t.bs",
    L"n1d.gh", L"l4y.cm", L"o6r.zp", L"s9k.ev", L"c2v.tn",
    L"m5b.qw", L"e7p.dx", L"f3l.aj", L"u9x.bk", L"i6c.rv"
};

static bool getMachineGuid(std::string& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[128] = { 0 };
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LONG s = RegQueryValueExW(hk, L"MachineGuid", nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(hk);
    if (s != ERROR_SUCCESS || type != REG_SZ) return false;
    out = util::ws2s(buf);
    return !out.empty();
}

static bool getDirRoot(int kind, std::wstring& out) {
    PWSTR p = nullptr;
    REFKNOWNFOLDERID id =
        (kind == 0) ? FOLDERID_ProgramData :
        (kind == 1) ? FOLDERID_RoamingAppData :
        (kind == 2) ? FOLDERID_LocalAppData :
                      FOLDERID_ProgramData;

    if (kind == 0 || kind == 3) {
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)) || !p) return false;
    } else {
        if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) return false;
    }
    out = p;
    CoTaskMemFree(p);
    return !out.empty();
}

static void hashSlot(const std::string& machineGuid, int idx, uint8_t out[32]) {
    std::string salt = util::ws2s(kSalts[idx]);
    std::string buf;
    buf.reserve(machineGuid.size() + salt.size() + 16 + 8);
    buf.append(machineGuid);
    buf.push_back('\0');
    buf.append(salt);
    buf.push_back('\0');
    char nb[16];
    _snprintf_s(nb, _TRUNCATE, "%d", idx);
    buf.append(nb);
    buf.append((const char*)kSentinel, sizeof(kSentinel));
    crypto::sha256((const uint8_t*)buf.data(), buf.size(), out);
}

static std::wstring slotFileName(const std::string& machineGuid, int idx) {
    uint8_t h[32];
    hashSlot(machineGuid, idx, h);
    char hex[33] = { 0 };
    for (int i = 0; i < 16; ++i) {
        static const char* d = "0123456789abcdef";
        hex[i*2 + 0] = d[h[i] >> 4];
        hex[i*2 + 1] = d[h[i] & 0xF];
    }
    hex[32] = 0;
    std::string fn = std::string(hex) + ".dat";
    return util::s2ws(fn);
}

static std::wstring slotFullPath(const std::string& machineGuid, int idx, bool createDirs) {
    std::wstring root;
    if (!getDirRoot(kSlots[idx].dirKind, root)) return L"";
    std::wstring path = root;
    if (kSlots[idx].sub && kSlots[idx].sub[0]) {
        path += L"\\";
        path += kSlots[idx].sub;
    }
    if (createDirs) {
        SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    }
    path += L"\\";
    path += slotFileName(machineGuid, idx);
    return path;
}

static bool writeSlotFile(const std::wstring& path, const std::string& machineGuid, int idx) {
    uint8_t h[32];
    hashSlot(machineGuid, idx, h);

    HANDLE hf = CreateFileW(path.c_str(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                            nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(hf, h, 32, &wrote, nullptr);
    CloseHandle(hf);
    return ok && wrote == 32;
}

static bool verifySlotFile(const std::wstring& path, const std::string& machineGuid, int idx) {
    HANDLE hf = CreateFileW(path.c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    uint8_t buf[32] = { 0 };
    DWORD got = 0;
    BOOL ok = ReadFile(hf, buf, 32, &got, nullptr);
    CloseHandle(hf);
    if (!ok || got != 32) return false;

    uint8_t expect[32];
    hashSlot(machineGuid, idx, expect);
    return memcmp(buf, expect, 32) == 0;
}

bool isBanned() {
    std::string mg;
    if (!getMachineGuid(mg)) return false;

    int hits = 0;
    for (int i = 0; i < 20; ++i) {
        std::wstring p = slotFullPath(mg, i, false);
        if (p.empty()) continue;
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (verifySlotFile(p, mg, i)) {
            ++hits;
            if (hits >= 2) return true;
        }
    }
    return false;
}

static void scheduleSelfDestruct() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t cmdExe[MAX_PATH] = { 0 };
    GetSystemDirectoryW(cmdExe, MAX_PATH);
    StringCchCatW(cmdExe, MAX_PATH, L"\\cmd.exe");

    std::wstring args = L"/c ping 127.0.0.1 -n 2 > nul & del /f /q \"";
    args += exePath;
    args += L"\"";

    std::wstring cmdLine = L"\"";
    cmdLine += cmdExe;
    cmdLine += L"\" ";
    cmdLine += args;

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(0);

    if (CreateProcessW(nullptr, mutableCmd.data(),
                       nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

[[noreturn]] void triggerBan(const std::string& reason) {
    util::logf("[ban] triggered: %s", reason.c_str());

    std::string mg;
    if (!getMachineGuid(mg)) {
        mg = "unknown-machine";
    }

    int planted = 0;
    for (int i = 0; i < 20; ++i) {
        std::wstring p = slotFullPath(mg, i, true);
        if (p.empty()) continue;
        if (writeSlotFile(p, mg, i)) ++planted;
    }
    util::logf("[ban] planted %d/20 markers", planted);

    scheduleSelfDestruct();

    util::sleepMs(120);
    util::hardExit(0xB00B);
}

}
