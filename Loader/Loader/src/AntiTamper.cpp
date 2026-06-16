#include "AntiTamper.h"
#include "Util.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace antitamper {

struct Needle { const wchar_t* substr; const char* tag; };

static const Needle kProcBL[] = {
    { L"x64dbg",        "x64dbg" },
    { L"x32dbg",        "x32dbg" },
    { L"ollydbg",       "ollydbg" },
    { L"windbg",        "windbg" },
    { L"ida.exe",       "ida" },
    { L"ida64.exe",     "ida64" },
    { L"idaq.exe",      "idaq" },
    { L"idaq64.exe",    "idaq64" },
    { L"ghidra",        "ghidra" },
    { L"radare2",       "radare2" },
    { L"r2.exe",        "radare2" },
    { L"binaryninja",   "binja" },
    { L"dnspy",         "dnspy" },
    { L"dnspy.exe",     "dnspy" },
    { L"dnspyex",       "dnspyex" },
    { L"cheatengine",   "cheatengine" },
    { L"cheat engine",  "cheatengine" },
    { L"scylla",        "scylla" },
    { L"immunitydbg",   "immunity" },
    { L"reclassex",     "reclass" },
    { L"reclass.net",   "reclass" },
    { L"http debugger", "http-debug" },
    { L"fiddler",       "fiddler" },
    { L"wireshark",     "wireshark" },
    { L"tshark",        "tshark" },
    { L"httpdebugger",  "httpdebugger" },
    { L"charles",       "charles" },
    { L"mitmproxy",     "mitmproxy" },
    { L"processhacker", "processhacker" },
    { L"procmon.exe",   "procmon" },
    { L"procmon64.exe", "procmon" },
    { L"procexp.exe",   "procexp" },
    { L"procexp64.exe", "procexp" },
    { L"apimonitor",    "apimonitor" },
    { L"de4dot",        "de4dot" },
    { L"hxd.exe",       "hxd" },
    { L"010editor",     "010editor" },
    { L"pe-bear",       "pe-bear" },
    { L"protection_id", "protection-id" },
    { L"die.exe",       "die" },
    { L"detect it easy","die" },
    { L"x64_dbg",       "x64dbg" },
};

static const Needle kModBL[] = {
    { L"scyllahide",    "scyllahide" },
    { L"hyperhide",     "hyperhide" },
    { L"titanhide",     "titanhide" },
    { L"vehdebugger",   "vehdebugger" },
    { L"strongod",      "strongod" },
    { L"safengine",     "safengine" },
    { L"phantom",       "phantom-driver" },
    { L"dbghelp.dll",   nullptr },
};

static const wchar_t* const kWindowBL[] = {
    L"OLLYDBG",
    L"WinDbgFrameClass",
    L"x64dbg",
    L"GBDYLLO",
    L"pediy06",
    L"FilemonClass",
    L"PROCMON_WINDOW_CLASS",
    L"REGmonClass",
    L"TIdaWindow",
    L"IDATopLevelWindow",
    L"IDAMainWindow",
    L"Cheat Engine",
    L"Wireshark",
};

static std::wstring lower(const std::wstring& s) {
    std::wstring r(s.size(), 0);
    for (size_t i = 0; i < s.size(); ++i) r[i] = (wchar_t)towlower(s[i]);
    return r;
}

Result scanProcesses() {
    Result r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            std::wstring name = lower(pe.szExeFile);
            for (auto& bl : kProcBL) {
                if (name.find(bl.substr) != std::wstring::npos) {
                    r.detected = true;
                    r.method = "process";
                    r.detail = bl.tag;
                    CloseHandle(snap);
                    return r;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return r;
}

Result scanModules() {
    Result r;
    HMODULE mods[1024];
    DWORD cb = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb)) return r;
    DWORD count = cb / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH] = { 0 };
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        std::wstring n = lower(name);
        for (auto& bl : kModBL) {
            if (!bl.substr) continue;
            if (n.find(bl.substr) != std::wstring::npos) {
                if (bl.tag) {
                    r.detected = true;
                    r.method = "module";
                    r.detail = bl.tag;
                    return r;
                }
            }
        }
    }
    return r;
}

static BOOL CALLBACK windowProc(HWND hwnd, LPARAM lp) {
    auto* r = reinterpret_cast<Result*>(lp);
    wchar_t cls[256] = { 0 }; wchar_t title[256] = { 0 };
    GetClassNameW(hwnd, cls, _countof(cls));
    GetWindowTextW(hwnd, title, _countof(title));
    std::wstring lcls = lower(cls), ltitle = lower(title);
    for (auto* bad : kWindowBL) {
        std::wstring lb = lower(bad);
        if (lcls.find(lb) != std::wstring::npos || ltitle.find(lb) != std::wstring::npos) {
            r->detected = true;
            r->method = "window";
            r->detail = util::ws2s(cls);
            return FALSE;
        }
    }
    return TRUE;
}

Result scanWindows() {
    Result r;
    EnumWindows(windowProc, (LPARAM)&r);
    return r;
}

Result selfIntegrityCheck() {
    Result r;
    HMODULE me = GetModuleHandleW(nullptr);
    if (!me) return r;
    auto base = (uint8_t*)me;
    auto dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return r;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return r;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + sec->VirtualAddress, &mbi, sizeof(mbi))) continue;
        if (mbi.Protect & (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READWRITE | PAGE_WRITECOPY)) {
            r.detected = true;
            r.method = "self.code-writable";
            char b[64];
            _snprintf_s(b, _TRUNCATE, "section=%.8s prot=%lx", sec->Name, mbi.Protect);
            r.detail = b;
            return r;
        }
    }
    return r;
}

Result fullScan() {
    Result r = scanProcesses();
    if (r.detected) return r;
    r = scanModules();
    if (r.detected) return r;
    r = scanWindows();
    if (r.detected) return r;
    r = selfIntegrityCheck();
    return r;
}

}
