#pragma once

#include "Config.h"

#if RAINHACK_ENABLE_VMPROTECT
#include "../vmprotect/VMProtectSDK.h"
struct VmpEndGuard { ~VmpEndGuard() { VMProtectEnd(); } };
#else

#define VMProtectBegin(s)                ((void)0)
#define VMProtectBeginVirtualization(s)  ((void)0)
#define VMProtectBeginMutation(s)        ((void)0)
#define VMProtectBeginUltra(s)           ((void)0)
#define VMProtectEnd()                   ((void)0)
struct VmpEndGuard {};

inline bool VMProtectIsDebuggerPresent(bool /*checkKernelMode*/) { return false; }
inline bool VMProtectIsVirtualMachinePresent(void)               { return false; }
inline bool VMProtectIsValidImageCRC(void)                       { return true;  }

#endif

#define VMP_BEGIN_ULTRA(name)            VMProtectBeginUltra(name);          VmpEndGuard _vmp_g_##__LINE__{}
#define VMP_BEGIN_MUTATION(name)         VMProtectBeginMutation(name);       VmpEndGuard _vmp_g_##__LINE__{}
#define VMP_BEGIN_VIRTUALIZATION(name)   VMProtectBeginVirtualization(name); VmpEndGuard _vmp_g_##__LINE__{}
#define VMP_BEGIN(name)                  VMProtectBegin(name);               VmpEndGuard _vmp_g_##__LINE__{}
