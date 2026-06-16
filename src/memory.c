#include "memory.h"

NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress, PEPROCESS TargetProcess, PVOID TargetAddress, SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode, PSIZE_T ReturnSize);
NTKERNELAPI NTSTATUS NTAPI ZwAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTKERNELAPI NTSTATUS NTAPI ZwFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTKERNELAPI NTSTATUS NTAPI ZwProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect);
NTKERNELAPI NTSTATUS NTAPI MmProtectMdlSystemAddress(PMDL MemoryDescriptorList, ULONG NewProtect);

NTSTATUS ReadProcessMemory(HANDLE ProcessId, PVOID SourceAddress, PVOID TargetAddress, SIZE_T Size, PSIZE_T BytesRead) {
    PEPROCESS Process;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) return Status;
    SIZE_T OutBytes = 0;
    Status = MmCopyVirtualMemory(Process, SourceAddress, PsGetCurrentProcess(), TargetAddress, Size, KernelMode, &OutBytes);
    if (BytesRead) *BytesRead = OutBytes;
    ObDereferenceObject(Process);
    return Status;
}

static NTSTATUS WriteViaMdl(PEPROCESS Process, PVOID Source, PVOID Target, SIZE_T Size, PSIZE_T BytesWritten) {
    NTSTATUS status;
    KAPC_STATE apc;
    PVOID localBuf;
    PMDL mdl;
    PVOID mapped;

    if (BytesWritten) *BytesWritten = 0;

    localBuf = ExAllocatePoolWithTag(NonPagedPool, Size, 'RHwm');
    if (!localBuf) {
        DbgPrint("[RH][drv] WriteViaMdl: pool alloc failed size=%zu\n", Size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeStackAttachProcess(Process, &apc);

    __try {
        ProbeForRead(Source, Size, 1);
        RtlCopyMemory(localBuf, Source, Size);
        status = STATUS_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        DbgPrint("[RH][drv] WriteViaMdl: ProbeForRead failed 0x%08X src=%p\n", status, Source);
        KeUnstackDetachProcess(&apc);
        ExFreePoolWithTag(localBuf, 'RHwm');
        return status;
    }

    mdl = IoAllocateMdl(Target, (ULONG)Size, FALSE, FALSE, NULL);
    if (!mdl) {
        DbgPrint("[RH][drv] WriteViaMdl: IoAllocateMdl failed target=%p size=%zu\n", Target, Size);
        KeUnstackDetachProcess(&apc);
        ExFreePoolWithTag(localBuf, 'RHwm');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        DbgPrint("[RH][drv] WriteViaMdl: MmProbeAndLockPages failed 0x%08X target=%p\n", status, Target);
        IoFreeMdl(mdl);
        KeUnstackDetachProcess(&apc);
        ExFreePoolWithTag(localBuf, 'RHwm');
        return status;
    }

    mapped = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);
    if (!mapped) {
        DbgPrint("[RH][drv] WriteViaMdl: MmMapLockedPagesSpecifyCache failed\n");
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        KeUnstackDetachProcess(&apc);
        ExFreePoolWithTag(localBuf, 'RHwm');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = MmProtectMdlSystemAddress(mdl, PAGE_READWRITE);
    DbgPrint("[RH][drv] WriteViaMdl: MmProtectMdlSystemAddress=0x%08X mapped=%p target=%p size=%zu\n",
             status, mapped, Target, Size);

    if (NT_SUCCESS(status)) {
        __try {
            RtlCopyMemory(mapped, localBuf, Size);
            if (BytesWritten) *BytesWritten = Size;
            status = STATUS_SUCCESS;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            DbgPrint("[RH][drv] WriteViaMdl: RtlCopyMemory threw 0x%08X\n", status);
        }
    }

    MmUnmapLockedPages(mapped, mdl);
    MmUnlockPages(mdl);
    IoFreeMdl(mdl);
    KeUnstackDetachProcess(&apc);
    ExFreePoolWithTag(localBuf, 'RHwm');
    return status;
}

NTSTATUS WriteProcessMemory(HANDLE ProcessId, PVOID SourceAddress, PVOID TargetAddress, SIZE_T Size, PSIZE_T BytesWritten) {
    PEPROCESS Process;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] WriteProcessMemory: PsLookup failed 0x%08X pid=%p\n", Status, ProcessId);
        return Status;
    }

    SIZE_T OutBytes = 0;
    Status = MmCopyVirtualMemory(PsGetCurrentProcess(), SourceAddress, Process, TargetAddress, Size, KernelMode, &OutBytes);

    if (Status != STATUS_SUCCESS || OutBytes != Size) {
        DbgPrint("[RH][drv] MmCopy write failed 0x%08X out=%zu/%zu target=%p -> MDL fallback\n",
                 Status, OutBytes, Size, TargetAddress);
        OutBytes = 0;
        Status = WriteViaMdl(Process, SourceAddress, TargetAddress, Size, &OutBytes);
        DbgPrint("[RH][drv] MDL write status=0x%08X out=%zu/%zu\n", Status, OutBytes, Size);
    }

    if (BytesWritten) *BytesWritten = OutBytes;
    ObDereferenceObject(Process);
    return Status;
}

NTSTATUS AllocateVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
    PEPROCESS Process;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] AllocateVirtualMemory: PsLookup failed 0x%08X pid=%p\n", Status, ProcessId);
        return Status;
    }

    KAPC_STATE ApcState;
    KeStackAttachProcess(Process, &ApcState);
    Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), BaseAddress, 0, RegionSize, AllocationType, Protect);
    KeUnstackDetachProcess(&ApcState);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] ZwAllocateVirtualMemory failed 0x%08X pid=%p size=%zu type=0x%X prot=0x%X\n",
                 Status, ProcessId, RegionSize ? *RegionSize : 0, AllocationType, Protect);
    }
    ObDereferenceObject(Process);
    return Status;
}

NTSTATUS FreeVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType) {
    PEPROCESS Process;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) return Status;

    KAPC_STATE ApcState;
    KeStackAttachProcess(Process, &ApcState);
    Status = ZwFreeVirtualMemory(ZwCurrentProcess(), BaseAddress, RegionSize, FreeType);
    KeUnstackDetachProcess(&ApcState);
    ObDereferenceObject(Process);
    return Status;
}

NTSTATUS ProtectVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect) {
    PEPROCESS Process;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] ProtectVirtualMemory: PsLookup failed 0x%08X pid=%p\n", Status, ProcessId);
        return Status;
    }

    KAPC_STATE ApcState;
    KeStackAttachProcess(Process, &ApcState);
    ULONG localOld = 0;
    Status = ZwProtectVirtualMemory(ZwCurrentProcess(), BaseAddress, RegionSize, NewProtect, &localOld);
    KeUnstackDetachProcess(&ApcState);
    if (OldProtect) *OldProtect = localOld;
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] ZwProtectVirtualMemory failed 0x%08X pid=%p addr=%p size=%zu prot=0x%X\n",
                 Status, ProcessId, BaseAddress ? *BaseAddress : NULL, RegionSize ? *RegionSize : 0, NewProtect);
    }
    ObDereferenceObject(Process);
    return Status;
}

typedef struct _RH_PEB_LDR_DATA {
    ULONG  Length;
    UCHAR  Initialized;
    PVOID  SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} RH_PEB_LDR_DATA, *PRH_PEB_LDR_DATA;

typedef struct _RH_PEB {
    UCHAR InheritedAddressSpace;
    UCHAR ReadImageFileExecOptions;
    UCHAR BeingDebugged;
    UCHAR BitField;
    PVOID Mutant;
    PVOID ImageBaseAddress;
    PRH_PEB_LDR_DATA Ldr;
} RH_PEB, *PRH_PEB;

typedef struct _RH_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} RH_LDR_DATA_TABLE_ENTRY, *PRH_LDR_DATA_TABLE_ENTRY;

NTKERNELAPI PVOID NTAPI PsGetProcessPeb(PEPROCESS Process);

NTSTATUS QueryProcessModule(HANDLE ProcessId, PCWSTR ModuleName, PULONGLONG BaseAddress, PSIZE_T ModuleSize) {
    if (BaseAddress) *BaseAddress = 0;
    if (ModuleSize)  *ModuleSize  = 0;

    if (!ModuleName || !BaseAddress || !ModuleSize) return STATUS_INVALID_PARAMETER;

    PEPROCESS Process = NULL;
    NTSTATUS Status = PsLookupProcessByProcessId(ProcessId, &Process);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("[RH][drv] QueryProcessModule: PsLookup failed 0x%08X pid=%p\n", Status, ProcessId);
        return Status;
    }

    KAPC_STATE apc;
    KeStackAttachProcess(Process, &apc);

    NTSTATUS result = STATUS_NOT_FOUND;

    __try {
        PRH_PEB peb = (PRH_PEB)PsGetProcessPeb(Process);
        if (!peb || !peb->Ldr) {
            result = STATUS_INVALID_ADDRESS;
        } else {
            UNICODE_STRING reqName;
            RtlInitUnicodeString(&reqName, ModuleName);

            PLIST_ENTRY head  = &peb->Ldr->InLoadOrderModuleList;
            PLIST_ENTRY entry = head->Flink;
            ULONG safety = 0;

            while (entry && entry != head && safety++ < 4096) {
                PRH_LDR_DATA_TABLE_ENTRY mod = CONTAINING_RECORD(entry, RH_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
                if (mod->BaseDllName.Buffer && mod->BaseDllName.Length > 0) {
                    if (RtlEqualUnicodeString(&reqName, &mod->BaseDllName, TRUE)) {
                        *BaseAddress = (ULONGLONG)mod->DllBase;
                        *ModuleSize  = mod->SizeOfImage;
                        result = STATUS_SUCCESS;
                        break;
                    }
                }
                entry = entry->Flink;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = GetExceptionCode();
        DbgPrint("[RH][drv] QueryProcessModule: SEH 0x%08X\n", result);
    }

    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(Process);
    return result;
}
