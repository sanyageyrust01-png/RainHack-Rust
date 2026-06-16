#pragma once

#include <ntifs.h>
#include "../include/ioctl.h"

NTSTATUS PayloadInit(VOID);
VOID     PayloadShutdown(VOID);

NTSTATUS PayloadBegin(PPAYLOAD_BEGIN_REQUEST Request);
NTSTATUS PayloadChunk(PVOID SystemBuffer, ULONG InputLength);
NTSTATUS PayloadDecrypt(PPAYLOAD_ID_REQUEST Request);
NTSTATUS PayloadGetMeta(PPAYLOAD_META_REQUEST Request);
NTSTATUS PayloadGetImports(PPAYLOAD_IMP_REQUEST Request, PVOID OutputBuffer, ULONG OutputLength, PULONG BytesReturned);
NTSTATUS PayloadSetIat(PVOID SystemBuffer, ULONG InputLength);
NTSTATUS PayloadInject(PPAYLOAD_INJECT_REQUEST Request);
NTSTATUS PayloadRelease(PPAYLOAD_ID_REQUEST Request);
