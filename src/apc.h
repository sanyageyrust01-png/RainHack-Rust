#pragma once
#include <ntifs.h>

NTSTATUS QueueUserApc(HANDLE ThreadId, PVOID NormalRoutine, PVOID NormalContext, PVOID SysArg1, PVOID SysArg2);
