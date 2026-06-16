#pragma once
#include <ntifs.h>

NTSTATUS ReadProcessMemory(HANDLE ProcessId, PVOID SourceAddress, PVOID TargetAddress, SIZE_T Size, PSIZE_T BytesRead);
NTSTATUS WriteProcessMemory(HANDLE ProcessId, PVOID SourceAddress, PVOID TargetAddress, SIZE_T Size, PSIZE_T BytesWritten);
NTSTATUS AllocateVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS FreeVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS ProtectVirtualMemory(HANDLE ProcessId, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect);
NTSTATUS QueryProcessModule(HANDLE ProcessId, PCWSTR ModuleName, PULONGLONG BaseAddress, PSIZE_T ModuleSize);
