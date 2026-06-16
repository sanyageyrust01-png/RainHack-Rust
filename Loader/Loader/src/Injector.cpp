#include "Injector.h"
#include "Util.h"
#include "VMProtectSDK.h"
#include "Resources.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winioctl.h>
#include <strsafe.h>
#include <vector>
#include <string>

namespace injector {

#define RH_DEVICE_PATH      L"\\\\.\\FREEWAREDEVICE"
#define IOCTL_WRITE_MEMORY           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLOC_MEMORY           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUEUE_APC              CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_UNLOAD_DRIVER          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROTECT_MEMORY         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QUERY_MODULE           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_PAYLOAD_BEGIN          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_CHUNK          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_DECRYPT        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_GET_META       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_GET_IMPORTS    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_SET_IAT        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x815, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_INJECT         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x816, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAYLOAD_RELEASE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x817, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RH_PAYLOAD_CHUNK_MAX         (32 * 1024)
#define RH_PAYLOAD_KEY_SIZE          32

typedef struct _RH_MEMORY_REQUEST {
    HANDLE    ProcessId;
    ULONGLONG TargetAddress;
    ULONGLONG BufferAddress;
    SIZE_T    Size;
    SIZE_T    ReturnSize;
    LONG      Status;
    ULONG     Reserved;
} RH_MEMORY_REQUEST;

typedef struct _RH_ALLOC_REQUEST {
    HANDLE    ProcessId;
    ULONGLONG Address;
    SIZE_T    Size;
    ULONG     AllocationType;
    ULONG     Protect;
    LONG      Status;
    ULONG     Reserved;
} RH_ALLOC_REQUEST;

typedef struct _RH_PROTECT_REQUEST {
    HANDLE    ProcessId;
    ULONGLONG Address;
    SIZE_T    Size;
    ULONG     NewProtect;
    ULONG     OldProtect;
    LONG      Status;
    ULONG     Reserved;
} RH_PROTECT_REQUEST;

typedef struct _RH_MODULE_QUERY_REQUEST {
    HANDLE    ProcessId;
    WCHAR     ModuleName[64];
    ULONGLONG BaseAddress;
    SIZE_T    Size;
    LONG      Status;
    ULONG     Reserved;
} RH_MODULE_QUERY_REQUEST;

typedef struct _RH_APC_REQUEST {
    HANDLE    ThreadId;
    ULONGLONG NormalRoutine;
    ULONGLONG NormalContext;
    ULONGLONG SystemArgument1;
    ULONGLONG SystemArgument2;
} RH_APC_REQUEST;

typedef struct _RH_PAYLOAD_BEGIN {
    SIZE_T    TotalEncryptedSize;
    UCHAR     SessionKey[RH_PAYLOAD_KEY_SIZE];
    ULONGLONG PayloadId;
} RH_PAYLOAD_BEGIN;

typedef struct _RH_PAYLOAD_CHUNK_HDR {
    ULONGLONG PayloadId;
    ULONG     Offset;
    ULONG     ChunkSize;
} RH_PAYLOAD_CHUNK_HDR;

typedef struct _RH_PAYLOAD_ID {
    ULONGLONG PayloadId;
} RH_PAYLOAD_ID;

typedef struct _RH_PAYLOAD_META {
    ULONGLONG PayloadId;
    ULONG     SizeOfImage;
    ULONG     EntryPointRva;
    ULONGLONG PreferredBase;
    ULONG     ImportCount;
    ULONG     ImportBlobSize;
    ULONG     RelocationCount;
} RH_PAYLOAD_META;

typedef struct _RH_PAYLOAD_IMP {
    ULONGLONG PayloadId;
    ULONG     BufferSize;
    ULONG     BytesWritten;
} RH_PAYLOAD_IMP;

#pragma pack(push, 1)
typedef struct _RH_IMP_ENTRY {
    USHORT  DllNameLen;
    USHORT  FuncNameLen;
    USHORT  Ordinal;
    USHORT  Flags;
    ULONG   IatRva;
} RH_IMP_ENTRY;

typedef struct _RH_IAT_ENTRY {
    ULONG     IatRva;
    ULONG     Pad;
    ULONGLONG ResolvedAddress;
} RH_IAT_ENTRY;
#pragma pack(pop)

typedef struct _RH_PAYLOAD_SET_IAT {
    ULONGLONG PayloadId;
    ULONG     EntryCount;
} RH_PAYLOAD_SET_IAT;

typedef struct _RH_PAYLOAD_INJECT {
    ULONGLONG PayloadId;
    HANDLE    TargetPid;
    ULONGLONG TargetBase;
} RH_PAYLOAD_INJECT;

bool isProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD exitCode = 0;
    BOOL ok = GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);
    return ok && exitCode == 259;
}

bool ensureTargetAlive(const wchar_t* exeName, DWORD knownPid, DWORD& outPid,
                       unsigned rescanTimeoutMs) {
    if (knownPid && isProcessAlive(knownPid)) {
        DWORD curr = findProcessId(exeName);
        if (curr && curr == knownPid) {
            outPid = knownPid;
            return true;
        }
        if (curr) {
            util::logf("ensureTargetAlive: PID changed %lu -> %lu (process restarted)",
                       knownPid, curr);
            outPid = curr;
            return true;
        }
    }

    uint64_t deadline = util::nowMs() + rescanTimeoutMs;
    while (util::nowMs() < deadline) {
        DWORD curr = findProcessId(exeName);
        if (curr && isProcessAlive(curr)) {
            if (curr != knownPid) {
                util::logf("ensureTargetAlive: re-resolved %ls -> PID %lu (was %lu)",
                           exeName, curr, knownPid);
            }
            outPid = curr;
            return true;
        }
        util::sleepMs(RAINHACK_PROC_ALIVE_RECHECK_MS);
    }

    outPid = 0;
    return false;
}

DWORD findProcessId(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                if (isProcessAlive(pe.th32ProcessID)) {
                    pid = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

DWORD waitForProcess(const wchar_t* primary, const wchar_t* fallback, unsigned timeoutSeconds) {
    uint64_t deadline = util::nowMs() + (uint64_t)timeoutSeconds * 1000ull;
    while (util::nowMs() < deadline) {
        DWORD pid = findProcessId(primary);
        if (pid) return pid;
        if (fallback) {
            pid = findProcessId(fallback);
            if (pid) return pid;
        }
        util::sleepMs(500);
    }
    return 0;
}

static int countProcessThreads(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int count = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

static bool driverQueryModule(HANDLE drv, DWORD pid, const wchar_t* name,
                              ULONGLONG& baseOut, SIZE_T& sizeOut) {
    RH_MODULE_QUERY_REQUEST req{};
    req.ProcessId = (HANDLE)(ULONG_PTR)pid;
    StringCchCopyW(req.ModuleName, _countof(req.ModuleName), name);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(drv, IOCTL_QUERY_MODULE,
                              &req, sizeof(req), &req, sizeof(req),
                              &ret, nullptr);
    if (!ok || req.Status < 0) return false;
    baseOut = req.BaseAddress;
    sizeOut = req.Size;
    return true;
}

bool validateProcessModules(DWORD pid, std::string& reason) {
    VMP_BEGIN_MUTATION("inject.validateModules");

    HANDLE drv = CreateFileW(RH_DEVICE_PATH,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (drv == INVALID_HANDLE_VALUE) {
        reason = "drv-open-failed";
        return false;
    }

    ULONGLONG gaBase = 0;    SIZE_T gaSize = 0;
    ULONGLONG unityBase = 0; SIZE_T unitySize = 0;

    bool foundGA    = driverQueryModule(drv, pid, L"GameAssembly.dll", gaBase, gaSize);
    bool foundUnity = driverQueryModule(drv, pid, L"UnityPlayer.dll", unityBase, unitySize);

    CloseHandle(drv);

    if (!foundGA) { reason = "ga-missing"; return false; }
    if (gaSize < 128u * 1024 * 1024) { reason = "ga-size"; return false; }
    if (!foundUnity) { reason = "unity-missing"; return false; }

    int threads = countProcessThreads(pid);
    if (threads < 15) { reason = "thread-count"; return false; }

    return true;
}

static HANDLE openDevice() {
    return CreateFileW(RH_DEVICE_PATH,
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

bool driverLoaded() {
    HANDLE h = openDevice();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static ULONGLONG kAllocMem(HANDLE drv, DWORD pid, SIZE_T size, ULONG protect, LONG* outStatus = nullptr) {
    RH_ALLOC_REQUEST req{};
    req.ProcessId      = (HANDLE)(ULONG_PTR)pid;
    req.Size           = size;
    req.AllocationType = MEM_COMMIT | MEM_RESERVE;
    req.Protect        = protect;
    DWORD ret = 0;
    if (!DeviceIoControl(drv, IOCTL_ALLOC_MEMORY, &req, sizeof(req), &req, sizeof(req), &ret, nullptr)) {
        if (outStatus) *outStatus = (LONG)0xC0000001;
        return 0;
    }
    if (outStatus) *outStatus = req.Status;
    if (req.Status < 0) return 0;
    return req.Address;
}

static LONG kProtectMem(HANDLE drv, DWORD pid, ULONGLONG addr, SIZE_T size, ULONG newProtect, ULONG* outOld = nullptr) {
    RH_PROTECT_REQUEST req{};
    req.ProcessId  = (HANDLE)(ULONG_PTR)pid;
    req.Address    = addr;
    req.Size       = size;
    req.NewProtect = newProtect;
    DWORD ret = 0;
    if (!DeviceIoControl(drv, IOCTL_PROTECT_MEMORY, &req, sizeof(req), &req, sizeof(req), &ret, nullptr)) {
        return (LONG)0xC0000001;
    }
    if (outOld) *outOld = req.OldProtect;
    return req.Status;
}

static ULONGLONG kAllocCodeRegion(HANDLE drv, DWORD pid, SIZE_T size, std::string& errOut) {
    LONG s = 0;
    ULONGLONG addr = kAllocMem(drv, pid, size, PAGE_EXECUTE_READWRITE, &s);
    if (addr) return addr;

    if ((ULONG)s == 0xC0000604) {
        LONG s2 = 0;
        addr = kAllocMem(drv, pid, size, PAGE_READWRITE, &s2);
        if (!addr) {
            char b[96]; StringCchPrintfA(b, _countof(b), " (alloc-rwx=0x%08X, alloc-rw=0x%08X)", (unsigned)s, (unsigned)s2);
            errOut = b;
            return 0;
        }
        ULONG oldProt = 0;
        LONG ps = kProtectMem(drv, pid, addr, size, PAGE_EXECUTE_READWRITE, &oldProt);
        if (ps < 0) {
            char b[96]; StringCchPrintfA(b, _countof(b), " (alloc-rwx=0x%08X, protect-rwx=0x%08X)", (unsigned)s, (unsigned)ps);
            errOut = b;
            return 0;
        }
        return addr;
    }

    if ((ULONG)s == 0xC000010A) {
        errOut = " (process is terminating - close Rust completely and try again)";
    } else if ((ULONG)s == 0xC0000022) {
        errOut = " (access-denied - EAC may have stripped handle)";
    } else if ((ULONG)s == 0xC0000005) {
        errOut = " (access-violation in driver alloc)";
    } else {
        char b[64]; StringCchPrintfA(b, _countof(b), " (status=0x%08X)", (unsigned)s);
        errOut = b;
    }
    return 0;
}

static bool kWriteMem(HANDLE drv, DWORD pid, ULONGLONG dst, const void* src, SIZE_T size, LONG* outStatus = nullptr) {
    RH_MEMORY_REQUEST req{};
    req.ProcessId     = (HANDLE)(ULONG_PTR)pid;
    req.TargetAddress = dst;
    req.BufferAddress = (ULONGLONG)src;
    req.Size          = size;
    DWORD ret = 0;
    if (!DeviceIoControl(drv, IOCTL_WRITE_MEMORY, &req, sizeof(req), &req, sizeof(req), &ret, nullptr)) {
        if (outStatus) *outStatus = (LONG)0xC0000001;
        return false;
    }
    if (outStatus) *outStatus = req.Status;
    return req.Status >= 0;
}

static bool kQueueApc(HANDLE drv, DWORD tid, ULONGLONG routine, ULONGLONG context) {
    RH_APC_REQUEST req{};
    req.ThreadId      = (HANDLE)(ULONG_PTR)tid;
    req.NormalRoutine = routine;
    req.NormalContext = context;
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_QUEUE_APC, &req, sizeof(req), &req, sizeof(req), &ret, nullptr) != FALSE;
}

static const BYTE kStubCode[] = {
    0x31, 0xC0,
    0xBA, 0x01, 0x00, 0x00, 0x00,
    0xF0, 0x0F, 0xB1, 0x11,
    0x75, 0x1F,
    0x53,
    0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0x41, 0x08,
    0x48, 0x8B, 0x59, 0x10,
    0x48, 0x89, 0xD9,
    0xBA, 0x01, 0x00, 0x00, 0x00,
    0x45, 0x31, 0xC0,
    0xFF, 0xD0,
    0x48, 0x83, 0xC4, 0x20,
    0x5B,
    0xC3
};
static const SIZE_T kStubDataSize  = 0x18;
static const SIZE_T kStubTotalSize = kStubDataSize + sizeof(kStubCode);

__declspec(noinline) bool manualMapDll(DWORD targetPid,
                  const std::vector<uint8_t>& dllImage,
                  std::string& err) {
    VMP_BEGIN_ULTRA("inject.kernelApc");
    if (dllImage.size() < sizeof(IMAGE_DOS_HEADER)) { err = "image-too-small"; return false; }

    HANDLE drv = openDevice();
    if (drv == INVALID_HANDLE_VALUE) {
        err = "driver not mapped (\\\\.\\FREEWAREDEVICE unreachable)";
        return false;
    }

    auto dos = (const IMAGE_DOS_HEADER*)dllImage.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { CloseHandle(drv); err = "not-pe"; return false; }
    auto nt = (const IMAGE_NT_HEADERS*)(dllImage.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { CloseHandle(drv); err = "not-pe-nt"; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { CloseHandle(drv); err = "wrong-arch (need x64)"; return false; }

    std::string allocErr;
    ULONGLONG remoteBase = kAllocCodeRegion(drv, targetPid, nt->OptionalHeader.SizeOfImage, allocErr);
    if (!remoteBase) { CloseHandle(drv); err = std::string("kernel alloc image failed") + allocErr; return false; }

    std::vector<BYTE> imageBuf(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(imageBuf.data(), dllImage.data(), nt->OptionalHeader.SizeOfHeaders);

    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData) {
            memcpy(imageBuf.data() + sec[i].VirtualAddress,
                   dllImage.data()  + sec[i].PointerToRawData,
                   sec[i].SizeOfRawData);
        }
    }

    DWORD_PTR delta = (DWORD_PTR)remoteBase - (DWORD_PTR)nt->OptionalHeader.ImageBase;
    if (delta != 0 && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        auto reloc = (PIMAGE_BASE_RELOCATION)(imageBuf.data() +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
        DWORD parsed = 0;
        DWORD total  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        while (parsed < total && reloc->SizeOfBlock) {
            DWORD count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            auto list = (WORD*)((BYTE*)reloc + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD i = 0; i < count; ++i) {
                if ((list[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                    auto p = (DWORD_PTR*)(imageBuf.data() + reloc->VirtualAddress + (list[i] & 0xFFF));
                    *p += delta;
                }
            }
            parsed += reloc->SizeOfBlock;
            reloc = (PIMAGE_BASE_RELOCATION)((BYTE*)reloc + reloc->SizeOfBlock);
        }
    }

    auto mappedNt = (PIMAGE_NT_HEADERS)(imageBuf.data() + ((PIMAGE_DOS_HEADER)imageBuf.data())->e_lfanew);
    if (mappedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(imageBuf.data() +
            mappedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        while (imp->Name) {
            const char* dllName = (const char*)(imageBuf.data() + imp->Name);
            HMODULE mod = LoadLibraryA(dllName);
            if (!mod) { CloseHandle(drv); err = std::string("LoadLibrary failed: ") + dllName; return false; }

            DWORD thunkRVA = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
            auto thunkRef = (PIMAGE_THUNK_DATA)(imageBuf.data() + thunkRVA);
            auto funcRef  = (PIMAGE_THUNK_DATA)(imageBuf.data() + imp->FirstThunk);

            while (thunkRef->u1.AddressOfData) {
                FARPROC addr;
                if (IMAGE_SNAP_BY_ORDINAL(thunkRef->u1.Ordinal)) {
                    addr = GetProcAddress(mod, (LPCSTR)(thunkRef->u1.Ordinal & 0xFFFF));
                } else {
                    auto byName = (PIMAGE_IMPORT_BY_NAME)(imageBuf.data() + thunkRef->u1.AddressOfData);
                    addr = GetProcAddress(mod, byName->Name);
                }
                if (!addr) { CloseHandle(drv); err = std::string("GetProcAddress failed in ") + dllName; return false; }
                funcRef->u1.Function = (ULONGLONG)addr;
                ++thunkRef; ++funcRef;
            }
            ++imp;
        }
    }

    LONG ws = 0;
    if (!kWriteMem(drv, targetPid, remoteBase, imageBuf.data(), imageBuf.size(), &ws)) {
        char b[64]; StringCchPrintfA(b, _countof(b), " (status=0x%08X)", (unsigned)ws);
        CloseHandle(drv); err = std::string("kernel write image failed") + b; return false;
    }

    std::vector<BYTE> stubBuf(kStubTotalSize, 0);
    LONG flag = 0;
    memcpy(stubBuf.data() + 0x00, &flag, sizeof(flag));
    ULONGLONG dllMainAddr = remoteBase + mappedNt->OptionalHeader.AddressOfEntryPoint;
    memcpy(stubBuf.data() + 0x08, &dllMainAddr, sizeof(dllMainAddr));
    ULONGLONG imageBaseFinal = remoteBase;
    memcpy(stubBuf.data() + 0x10, &imageBaseFinal, sizeof(imageBaseFinal));
    memcpy(stubBuf.data() + kStubDataSize, kStubCode, sizeof(kStubCode));

    std::string stubErr;
    ULONGLONG remoteStub = kAllocCodeRegion(drv, targetPid, kStubTotalSize, stubErr);
    if (!remoteStub) { CloseHandle(drv); err = std::string("kernel alloc stub failed") + stubErr; return false; }

    LONG ws2 = 0;
    if (!kWriteMem(drv, targetPid, remoteStub, stubBuf.data(), stubBuf.size(), &ws2)) {
        char b[64]; StringCchPrintfA(b, _countof(b), " (status=0x%08X)", (unsigned)ws2);
        CloseHandle(drv); err = std::string("kernel write stub failed") + b; return false;
    }

    ULONGLONG stubCodeAddr = remoteStub + kStubDataSize;
    ULONGLONG stubDataAddr = remoteStub;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    int queued = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{}; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == targetPid) {
                    if (kQueueApc(drv, te.th32ThreadID, stubCodeAddr, stubDataAddr)) {
                        ++queued;
                    }
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    CloseHandle(drv);

    if (queued == 0) {
        err = "no APCs queued (no threads in target?)";
        return false;
    }
    util::logf("kernel APC queued to %d thread(s) of PID %lu", queued, targetPid);
    return true;
}

static bool kPayloadBegin(HANDLE drv, SIZE_T totalSize, const uint8_t* sessionKey, ULONGLONG& payloadIdOut) {
    RH_PAYLOAD_BEGIN req{};
    req.TotalEncryptedSize = totalSize;
    memcpy(req.SessionKey, sessionKey, RH_PAYLOAD_KEY_SIZE);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(drv, IOCTL_PAYLOAD_BEGIN, &req, sizeof(req), &req, sizeof(req), &ret, nullptr);
    payloadIdOut = req.PayloadId;
    SecureZeroMemory(req.SessionKey, sizeof(req.SessionKey));
    return ok != FALSE && payloadIdOut != 0;
}

static bool kPayloadChunk(HANDLE drv, ULONGLONG payloadId, ULONG offset, const uint8_t* data, ULONG chunkSize) {
    std::vector<uint8_t> buf(sizeof(RH_PAYLOAD_CHUNK_HDR) + chunkSize);
    auto* hdr = reinterpret_cast<RH_PAYLOAD_CHUNK_HDR*>(buf.data());
    hdr->PayloadId = payloadId;
    hdr->Offset = offset;
    hdr->ChunkSize = chunkSize;
    memcpy(buf.data() + sizeof(RH_PAYLOAD_CHUNK_HDR), data, chunkSize);
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_PAYLOAD_CHUNK, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr) != FALSE;
}

static bool kPayloadDecrypt(HANDLE drv, ULONGLONG payloadId) {
    RH_PAYLOAD_ID req{}; req.PayloadId = payloadId;
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_PAYLOAD_DECRYPT, &req, sizeof(req), &req, sizeof(req), &ret, nullptr) != FALSE;
}

static bool kPayloadGetMeta(HANDLE drv, ULONGLONG payloadId, RH_PAYLOAD_META& metaOut) {
    metaOut = {};
    metaOut.PayloadId = payloadId;
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_PAYLOAD_GET_META, &metaOut, sizeof(metaOut), &metaOut, sizeof(metaOut), &ret, nullptr) != FALSE;
}

static bool kPayloadGetImports(HANDLE drv, ULONGLONG payloadId, ULONG impBlobSize, std::vector<uint8_t>& blobOut) {
    ULONG total = sizeof(RH_PAYLOAD_IMP) + impBlobSize;
    blobOut.assign(total, 0);
    auto* hdr = reinterpret_cast<RH_PAYLOAD_IMP*>(blobOut.data());
    hdr->PayloadId = payloadId;
    hdr->BufferSize = impBlobSize;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(drv, IOCTL_PAYLOAD_GET_IMPORTS, blobOut.data(), (DWORD)blobOut.size(),
                              blobOut.data(), (DWORD)blobOut.size(), &ret, nullptr);
    if (!ok) return false;
    blobOut.resize(ret);
    return true;
}

static bool kPayloadSetIat(HANDLE drv, ULONGLONG payloadId, const std::vector<RH_IAT_ENTRY>& entries) {
    std::vector<uint8_t> buf(sizeof(RH_PAYLOAD_SET_IAT) + entries.size() * sizeof(RH_IAT_ENTRY));
    auto* hdr = reinterpret_cast<RH_PAYLOAD_SET_IAT*>(buf.data());
    hdr->PayloadId = payloadId;
    hdr->EntryCount = (ULONG)entries.size();
    if (!entries.empty()) {
        memcpy(buf.data() + sizeof(RH_PAYLOAD_SET_IAT), entries.data(), entries.size() * sizeof(RH_IAT_ENTRY));
    }
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_PAYLOAD_SET_IAT, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr) != FALSE;
}

static bool kPayloadInject(HANDLE drv, ULONGLONG payloadId, DWORD targetPid, ULONGLONG targetBase) {
    RH_PAYLOAD_INJECT req{};
    req.PayloadId = payloadId;
    req.TargetPid = (HANDLE)(ULONG_PTR)targetPid;
    req.TargetBase = targetBase;
    DWORD ret = 0;
    return DeviceIoControl(drv, IOCTL_PAYLOAD_INJECT, &req, sizeof(req), &req, sizeof(req), &ret, nullptr) != FALSE;
}

static void kPayloadRelease(HANDLE drv, ULONGLONG payloadId) {
    RH_PAYLOAD_ID req{}; req.PayloadId = payloadId;
    DWORD ret = 0;
    DeviceIoControl(drv, IOCTL_PAYLOAD_RELEASE, &req, sizeof(req), &req, sizeof(req), &ret, nullptr);
}

__declspec(noinline) bool kernelStreamMap(DWORD targetPid,
                                          const std::vector<uint8_t>& encryptedBlob,
                                          const std::vector<uint8_t>& sessionKey,
                                          std::string& err) {
    VMP_BEGIN_ULTRA("inject.kernelStream");
    if (sessionKey.size() != RH_PAYLOAD_KEY_SIZE) { err = "bad-key-size"; return false; }
    if (encryptedBlob.size() < 12 + 16 + 64) { err = "blob-too-small"; return false; }

    if (!isProcessAlive(targetPid)) {
        err = "target process died before inject";
        util::logf("kernelStreamMap: PID %lu not alive at entry", targetPid);
        return false;
    }

    HANDLE drv = openDevice();
    if (drv == INVALID_HANDLE_VALUE) { err = "driver not mapped"; return false; }

    if (!isProcessAlive(targetPid)) {
        CloseHandle(drv);
        err = "target process died after driver open";
        return false;
    }

    ULONGLONG payloadId = 0;
    if (!kPayloadBegin(drv, encryptedBlob.size(), sessionKey.data(), payloadId)) {
        CloseHandle(drv); err = "PAYLOAD_BEGIN failed"; return false;
    }

    {
        ULONG offset = 0;
        while (offset < encryptedBlob.size()) {
            ULONG remaining = (ULONG)encryptedBlob.size() - offset;
            ULONG take = remaining > RH_PAYLOAD_CHUNK_MAX ? RH_PAYLOAD_CHUNK_MAX : remaining;
            if (!kPayloadChunk(drv, payloadId, offset, encryptedBlob.data() + offset, take)) {
                kPayloadRelease(drv, payloadId);
                CloseHandle(drv); err = "PAYLOAD_CHUNK failed"; return false;
            }
            offset += take;
        }
    }

    if (!kPayloadDecrypt(drv, payloadId)) {
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = "PAYLOAD_DECRYPT failed"; return false;
    }

    RH_PAYLOAD_META meta{};
    if (!kPayloadGetMeta(drv, payloadId, meta)) {
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = "PAYLOAD_GET_META failed"; return false;
    }

    std::string targetAllocErr;
    ULONGLONG targetBase = kAllocCodeRegion(drv, targetPid, meta.SizeOfImage, targetAllocErr);
    if (!targetBase) {
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = std::string("kernel alloc image failed") + targetAllocErr; return false;
    }

    std::vector<RH_IAT_ENTRY> resolved;
    if (meta.ImportCount > 0 && meta.ImportBlobSize > 0) {
        std::vector<uint8_t> impBlob;
        if (!kPayloadGetImports(drv, payloadId, meta.ImportBlobSize, impBlob)) {
            kPayloadRelease(drv, payloadId);
            CloseHandle(drv); err = "PAYLOAD_GET_IMPORTS failed"; return false;
        }
        if (impBlob.size() < sizeof(RH_PAYLOAD_IMP) + sizeof(ULONG)) {
            kPayloadRelease(drv, payloadId);
            CloseHandle(drv); err = "imports blob malformed"; return false;
        }

        const uint8_t* p = impBlob.data() + sizeof(RH_PAYLOAD_IMP);
        ULONG count = *reinterpret_cast<const ULONG*>(p);
        p += sizeof(ULONG);
        if (count != meta.ImportCount) {
            kPayloadRelease(drv, payloadId);
            CloseHandle(drv); err = "imports count mismatch"; return false;
        }

        const RH_IMP_ENTRY* entries = reinterpret_cast<const RH_IMP_ENTRY*>(p);
        const uint8_t* names = reinterpret_cast<const uint8_t*>(entries + count);
        const uint8_t* namesEnd = impBlob.data() + impBlob.size();

        std::string lastDll;
        HMODULE lastMod = nullptr;
        const uint8_t* nameCursor = names;

        resolved.reserve(count);
        for (ULONG i = 0; i < count; ++i) {
            const RH_IMP_ENTRY& e = entries[i];

            if (nameCursor + e.DllNameLen + 1 > namesEnd) {
                kPayloadRelease(drv, payloadId);
                CloseHandle(drv); err = "imports overflow dll"; return false;
            }
            std::string dllName((const char*)nameCursor, e.DllNameLen);
            nameCursor += e.DllNameLen + 1;

            std::string funcName;
            if (e.FuncNameLen > 0) {
                if (nameCursor + e.FuncNameLen + 1 > namesEnd) {
                    kPayloadRelease(drv, payloadId);
                    CloseHandle(drv); err = "imports overflow func"; return false;
                }
                funcName.assign((const char*)nameCursor, e.FuncNameLen);
                nameCursor += e.FuncNameLen + 1;
            }

            if (dllName != lastDll) {
                lastMod = LoadLibraryA(dllName.c_str());
                if (!lastMod) {
                    kPayloadRelease(drv, payloadId);
                    CloseHandle(drv); err = std::string("LoadLibrary failed: ") + dllName; return false;
                }
                lastDll = dllName;
            }

            FARPROC proc = nullptr;
            if (e.FuncNameLen == 0) {
                proc = GetProcAddress(lastMod, (LPCSTR)(uintptr_t)e.Ordinal);
            } else {
                proc = GetProcAddress(lastMod, funcName.c_str());
            }
            if (!proc) {
                kPayloadRelease(drv, payloadId);
                CloseHandle(drv);
                err = std::string("GetProcAddress failed: ") + dllName + "!" +
                      (e.FuncNameLen ? funcName : std::string("#") + std::to_string(e.Ordinal));
                return false;
            }

            RH_IAT_ENTRY ie{};
            ie.IatRva = e.IatRva;
            ie.ResolvedAddress = (ULONGLONG)proc;
            resolved.push_back(ie);
        }
    }

    if (!resolved.empty()) {
        if (!kPayloadSetIat(drv, payloadId, resolved)) {
            kPayloadRelease(drv, payloadId);
            CloseHandle(drv); err = "PAYLOAD_SET_IAT failed"; return false;
        }
    }

    if (!kPayloadInject(drv, payloadId, targetPid, targetBase)) {
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = "PAYLOAD_INJECT failed"; return false;
    }

    std::vector<BYTE> stubBuf(kStubTotalSize, 0);
    LONG flag = 0;
    memcpy(stubBuf.data() + 0x00, &flag, sizeof(flag));
    ULONGLONG dllMainAddr = targetBase + meta.EntryPointRva;
    memcpy(stubBuf.data() + 0x08, &dllMainAddr, sizeof(dllMainAddr));
    ULONGLONG imageBaseFinal = targetBase;
    memcpy(stubBuf.data() + 0x10, &imageBaseFinal, sizeof(imageBaseFinal));
    memcpy(stubBuf.data() + kStubDataSize, kStubCode, sizeof(kStubCode));

    std::string stubAllocErr;
    ULONGLONG remoteStub = kAllocCodeRegion(drv, targetPid, kStubTotalSize, stubAllocErr);
    if (!remoteStub) {
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = std::string("kernel alloc stub failed") + stubAllocErr; return false;
    }

    LONG ws3 = 0;
    if (!kWriteMem(drv, targetPid, remoteStub, stubBuf.data(), stubBuf.size(), &ws3)) {
        char b[64]; StringCchPrintfA(b, _countof(b), " (status=0x%08X)", (unsigned)ws3);
        kPayloadRelease(drv, payloadId);
        CloseHandle(drv); err = std::string("kernel write stub failed") + b; return false;
    }

    ULONGLONG stubCodeAddr = remoteStub + kStubDataSize;
    ULONGLONG stubDataAddr = remoteStub;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    int queued = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{}; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == targetPid) {
                    if (kQueueApc(drv, te.th32ThreadID, stubCodeAddr, stubDataAddr)) {
                        ++queued;
                    }
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    kPayloadRelease(drv, payloadId);
    CloseHandle(drv);

    if (queued == 0) {
        err = "no APCs queued";
        return false;
    }
    util::logf("kernel-stream APC queued to %d thread(s) of PID %lu", queued, targetPid);
    return true;
}

bool runKdmapper(const std::wstring& kdmapperExe,
                 const std::vector<uint8_t>& sysImage,
                 std::string& err) {
    std::wstring kdmDir = kdmapperExe;
    size_t slash = kdmDir.find_last_of(L"\\/");
    std::wstring kdmName;
    if (slash != std::wstring::npos) {
        kdmName = kdmDir.substr(slash + 1);
        kdmDir.resize(slash);
    } else {
        kdmName = kdmapperExe;
        wchar_t tmpDir[MAX_PATH] = { 0 };
        GetTempPathW(MAX_PATH, tmpDir);
        kdmDir = tmpDir;
    }

    auto rnd = util::randomHex(6);
    std::wstring driverName = L"rh" + util::s2ws(rnd) + L".sys";
    std::wstring driverPath = kdmDir + L"\\" + driverName;

    HANDLE h = CreateFileW(driverPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "kdmapper-tmpfile (Defender real-time blocked .sys?)";
        return false;
    }
    DWORD wrote = 0;
    WriteFile(h, sysImage.data(), (DWORD)sysImage.size(), &wrote, nullptr);
    CloseHandle(h);

    util::sleepMs(150);
    if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "driver .sys vanished after write (Defender quarantined)";
        return false;
    }

    HANDLE rdR = nullptr, rdW = nullptr;
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    CreatePipe(&rdR, &rdW, &sa, 0);
    SetHandleInformation(rdR, HANDLE_FLAG_INHERIT, 0);

    std::wstring args = L"\"" + kdmName + L"\" \"" + driverName + L"\"";

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = rdW;
    si.hStdError   = rdW;
    si.hStdInput   = nullptr;
    PROCESS_INFORMATION pi{};

    util::logf("kdmapper launch: app=%ls cwd=%ls args=%ls",
               kdmapperExe.c_str(), kdmDir.c_str(), args.c_str());

    if (!CreateProcessW(kdmapperExe.c_str(), args.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, kdmDir.c_str(), &si, &pi)) {
        DWORD le = GetLastError();
        CloseHandle(rdR); CloseHandle(rdW);
        DeleteFileW(driverPath.c_str());
        char buf[64]; _snprintf_s(buf, _TRUNCATE, "CreateProcess-kdmapper-le%lu", le);
        err = buf;
        return false;
    }
    CloseHandle(rdW);

    std::string captured;
    {
        char chunk[1024];
        DWORD got = 0;
        while (ReadFile(rdR, chunk, sizeof(chunk) - 1, &got, nullptr) && got > 0) {
            chunk[got] = 0;
            captured.append(chunk, got);
            if (captured.size() > 64 * 1024) break;
        }
    }
    CloseHandle(rdR);

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(driverPath.c_str());

    if (!captured.empty()) {
        std::string s = captured;
        for (auto& c : s) if (c == '\r' || c == '\n') c = ' ';
        if (s.size() > 480) s.resize(480);
        util::logf("kdmapper out: %s", s.c_str());
    } else {
        util::logf("kdmapper out: <empty>");
    }

    if (code != 0) {
        std::string snippet = captured;
        for (auto& c : snippet) if (c == '\r' || c == '\n') c = ' ';
        if (snippet.size() > 160) snippet.resize(160);
        err = "kdmapper-exit-" + std::to_string((int32_t)code) + " | " + snippet;
        return false;
    }
    return true;
}

static bool runHidden(const std::wstring& cmd, DWORD timeoutMs, DWORD& exitCode) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, timeoutMs);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

static void tryDefenderExclusion(const std::wstring& dirPath) {
    DWORD code = 0;
    std::wstring c1 =
        L"powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass "
        L"-Command \"try { Add-MpPreference -ExclusionPath '" + dirPath +
        L"' -ErrorAction SilentlyContinue } catch {}\"";
    runHidden(c1, 8000, code);
    util::logf("defender exclusion attempt: code=%lu", code);
}

__declspec(noinline) bool runEmbeddedKdmapper(const std::vector<uint8_t>& sysImage,
                         std::string& err) {
    VMP_BEGIN_MUTATION("inject.kdmExtract");

    std::vector<uint8_t> dec;
    if (!resources::loadKdmapperExe(dec) || dec.empty()) {
        err = "kdmapper: load+decrypt failed";
        return false;
    }

    wchar_t tmpDir[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmpDir);

    tryDefenderExclusion(tmpDir);

    auto rnd = util::randomHex(6);
    std::wstring kdmExe = std::wstring(tmpDir) + L"rk" + util::s2ws(rnd) + L".exe";

    HANDLE hk = CreateFileW(kdmExe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hk == INVALID_HANDLE_VALUE) {
        SecureZeroMemory(dec.data(), dec.size());
        err = "kdmapper write failed (Defender real-time blocked it?)";
        return false;
    }
    DWORD wrote = 0;
    BOOL wok = WriteFile(hk, dec.data(), (DWORD)dec.size(), &wrote, nullptr);
    CloseHandle(hk);
    size_t decSize = dec.size();
    SecureZeroMemory(dec.data(), dec.size());
    dec.clear();
    dec.shrink_to_fit();
    if (!wok || wrote != decSize) {
        DeleteFileW(kdmExe.c_str());
        err = "kdmapper write incomplete (Defender quarantined?)";
        return false;
    }

    util::sleepMs(150);
    DWORD attrs = GetFileAttributesW(kdmExe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        err = "kdmapper file vanished after write (Defender quarantined)";
        return false;
    }

    bool ok = runKdmapper(kdmExe, sysImage, err);
    DeleteFileW(kdmExe.c_str());
    return ok;
}

bool unloadDriver() {
    HANDLE drv = openDevice();
    if (drv == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(drv, IOCTL_UNLOAD_DRIVER, nullptr, 0, nullptr, 0, &ret, nullptr);
    CloseHandle(drv);
    return ok != FALSE;
}

}
