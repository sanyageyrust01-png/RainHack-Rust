#include <ntifs.h>
#include "../include/ioctl.h"
#include "memory.h"
#include "apc.h"
#include "payload.h"

NTKERNELAPI NTSTATUS ObSetSecurityObjectByPointer(
    PVOID Object,
    SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor);

UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\FREEWAREDEVICE");
UNICODE_STRING SymLinkName = RTL_CONSTANT_STRING(L"\\DosDevices\\FREEWAREDEVICE");
PDEVICE_OBJECT pDeviceObject = NULL;

static NTSTATUS SetNullDaclOnObject(PVOID Object) {
    SECURITY_DESCRIPTOR sd;
    NTSTATUS s1 = RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(s1)) return s1;
    NTSTATUS s2 = RtlSetDaclSecurityDescriptor(&sd, TRUE, NULL, FALSE);
    if (!NT_SUCCESS(s2)) return s2;
    return ObSetSecurityObjectByPointer(Object, DACL_SECURITY_INFORMATION, &sd);
}

NTSTATUS DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesIO = 0;

    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;

    if (code == IOCTL_READ_MEMORY) {
        PMEMORY_REQUEST req = (PMEMORY_REQUEST)sysBuf;
        if (req && inLen >= sizeof(MEMORY_REQUEST) && outLen >= sizeof(MEMORY_REQUEST)) {
            NTSTATUS sub = ReadProcessMemory(req->ProcessId, (PVOID)req->TargetAddress, (PVOID)req->BufferAddress, req->Size, &req->ReturnSize);
            req->Status = (LONG)sub;
            bytesIO = sizeof(MEMORY_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_WRITE_MEMORY) {
        PMEMORY_REQUEST req = (PMEMORY_REQUEST)sysBuf;
        if (req && inLen >= sizeof(MEMORY_REQUEST) && outLen >= sizeof(MEMORY_REQUEST)) {
            NTSTATUS sub = WriteProcessMemory(req->ProcessId, (PVOID)req->BufferAddress, (PVOID)req->TargetAddress, req->Size, &req->ReturnSize);
            req->Status = (LONG)sub;
            bytesIO = sizeof(MEMORY_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_ALLOC_MEMORY) {
        PALLOC_REQUEST req = (PALLOC_REQUEST)sysBuf;
        if (req && inLen >= sizeof(ALLOC_REQUEST) && outLen >= sizeof(ALLOC_REQUEST)) {
            PVOID addr = (PVOID)req->Address;
            NTSTATUS sub = AllocateVirtualMemory(req->ProcessId, &addr, &req->Size, req->AllocationType, req->Protect);
            req->Address = (ULONGLONG)addr;
            req->Status = (LONG)sub;
            DbgPrint("[RH][drv] IOCTL_ALLOC_MEMORY pid=%p size=%zu type=0x%X prot=0x%X status=0x%08X addr=%p\n",
                     req->ProcessId, req->Size, req->AllocationType, req->Protect, sub, addr);
            bytesIO = sizeof(ALLOC_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_FREE_MEMORY) {
        PFREE_REQUEST req = (PFREE_REQUEST)sysBuf;
        if (req && inLen >= sizeof(FREE_REQUEST) && outLen >= sizeof(FREE_REQUEST)) {
            PVOID addr = (PVOID)req->Address;
            NTSTATUS sub = FreeVirtualMemory(req->ProcessId, &addr, &req->Size, req->FreeType);
            req->Address = (ULONGLONG)addr;
            req->Status = (LONG)sub;
            bytesIO = sizeof(FREE_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PROTECT_MEMORY) {
        PPROTECT_REQUEST req = (PPROTECT_REQUEST)sysBuf;
        if (req && inLen >= sizeof(PROTECT_REQUEST) && outLen >= sizeof(PROTECT_REQUEST)) {
            PVOID addr = (PVOID)req->Address;
            ULONG oldProt = 0;
            NTSTATUS sub = ProtectVirtualMemory(req->ProcessId, &addr, &req->Size, req->NewProtect, &oldProt);
            req->Address = (ULONGLONG)addr;
            req->OldProtect = oldProt;
            req->Status = (LONG)sub;
            DbgPrint("[RH][drv] IOCTL_PROTECT_MEMORY pid=%p addr=%p size=%zu newProt=0x%X status=0x%08X oldProt=0x%X\n",
                     req->ProcessId, addr, req->Size, req->NewProtect, sub, oldProt);
            bytesIO = sizeof(PROTECT_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_QUERY_MODULE) {
        PMODULE_QUERY_REQUEST req = (PMODULE_QUERY_REQUEST)sysBuf;
        if (req && inLen >= sizeof(MODULE_QUERY_REQUEST) && outLen >= sizeof(MODULE_QUERY_REQUEST)) {
            req->ModuleName[63] = 0;
            ULONGLONG base = 0;
            SIZE_T sz = 0;
            NTSTATUS sub = QueryProcessModule(req->ProcessId, req->ModuleName, &base, &sz);
            req->BaseAddress = base;
            req->Size = sz;
            req->Status = (LONG)sub;
            bytesIO = sizeof(MODULE_QUERY_REQUEST);
            status = STATUS_SUCCESS;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_QUEUE_APC) {
        PAPC_REQUEST req = (PAPC_REQUEST)sysBuf;
        if (req && inLen >= sizeof(APC_REQUEST)) {
            status = QueueUserApc(req->ThreadId, (PVOID)req->NormalRoutine, (PVOID)req->NormalContext, (PVOID)req->SystemArgument1, (PVOID)req->SystemArgument2);
            bytesIO = sizeof(APC_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_BEGIN) {
        if (inLen >= sizeof(PAYLOAD_BEGIN_REQUEST) && outLen >= sizeof(PAYLOAD_BEGIN_REQUEST)) {
            status = PayloadBegin((PPAYLOAD_BEGIN_REQUEST)sysBuf);
            bytesIO = sizeof(PAYLOAD_BEGIN_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_CHUNK) {
        status = PayloadChunk(sysBuf, inLen);
        bytesIO = 0;
    }
    else if (code == IOCTL_PAYLOAD_DECRYPT) {
        if (inLen >= sizeof(PAYLOAD_ID_REQUEST)) {
            status = PayloadDecrypt((PPAYLOAD_ID_REQUEST)sysBuf);
            bytesIO = sizeof(PAYLOAD_ID_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_GET_META) {
        if (inLen >= sizeof(PAYLOAD_META_REQUEST) && outLen >= sizeof(PAYLOAD_META_REQUEST)) {
            status = PayloadGetMeta((PPAYLOAD_META_REQUEST)sysBuf);
            bytesIO = sizeof(PAYLOAD_META_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_GET_IMPORTS) {
        if (inLen >= sizeof(PAYLOAD_IMP_REQUEST)) {
            ULONG written = 0;
            status = PayloadGetImports((PPAYLOAD_IMP_REQUEST)sysBuf, sysBuf, outLen, &written);
            bytesIO = written;
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_SET_IAT) {
        status = PayloadSetIat(sysBuf, inLen);
        bytesIO = 0;
    }
    else if (code == IOCTL_PAYLOAD_INJECT) {
        if (inLen >= sizeof(PAYLOAD_INJECT_REQUEST)) {
            status = PayloadInject((PPAYLOAD_INJECT_REQUEST)sysBuf);
            bytesIO = sizeof(PAYLOAD_INJECT_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_PAYLOAD_RELEASE) {
        if (inLen >= sizeof(PAYLOAD_ID_REQUEST)) {
            status = PayloadRelease((PPAYLOAD_ID_REQUEST)sysBuf);
            bytesIO = sizeof(PAYLOAD_ID_REQUEST);
        } else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == IOCTL_UNLOAD_DRIVER) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        PayloadShutdown();
        IoDeleteSymbolicLink(&SymLinkName);
        IoDeleteDevice(pDeviceObject);
        return STATUS_SUCCESS;
    }
    else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesIO;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS UnloadDriver(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    PayloadShutdown();
    IoDeleteSymbolicLink(&SymLinkName);
    IoDeleteDevice(pDeviceObject);
    return STATUS_SUCCESS;
}

NTSTATUS DriverInitialize(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);
    NTSTATUS status = IoCreateDevice(
        DriverObject, 0, &DeviceName,
        FILE_DEVICE_UNKNOWN, 0, FALSE,
        &pDeviceObject);
    DbgPrint("[RH][drv] IoCreateDevice status=0x%08X dev=%p\n", status, pDeviceObject);
    if (!NT_SUCCESS(status)) return status;

    status = PayloadInit();
    DbgPrint("[RH][drv] PayloadInit status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(pDeviceObject);
        return status;
    }

    {
        NTSTATUS sDev = SetNullDaclOnObject(pDeviceObject);
        DbgPrint("[RH][drv] NULL DACL on device: 0x%08X\n", sDev);

        status = IoCreateSymbolicLink(&SymLinkName, &DeviceName);
        DbgPrint("[RH][drv] IoCreateSymbolicLink status=0x%08X name=%wZ\n",
            status, &SymLinkName);
        if (!NT_SUCCESS(status)) {
            IoDeleteDevice(pDeviceObject);
            return status;
        }

        {
            OBJECT_ATTRIBUTES oa;
            HANDLE hSym = NULL;
            SECURITY_DESCRIPTOR sd;
            NTSTATUS sOpen, sSet = STATUS_UNSUCCESSFUL;
            InitializeObjectAttributes(&oa, &SymLinkName,
                OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
            sOpen = ZwOpenSymbolicLinkObject(&hSym, WRITE_DAC, &oa);
            if (NT_SUCCESS(sOpen)) {
                RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
                RtlSetDaclSecurityDescriptor(&sd, TRUE, NULL, FALSE);
                sSet = ZwSetSecurityObject(hSym, DACL_SECURITY_INFORMATION, &sd);
                ZwClose(hSym);
            }
            DbgPrint("[RH][drv] NULL DACL on symlink: open=0x%08X set=0x%08X\n", sOpen, sSet);
        }
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    DriverObject->DriverUnload = UnloadDriver;

    pDeviceObject->Flags |= DO_BUFFERED_IO;
    pDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("[RH][drv] DriverInitialize complete OK\n");
    return STATUS_SUCCESS;
}

NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName, PDRIVER_INITIALIZE InitializationFunction);

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrint("[RH][drv] DriverEntry called drvObj=%p\n", DriverObject);
    UNICODE_STRING drvName = RTL_CONSTANT_STRING(L"\\Driver\\IDDoSYF");
    NTSTATUS s = IoCreateDriver(&drvName, &DriverInitialize);
    DbgPrint("[RH][drv] IoCreateDriver status=0x%08X\n", s);
    return s;
}
