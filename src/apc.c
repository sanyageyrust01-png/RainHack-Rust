#include "apc.h"

typedef enum _KAPC_ENVIRONMENT {
    OriginalApcEnvironment,
    AttachedApcEnvironment,
    CurrentApcEnvironment,
    InsertApcEnvironment
} KAPC_ENVIRONMENT;

typedef VOID (*PKNORMAL_ROUTINE)(PVOID, PVOID, PVOID);
typedef VOID (*PKKERNEL_ROUTINE)(PRKAPC, PKNORMAL_ROUTINE*, PVOID*, PVOID*, PVOID*);
typedef VOID (*PKRUNDOWN_ROUTINE)(PRKAPC);

NTKERNELAPI VOID KeInitializeApc(
    PRKAPC Apc, PETHREAD Thread, KAPC_ENVIRONMENT Environment,
    PKKERNEL_ROUTINE KernelRoutine, PKRUNDOWN_ROUTINE RundownRoutine,
    PKNORMAL_ROUTINE NormalRoutine, KPROCESSOR_MODE ProcessorMode, PVOID NormalContext
);

NTKERNELAPI BOOLEAN KeInsertQueueApc(
    PRKAPC Apc, PVOID SystemArgument1, PVOID SystemArgument2, KPRIORITY Increment
);

typedef struct _KAPC_INTERNAL {
    UCHAR              Type;
    UCHAR              SpareByte0;
    UCHAR              Size;
    UCHAR              SpareByte1;
    ULONG              SpareLong0;
    PKTHREAD           Thread;
    LIST_ENTRY         ApcListEntry;
    PKKERNEL_ROUTINE   KernelRoutine;
    PKRUNDOWN_ROUTINE  RundownRoutine;
    PKNORMAL_ROUTINE   NormalRoutine;
    PVOID              NormalContext;
    PVOID              SystemArgument1;
    PVOID              SystemArgument2;
    CHAR               ApcStateIndex;
    CHAR               ApcMode;
    BOOLEAN            Inserted;
} KAPC_INTERNAL, *PKAPC_INTERNAL;

VOID ApcKernelRoutine(PRKAPC Apc, PKNORMAL_ROUTINE* NormalRoutine, PVOID* NormalContext, PVOID* SystemArgument1, PVOID* SystemArgument2) {
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExFreePoolWithTag(Apc, 'CPA_');
}

VOID ApcRundownRoutine(PRKAPC Apc) {
    ExFreePoolWithTag(Apc, 'CPA_');
}

NTSTATUS QueueUserApc(HANDLE ThreadId, PVOID NormalRoutine, PVOID NormalContext, PVOID SysArg1, PVOID SysArg2) {
    PETHREAD Thread;
    NTSTATUS Status = PsLookupThreadByThreadId(ThreadId, &Thread);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    PRKAPC Apc = (PRKAPC)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(KAPC), 'CPA_');
    if (!Apc) {
        ObDereferenceObject(Thread);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeApc(
        Apc,
        Thread,
        OriginalApcEnvironment,
        ApcKernelRoutine,
        ApcRundownRoutine,
        (PKNORMAL_ROUTINE)NormalRoutine,
        UserMode,
        NormalContext
    );

    ((PKAPC_INTERNAL)Apc)->SpareByte0 = 1;

    BOOLEAN Inserted = KeInsertQueueApc(Apc, SysArg1, SysArg2, 2);

    ObDereferenceObject(Thread);

    if (!Inserted) {
        ExFreePoolWithTag(Apc, 'CPA_');
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}
