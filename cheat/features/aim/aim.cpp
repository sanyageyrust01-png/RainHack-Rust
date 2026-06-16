#include "aim.h"

#include "../esp/esp.h"

#include "../Menu/Menu.h"

#include "../offset/offsets.hpp"

#include "../il2cpp/il2cpp.h"

#include <imgui.h>

#include <cmath>

#include <cstdarg>

#include <cstdio>

#include <cstring>

#include <windows.h>

#include <TlHelp32.h>

#include <MinHook.h>

#include <emmintrin.h>



static void AimLog(const char* fmt, ...) {

    char buf[512];

    va_list args;

    va_start(args, fmt);

    vsnprintf(buf, sizeof(buf), fmt, args);

    va_end(args);

    OutputDebugStringA(buf);

    OutputDebugStringA("\n");

}



namespace Aim {

    extern uintptr_t currentTarget;

    extern float bestTargetFOV;

    extern float targetPosX, targetPosY, targetPosZ;

    extern float targetVelX, targetVelY, targetVelZ;

    extern float localEyeX, localEyeY, localEyeZ;

    extern uintptr_t localPlayer;

    extern unsigned long long lastTargetTick;

    extern bool currentTargetVisible;



    void* oClientInput = nullptr;



    // Which BasePlayer method did we actually hook. Determines where

    // to fetch the InputState* argument inside hkClientInput.

    enum HookedMethodKind {

        HK_NONE = 0,

        HK_INPUTSTATE_ARG,   // method signature: (this, InputState, [methodInfo])

        HK_NO_ARG,           // method signature: (this, [methodInfo]) — read serverInput from BasePlayer

    };

    static HookedMethodKind g_hookKind = HK_NONE;

    static const char* g_hookedName = "(none)";

    static int g_hookedArgc = -1;

    // Address of the function MinHook patched, used by EnableInputHook().

    static void* g_hookedFnPtr = nullptr;



    // InputState.current / InputMessage.aimAngles offsets (resolved at runtime)

    int currentOffset = 0x10;

    int aimAnglesOffset = 0x14;

    // BasePlayer.serverInput offset — used when we hooked a 0-arg method and need

    // to grab the InputState pointer from the player. Resolved at runtime.

    int serverInputOffset = 0;



    float currentPitch = 0.0f;

    float currentYaw = 0.0f;

    bool shouldSilentAim = false;



    // Dynamic weapon offsets — defaults come from the 2026-04-17 dump.

    int recoilOffset = 0x3A8;

    int aimSwayOffset = 0x3A0;

    int aimSwaySpeedOffset = 0x3A4;

    int aimConeOffset = 0x3B8;

    int hipAimConeOffset = 0x3BC;

    int aimConePenaltyMaxOffset = 0x3C4;

    int aimPenaltyPerShotOffset = 0x3C0;

    int aimconePenaltyOffset = 0;      // runtime accumulated penalty (float)

    int stancePenaltyOffset = 0;       // stance penalty (float)

    int stancePenaltyScaleOffset = 0x3D0;  // BaseProjectile.stancePenaltyScale — multiplier, zero it to kill stance contribution



    // PlayerEyes offsets for true silent aim.

    //

    // BasePlayer.player_eyes @ 0x2E0 is ENCRYPTED in build 22830770 — raw

    // 8 bytes must be passed through the shl+add+xor decryptor (same family

    // as entity_list, decrypt RVA 0x114D7B0) to get the real PlayerEyes

    // instance pointer.

    //

    // PlayerEyes.bodyRotation @ 0x50 is a Unity Quaternion (16 bytes, xyzw

    // floats). DoAttack reads eyes.BodyForward() which derives the shot

    // direction from this Quaternion. Writing our target-aim Quaternion

    // here right before DoAttack is the clean silent-aim path.

    int eyesOffset = 0x348;            // BasePlayer.eyes (May 2026 EntityRef<PlayerEyes>)

    int rotationLookOffset = 0x50;     // PlayerEyes.bodyRotation (Quaternion xyzw)

    bool eyesIsEncrypted = true;       // set to false if decryption proves unneeded



    // Cached PlayerEyes class pointer for runtime probing (see ProbeEyesOffset).

    Il2CppClass* g_peClass = nullptr;



    // One-shot flag: have we already tried to probe eyes/rotationLook at runtime?

    // Set true on first successful probe or after 10 failed attempts.

    bool g_eyeProbeDone = false;

    int  g_eyeProbeAttempts = 0;



    int recoilYawMinOffset = 0x18;

    int recoilYawMaxOffset = 0x1C;

    int recoilPitchMinOffset = 0x20;

    int recoilPitchMaxOffset = 0x24;

    int newRecoilOverrideOffset = 0x80;



    // Dynamic inventory-chain offsets — defaults from offsets.hpp, resolved at runtime.

    int dynInventoryOffset = (int)offsets::BasePlayer::playerInventory; // 0x690 (May 2026 build)

    int dynContainerBeltOffset = (int)offsets::PlayerInventory::container1; // 0x38 (May 2026 build)

    int dynItemListOffset = (int)offsets::ItemContainer::list; // 0x50 (May 2026 build)

    int dynHeldEntityOffset = (int)offsets::Item::heldEntity; // 0x40 (May 2026 build)



    TracerEvent  g_tracers[kMaxTracers] = {};

    volatile int g_tracerHead = 0;



    void PushTracer(float sx, float sy, float sz, float vx, float vy, float vz) {

        int idx = (g_tracerHead) % kMaxTracers;

        g_tracers[idx].startX = sx; g_tracers[idx].startY = sy; g_tracers[idx].startZ = sz;

        g_tracers[idx].velX   = vx; g_tracers[idx].velY   = vy; g_tracers[idx].velZ   = vz;

        g_tracers[idx].spawnTick = GetTickCount64();

        g_tracerHead = (idx + 1) % kMaxTracers;

    }

}



// In-process float write that CANNOT FAULT, even on stale / freed

// weapon pointers. Uses WriteProcessMemory on self-handle: if the page

// has been unmapped (weapon holstered, GC moved the object, player went

// behind cover and the game cleared state) WPM just returns false

// instead of raising SEGV.

//

// This MUST stay non-faulting because we run under a manual-map APC

// injector: no PDATA is registered for this DLL's code pages, so any

// __try/__except wrapper is a no-op and a page fault instantly crashes

// the game. Previous versions used `*(volatile float*)addr = val` which

// crashed the second the player stepped behind cover mid-burst because

// weapon state transiently swapped out under us.

//

// Overhead is ~150-300ns per call on modern Windows (no cross-process

// marshalling for self-handle WPM). Well under 1ms per shot even for a

// full NoSpread sweep.

static inline void DirectWriteFloat(uintptr_t addr, float val) {

    WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr,

                       &val, sizeof(val), nullptr);

}



// BaseProjectile klass pointer, cached by InitIL2CPPOffsets after

// il2cpp.class_from_name resolves it. Used by ApplyWeaponMods to match

// entity klasses via pointer comparison (walking parents) -- much faster

// than re-reading class name strings for every entity each frame.

uintptr_t g_bpKlass = 0;



typedef void* (*fn_get_transform_t)(void* self, void* method);

typedef void  (*fn_get_pos_injected_t)(void* self, float* ret, void* method);

typedef void  (*fn_set_pos_injected_t)(void* self, float* val, void* method);

static fn_get_transform_t     g_fnGetTransform     = nullptr;

static fn_get_pos_injected_t  g_fnGetPosInjected   = nullptr;

static fn_set_pos_injected_t  g_fnSetPosInjected   = nullptr;

static bool g_transformMethodsResolved = false;



static bool g_manipActive = false;

static float g_manipPeekEyePos[3] = {};





static void ResolveTransformMethods() {

    if (g_transformMethodsResolved) return;

    g_transformMethodsResolved = true;



    if (il2cpp.resolve_icall) {

        g_fnGetTransform   = (fn_get_transform_t)il2cpp.resolve_icall(

            "UnityEngine.Component::get_transform()");

        g_fnGetPosInjected = (fn_get_pos_injected_t)il2cpp.resolve_icall(

            "UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)");

        g_fnSetPosInjected = (fn_set_pos_injected_t)il2cpp.resolve_icall(

            "UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)");

    }



    AimLog("[RH][manip] icall resolve: getTr=%p getPos=%p setPos=%p",

           (void*)g_fnGetTransform, (void*)g_fnGetPosInjected, (void*)g_fnSetPosInjected);



    if (!g_fnGetTransform || !g_fnGetPosInjected || !g_fnSetPosInjected) {

        Il2CppImage* coreImg = il2cpp.FindImage("UnityEngine.CoreModule.dll");

        if (coreImg) {

            if (!g_fnGetTransform) {

                Il2CppClass* compCls = il2cpp.class_from_name(coreImg, "UnityEngine", "Component");

                if (compCls) {

                    void* m = il2cpp.class_get_method_from_name(compCls, "get_transform", 0);

                    if (m) g_fnGetTransform = (fn_get_transform_t)(*(void**)m);

                }

            }

            Il2CppClass* trCls = il2cpp.class_from_name(coreImg, "UnityEngine", "Transform");

            if (trCls) {

                if (!g_fnGetPosInjected) {

                    void* m = il2cpp.class_get_method_from_name(trCls, "get_position_Injected", 1);

                    if (!m) m = il2cpp.class_get_method_from_name(trCls, "get_position", 0);

                    if (m) g_fnGetPosInjected = (fn_get_pos_injected_t)(*(void**)m);

                }

                if (!g_fnSetPosInjected) {

                    void* m = il2cpp.class_get_method_from_name(trCls, "set_position_Injected", 1);

                    if (!m) m = il2cpp.class_get_method_from_name(trCls, "set_position", 1);

                    if (m) g_fnSetPosInjected = (fn_set_pos_injected_t)(*(void**)m);

                }

            }

        }

        AimLog("[RH][manip] fallback resolve: getTr=%p getPos=%p setPos=%p",

               (void*)g_fnGetTransform, (void*)g_fnGetPosInjected, (void*)g_fnSetPosInjected);

    }

}



static bool SafeWrite(uintptr_t addr, const void* data, size_t size) {

    SIZE_T written = 0;

    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, data, size, &written) && written == size;

}



static bool SafeRead(uintptr_t addr, void* out, size_t size) {

    SIZE_T bytesRead = 0;

    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, size, &bytesRead) && bytesRead == size;

}



static bool SafeReadPtr(uintptr_t addr, uintptr_t* out) {

    return SafeRead(addr, out, sizeof(uintptr_t));

}



// Fast canonical-range check: pure arithmetic, no syscall.

// Filters NULL/small, kernel space, and encrypted hi-bit values (0xAB0A...).

// Use this in hot loops; SafeRead's internal RPM handles uncommitted pages.

static inline bool IsCanonicalUserPtr(uintptr_t p) {

    return p >= 0x10000 && p < 0x0000800000000000ULL;

}



// Strict validation: canonical range + VirtualQuery confirms page is committed

// and readable. Costs a syscall (~1-5us). Use SPARINGLY -- only once per scan

// entry, not in hot inner loops. Otherwise expect 2-3 FPS.

static bool IsUserReadablePtr(uintptr_t p) {

    if (!IsCanonicalUserPtr(p)) return false;

    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery((void*)p, &mbi, sizeof(mbi))) return false;

    if (mbi.State != MEM_COMMIT) return false;

    constexpr DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |

                               PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;

    return (mbi.Protect & readMask) != 0;

}



static void SafeWriteFloat(uintptr_t addr, float val) {

    SafeWrite(addr, &val, sizeof(float));

}



namespace Aim {



    static Il2CppMethodInfo* g_linecastMethod   = nullptr;

    static bool              g_linecastResolved = false;

    static bool              g_linecastTried    = false;

    static const int32_t     kVisibilityMask    = 0x08A18101;



    static void ResolveLinecastMethod() {

        if (g_linecastTried) return;

        g_linecastTried = true;



        if (!il2cpp.class_from_name || !il2cpp.class_get_method_from_name ||

            !il2cpp.runtime_invoke) {

            AimLog("[RH][aim][vis] il2cpp API not initialised yet");

            return;

        }



        const char* candidates[] = {

            "UnityEngine.PhysicsModule.dll",

            "UnityEngine.dll",

            "UnityEngine.Physics.dll",

        };

        Il2CppClass* phys = nullptr;

        for (const char* img : candidates) {

            Il2CppImage* image = il2cpp.FindImage(img);

            if (!image) continue;

            phys = il2cpp.class_from_name(image, "UnityEngine", "Physics");

            if (phys) {

                AimLog("[RH][aim][vis] Physics klass via %s -> %p", img, phys);

                break;

            }

        }

        if (!phys && il2cpp.domain_get && il2cpp.domain_get_assemblies &&

            il2cpp.assembly_get_image) {

            Il2CppDomain* dom = il2cpp.domain_get();

            if (dom) {

                size_t count = 0;

                Il2CppAssembly** arr = il2cpp.domain_get_assemblies(dom, &count);

                for (size_t i = 0; i < count && !phys; i++) {

                    Il2CppImage* image = il2cpp.assembly_get_image(arr[i]);

                    if (image) phys = il2cpp.class_from_name(image, "UnityEngine", "Physics");

                }

                if (phys) AimLog("[RH][aim][vis] Physics klass via domain scan -> %p", phys);

            }

        }

        if (!phys) {

            AimLog("[RH][aim][vis] UnityEngine.Physics not found");

            return;

        }



        Il2CppMethodInfo* m = il2cpp.class_get_method_from_name(phys, "Linecast", 3);

        if (!m) {

            AimLog("[RH][aim][vis] Physics.Linecast(argc=3) not found");

            return;

        }

        g_linecastMethod   = m;

        g_linecastResolved = true;

        AimLog("[RH][aim][vis] Linecast resolved method=%p mask=0x%X",

               m, (unsigned)kVisibilityMask);

    }



    bool IsLineVisible(float sx, float sy, float sz,

                       float tx, float ty, float tz) {

        if (!g_linecastTried) ResolveLinecastMethod();

        if (!g_linecastResolved || !g_linecastMethod || !il2cpp.runtime_invoke)

            return true;

        if (!(sx == sx) || !(sy == sy) || !(sz == sz)) return true;

        if (!(tx == tx) || !(ty == ty) || !(tz == tz)) return true;



        struct V3 { float x, y, z; };

        V3      start = { sx, sy, sz };

        V3      end   = { tx, ty, tz };

        int32_t mask  = kVisibilityMask;

        void*   args[3] = { &start, &end, &mask };

        void*   exc   = nullptr;

        Il2CppObject* boxed = il2cpp.runtime_invoke(g_linecastMethod, nullptr, args, &exc);

        if (exc || !boxed) return true;

        uint8_t hit = *(const uint8_t*)((uintptr_t)boxed + 0x10);

        return hit == 0;

    }



    // Resolve a field offset by trying multiple candidate names. Logs result.

    static void ResolveField(Il2CppClass* klass, const char* klassName, int* out,

                             const char* const* names, int nameCount, const char* what) {

        if (!klass) {

            AimLog("[RH][aim] %s: class %s is NULL, keeping default 0x%X", what, klassName, *out);

            return;

        }

        for (int i = 0; i < nameCount; i++) {

            Il2CppFieldInfo* f = il2cpp.class_get_field_from_name(klass, names[i]);

            if (f) {

                int off = il2cpp.field_get_offset(f);

                *out = off;

                AimLog("[RH][aim] %s: %s.%s -> 0x%X (name match #%d)", what, klassName, names[i], off, i);

                return;

            }

        }

        AimLog("[RH][aim] %s: no name matched on %s, keeping default 0x%X (tried: %s%s%s%s)",

               what, klassName, *out,

               nameCount > 0 ? names[0] : "",

               nameCount > 1 ? "," : "", nameCount > 1 ? names[1] : "",

               nameCount > 2 ? ",..." : "");

    }



    // Dump every field on a class (name + offset). Used to discover the

    // actual field names in obfuscated/renamed game builds.

    static void DumpFields(Il2CppClass* klass, const char* klassName, int maxFields = 80) {

        if (!klass) {

            AimLog("[RH][aim] DumpFields(%s): class NULL", klassName);

            return;

        }

        if (!il2cpp.class_get_fields || !il2cpp.field_get_name || !il2cpp.field_get_offset) {

            AimLog("[RH][aim] DumpFields(%s): iter API unavailable", klassName);

            return;

        }

        AimLog("[RH][aim] --- Fields of %s ---", klassName);

        void* iter = nullptr;

        Il2CppFieldInfo* f;

        int n = 0;

        while ((f = il2cpp.class_get_fields(klass, &iter)) != nullptr && n < maxFields) {

            const char* fname = il2cpp.field_get_name(f);

            int off = il2cpp.field_get_offset(f);

            AimLog("[RH][aim]   %s.%s @ 0x%X", klassName, fname ? fname : "?", off);

            n++;

        }

        AimLog("[RH][aim] --- %d fields printed for %s ---", n, klassName);

    }



    // Search BasePlayer fields for the PlayerEyes component by offset range

    // and type pattern. Returns the offset, or 0 if not found.

    //

    // Strategy: in recent Rust builds the BasePlayer field named `eyes`

    // sometimes gets renamed by the Unity IL2CPP metadata processor. We

    // try a wide list of candidate names first, then fall back to a

    // heuristic guess based on offset ranges known from the game version.

    static int FindEyesOffsetByName(Il2CppClass* basePlayerClass) {

        if (!basePlayerClass) return 0;

        const char* candidates[] = {

            "eyes", "playerEyes", "_eyes", "m_eyes", "Eyes", "eyeComponent",

            "eye", "headEyes", "viewEyes", "playerEye", "eye_component",

        };

        for (const char* name : candidates) {

            Il2CppFieldInfo* f = il2cpp.class_get_field_from_name(basePlayerClass, name);

            if (f) {

                int off = il2cpp.field_get_offset(f);

                AimLog("[RH][aim] BasePlayer.eyes: matched candidate '%s' -> 0x%X", name, off);

                return off;

            }

        }

        return 0;

    }



    static int FindRotationLookOffsetByName(Il2CppClass* peClass) {

        if (!peClass) return 0;

        const char* candidates[] = {

            "rotationLook", "viewRotation", "bodyRotation", "rotation",

            "aimRotation", "_rotation", "m_rotation", "lookRotation",

            "eulerLook", "anglesLook", "EulerAngles",

        };

        for (const char* name : candidates) {

            Il2CppFieldInfo* f = il2cpp.class_get_field_from_name(peClass, name);

            if (f) {

                int off = il2cpp.field_get_offset(f);

                AimLog("[RH][aim] PlayerEyes.rotationLook: matched candidate '%s' -> 0x%X", name, off);

                return off;

            }

        }

        return 0;

    }



    // ================================================================

    // ENCRYPTED EYE POINTER — ADAPTIVE RESOLUTION

    // ================================================================

    // BasePlayer.player_eyes may be stored RAW or encrypted with one of

    // several IL2CPP field decryptors depending on the Rust build. We

    // support three modes and auto-detect which one works on this build:

    //

    //   mode 0: RAW    — pointer stored directly

    //   mode 1: EL     — decrypt_el pattern (v<<13, +0x151da616, a<<16)

    //   mode 2: CE     — decrypt_ce pattern (v<<18, +0x97acc028, a<<12, +0x12c55e8b)

    //

    // Field offset is also unreliable — `eyes` is obfuscated on recent

    // builds so class_get_field_from_name() returns NULL. We FALLBACK to

    // scanning BasePlayer[0x100..0x400] in 8-byte steps, trying all three

    // decrypt modes at each slot, looking for a pointer whose first qword

    // matches g_peClass. The first hit is cached for subsequent calls.



    // decrypt_el — same as ESP's entityList decryptor.

    // Algorithm extracted from sub_180E933D0 (RVA 0xE933D0) of the May 2026 build.

    // The same decoder is reused for BasePlayer.player_eyes (handle_shl_add_xor family).

    static uint64_t DecryptEL(uint64_t val) {

        auto half = [](uint32_t v) -> uint32_t {

            uint32_t a = v ^ 0x7FB09BA3u;

            a = (a << 25) | (a >> 7);

            a ^= 0x92589FA8u;

            a = (a << 19) | (a >> 13);

            return a;

        };

        return ((uint64_t)half((uint32_t)(val >> 32)) << 32) | half((uint32_t)val);

    }



    // Local resolver for il2cpp_gchandle_get_target. The decrypted

    // player_eyes value is often a GC handle (small integer), not a direct

    // pointer — GC-handle-resolve maps it to the real object pointer.

    typedef void* (*fn_il2cpp_gchandle_get_target)(uint32_t handle);

    static fn_il2cpp_gchandle_get_target g_gchandle_get_target = nullptr;

    static uintptr_t GcHandleResolve(uintptr_t handle) {

        if (handle == 0) return 0;

        if (!g_gchandle_get_target) {

            HMODULE ga = GetModuleHandleA("GameAssembly.dll");

            if (!ga) return 0;

            g_gchandle_get_target = (fn_il2cpp_gchandle_get_target)

                GetProcAddress(ga, "il2cpp_gchandle_get_target");

            if (!g_gchandle_get_target) return 0;

        }

        return (uintptr_t)g_gchandle_get_target((uint32_t)handle);

    }



    // decrypt_ce — same as ESP's clientEntities decryptor.

    // Algorithm extracted from sub_180E1B8C0 (RVA 0xE1B8C0) of the May 2026 build.

    static uint64_t DecryptCE(uint64_t val) {

        auto half = [](uint32_t v) -> uint32_t {

            uint32_t a = v + 0x2FAF9B7Bu;

            a = (a << 9) | (a >> 23);

            a -= 0x307C0EA5u;

            a ^= 0x50BB0B84u;

            return a;

        };

        return ((uint64_t)half((uint32_t)(val >> 32)) << 32) | half((uint32_t)val);

    }



    // Cached result of the adaptive scan — set on first success, used for

    // all subsequent calls so we don't re-scan every shot.

    int      g_eyesResolvedOffset = 0;   // 0 means "not resolved yet"

    int      g_eyesResolvedMode   = -1;  // 0=RAW, 1=EL, 2=CE

    int      g_quatOffset         = 0;   // resolved Quaternion offset within PlayerEyes



    // Direct PlayerEyes resolver for build 22830770+.

    // Reads BasePlayer+0x2E0, tries EL → GC-handle resolve, then CE → GC,

    // then RAW, returning the first pointer whose first qword is a canonical

    // klass vptr. Skips the fingerprint / parent-chain checks that fail on

    // obfuscated builds — the offset 0x2E0 is authoritative per the external

    // memory dumper for this build range.

    //

    // The cached mode is stored in g_eyesDirectMode so subsequent calls skip

    // the trial-and-error phase.

    static int g_eyesDirectMode = -1; // -1=untried, 0=RAW, 1=EL+GC, 2=CE+GC, 3=EL raw, 4=CE raw, 5=ENTITYREF (May2026)

    static inline uint32_t EntityRefDecryptDword(uint32_t v) {

        uint32_t a = v + 0x2B0024DCu;

        a = (a << 12) | (a >> 20);

        a ^= 0x1B941E57u;

        a -= 0x4DC4DEE4u;

        return a;

    }

    static uintptr_t ResolveEyesFromEntityRef(uintptr_t entityRef) {

        static int s_diagCount = 0;

        bool diag = (s_diagCount < 5);

        if (!IsCanonicalUserPtr(entityRef)) {

            if (diag) { ++s_diagCount; AimLog("[RH][aim] ER diag: entityRef not canon %p", (void*)entityRef); }

            return 0;

        }

        uint8_t validFlag = 0;

        bool readFlag = SafeRead(entityRef + 0x10, &validFlag, 1);

        uint64_t encrypted = 0;

        bool readEnc = SafeRead(entityRef + 0x18, &encrypted, 8);

        uint32_t lo = EntityRefDecryptDword((uint32_t)encrypted);

        uint32_t hi = EntityRefDecryptDword((uint32_t)(encrypted >> 32));

        uint64_t handle = ((uint64_t)hi << 32) | lo;

        uintptr_t obj = (handle && readEnc) ? GcHandleResolve((uintptr_t)handle) : 0;

        uintptr_t klass = 0;

        bool readKlass = (obj && IsCanonicalUserPtr(obj)) ? SafeReadPtr(obj, &klass) : false;

        if (diag) {

            ++s_diagCount;

            AimLog("[RH][aim] ER diag: ref=%p flagOK=%d flag=%u encOK=%d enc=0x%llX "

                   "handle=0x%llX obj=%p klassOK=%d klass=%p",

                   (void*)entityRef, (int)readFlag, (unsigned)validFlag,

                   (int)readEnc, (unsigned long long)encrypted,

                   (unsigned long long)handle, (void*)obj,

                   (int)readKlass, (void*)klass);

        }

        if (!readEnc || handle == 0) return 0;

        if (!IsCanonicalUserPtr(obj)) return 0;

        if (!readKlass || !IsCanonicalUserPtr(klass)) return 0;

        return obj;

    }

    typedef uintptr_t (*fn_basePlayer_get_eyes_t)(uintptr_t self);

    static fn_basePlayer_get_eyes_t g_fnBpGetEyes = nullptr;

    static bool g_fnBpGetEyesTried = false;

    static fn_basePlayer_get_eyes_t GetBpEyesGetter() {

        if (g_fnBpGetEyesTried) return g_fnBpGetEyes;

        g_fnBpGetEyesTried = true;

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!ga) return nullptr;

        const uintptr_t kEyesGetterRva = 0xCB1A80;

        uintptr_t addr = (uintptr_t)ga + kEyesGetterRva;

        uint8_t bytes[4] = {};

        if (!SafeRead(addr, bytes, 4)) {

            AimLog("[RH][aim] BpEyesGetter: cannot read prologue at %p", (void*)addr);

            return nullptr;

        }

        AimLog("[RH][aim] BpEyesGetter @ %p prologue=%02X %02X %02X %02X",

               (void*)addr, bytes[0], bytes[1], bytes[2], bytes[3]);

        g_fnBpGetEyes = (fn_basePlayer_get_eyes_t)addr;

        return g_fnBpGetEyes;

    }

    static uintptr_t ResolveEyesDirect(uintptr_t player) {

        if (player == 0) return 0;

        if (auto fn = GetBpEyesGetter()) {

            uintptr_t pe = 0;

            __try {

                pe = fn(player);

            } __except (EXCEPTION_EXECUTE_HANDLER) {

                pe = 0;

            }

            if (IsCanonicalUserPtr(pe)) {

                uintptr_t klass = 0;

                if (SafeReadPtr(pe, &klass) && IsCanonicalUserPtr(klass)) {

                    if (g_eyesDirectMode != 6) {

                        g_eyesDirectMode = 6;

                        const char* klassName = "?";

                        if (il2cpp.class_get_name) {

                            __try {

                                klassName = il2cpp.class_get_name((Il2CppClass*)klass);

                                if (!klassName) klassName = "?";

                            } __except (EXCEPTION_EXECUTE_HANDLER) { klassName = "?!"; }

                        }

                        AimLog("[RH][aim] EYES via getter -> pe=%p klass=%p name='%s'",

                               (void*)pe, (void*)klass, klassName);

                    }

                    return pe;

                }

            }

        }

        uintptr_t entityRef = 0;

        if (SafeReadPtr(player + 0x348, &entityRef) && IsCanonicalUserPtr(entityRef)) {

            uintptr_t pe = ResolveEyesFromEntityRef(entityRef);

            if (pe) {

                if (g_eyesDirectMode != 5) {

                    g_eyesDirectMode = 5;

                    static bool logged = false;

                    if (!logged) {

                        logged = true;

                        uintptr_t klass = 0;

                        SafeReadPtr(pe, &klass);

                        const char* klassName = "?";

                        if (il2cpp.class_get_name && IsCanonicalUserPtr(klass)) {

                            __try {

                                klassName = il2cpp.class_get_name((Il2CppClass*)klass);

                                if (!klassName) klassName = "?";

                            } __except (EXCEPTION_EXECUTE_HANDLER) { klassName = "?!"; }

                        }

                        AimLog("[RH][aim] EYES via EntityRef@+0x348 -> ref=%p pe=%p klass='%s'",

                               (void*)entityRef, (void*)pe, klassName);

                    }

                }

                return pe;

            }

        }

        uint64_t raw = 0;

        if (!SafeRead(player + 0x2E0, &raw, 8)) return 0;

        if (raw == 0) return 0;



        auto tryPtr = [](uintptr_t p) -> bool {

            if (!IsCanonicalUserPtr(p)) return false;

            uintptr_t klass = 0;

            if (!SafeReadPtr(p, &klass)) return false;

            return IsCanonicalUserPtr(klass);

        };



        // Fast path: known mode cached.

        if (g_eyesDirectMode >= 0) {

            uintptr_t p = 0;

            switch (g_eyesDirectMode) {

                case 0: p = (uintptr_t)raw; break;

                case 1: p = GcHandleResolve((uintptr_t)DecryptEL(raw)); break;

                case 2: p = GcHandleResolve((uintptr_t)DecryptCE(raw)); break;

                case 3: p = (uintptr_t)DecryptEL(raw); break;

                case 4: p = (uintptr_t)DecryptCE(raw); break;

            }

            if (tryPtr(p)) return p;

            g_eyesDirectMode = -1; // reset — player respawn / pointer moved

        }



        // Trial: try each mode in priority order.

        struct Try { int mode; uintptr_t p; };

        Try candidates[5] = {

            { 1, GcHandleResolve((uintptr_t)DecryptEL(raw)) },

            { 2, GcHandleResolve((uintptr_t)DecryptCE(raw)) },

            { 0, (uintptr_t)raw },

            { 3, (uintptr_t)DecryptEL(raw) },

            { 4, (uintptr_t)DecryptCE(raw) },

        };

        for (auto& t : candidates) {

            if (tryPtr(t.p)) {

                g_eyesDirectMode = t.mode;

                static bool logged = false;

                if (!logged) {

                    logged = true;

                    uintptr_t klass = 0;

                    SafeReadPtr(t.p, &klass);

                    const char* klassName = "?";

                    if (il2cpp.class_get_name && IsCanonicalUserPtr(klass)) {

                        __try {

                            klassName = il2cpp.class_get_name((Il2CppClass*)klass);

                            if (!klassName) klassName = "?";

                        } __except (EXCEPTION_EXECUTE_HANDLER) { klassName = "?!"; }

                    }

                    const char* modeStr[5] = { "RAW", "EL+GC", "CE+GC", "EL", "CE" };

                    AimLog("[RH][aim] EYES DIRECT resolved: BasePlayer+0x2E0 mode=%s "

                           "-> %p klass=%p name='%s'",

                           modeStr[t.mode], (void*)t.p, (void*)klass, klassName);

                }

                return t.p;

            }

        }

        return 0;

    }



    // Check if 16 bytes at `p` look like a NON-TRIVIAL Unity Quaternion

    // (4 floats with unit magnitude, representing a real rotation).

    // Strict version used during broad scanning:

    //   - All components finite and in [-1.01, 1.01]

    //   - Magnitude squared in [0.97, 1.03] (tight unit-length tolerance)

    //   - NOT axis-aligned (rejects (0,0,0,1), (1,0,0,0) etc — those are

    //     trivially matched by any mostly-zero memory chunk)

    // A real bodyRotation from a player actively moving will easily pass

    // these checks.

    static bool LooksLikeQuaternion(uintptr_t p) {

        float q[4];

        if (!SafeRead(p, q, 16)) return false;

        for (int i = 0; i < 4; i++) {

            uint32_t bits; memcpy(&bits, &q[i], 4);

            uint32_t exp = (bits >> 23) & 0xFF;

            if (exp == 0xFF) return false;              // NaN/Inf

            if (q[i] < -1.01f || q[i] > 1.01f) return false;

        }

        float mag2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];

        if (mag2 < 0.97f || mag2 > 1.03f) return false; // must be near unit



        // Count components with |v| > 0.05 — a real rotation has at LEAST

        // two such components (e.g. yaw rotation has w != 0 and y != 0).

        int nonTrivial = 0;

        for (int i = 0; i < 4; i++) if (q[i] > 0.05f || q[i] < -0.05f) nonTrivial++;

        if (nonTrivial < 2) return false; // reject axis-aligned (identity etc)

        return true;

    }



    // Loose Quaternion check (for the TWO-QUAT signature below). Only requires

    // finite values in [-1.01, 1.01] and magnitude ~= 1. Allows identity

    // rotations (rest state) since the two-quat signature itself is specific

    // enough to uniquely identify PlayerEyes.

    static bool LooksLikeQuaternionLoose(uintptr_t p) {

        float q[4];

        if (!SafeRead(p, q, 16)) return false;

        for (int i = 0; i < 4; i++) {

            uint32_t bits; memcpy(&bits, &q[i], 4);

            uint32_t exp = (bits >> 23) & 0xFF;

            if (exp == 0xFF) return false;

            if (q[i] < -1.01f || q[i] > 1.01f) return false;

        }

        float mag2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];

        return mag2 > 0.90f && mag2 < 1.10f;

    }



    // PlayerEyes signature (confirmed by game dump):

    //   +0x40 viewOffset     (Vector3, 12 bytes)

    //   +0x50 bodyRotation   (Quaternion, 16 bytes)

    //   +0x6C unkQuaternion  (Quaternion, 16 bytes — likely headRotation)

    //

    // Require TWO unit-length Quaternions at the exact dumped offsets. This

    // is a unique signature — random objects in BasePlayer memory don't have

    // two Quaternions spaced at exactly 0x50 and 0x6C.

    static int FindQuaternionOffset(uintptr_t candidate) {

        bool q1 = LooksLikeQuaternionLoose(candidate + 0x50);

        bool q2 = LooksLikeQuaternionLoose(candidate + 0x6C);

        if (q1 && q2) return 0x50;  // return bodyRotation offset

        return 0;

    }



    // Check if a float looks like a "plausible" non-pointer non-NaN value

    // in a small magnitude range suitable for Unity Vector3 components.

    static bool IsPlausibleFloat(float f) {

        uint32_t bits; memcpy(&bits, &f, 4);

        uint32_t exp = (bits >> 23) & 0xFF;

        if (exp == 0xFF) return false;       // NaN/Inf

        if (exp == 0)    return (bits == 0); // zero OK; denormals rejected

        if (exp > 0x90)  return false;       // > ~65k — pointer-ish

        return (f > -1000.0f && f < 1000.0f);

    }



    // Fingerprint PlayerEyes instances using the field layout dumped at init:

    //   +0x28 Vector3 thirdPersonSleepingOffset   (e.g. (0, 0.28, 0.5))

    //   +0x38 Vector3 defaultLazyAim              (e.g. (0, 0, 1))

    //   +0x50 Quaternion (unit)                   (bodyRotation)

    //   +0x6C Quaternion (unit)                   (headRotation / other)

    //   +0x7C Quaternion (unit)                   (extra rotation)

    //

    // Require ALL of: both Vector3 fields plausible + at least 1 non-zero

    // component + at least ONE valid unit quaternion at the known quat

    // offsets. This combined structural signature is specific enough that

    // random objects (BasePlayer's ~30+ inner references) don't match.

    //

    // Returns the offset of the first valid Quaternion so the caller can

    // immediately set rotationLookOffset; returns -1 if not a PlayerEyes.

    static int FingerprintPlayerEyes(uintptr_t candidate) {

        float sleep[3] = {0}, lazy[3] = {0};

        if (!SafeRead(candidate + 0x28, sleep, 12)) return -1;

        if (!SafeRead(candidate + 0x38, lazy,  12)) return -1;

        int nonZero = 0;

        for (int i = 0; i < 3; i++) {

            if (!IsPlausibleFloat(sleep[i])) return -1;

            if (!IsPlausibleFloat(lazy[i]))  return -1;

            if (sleep[i] != 0.0f) nonZero++;

            if (lazy[i]  != 0.0f) nonZero++;

        }

        if (nonZero < 1) return -1;

        // Structural confirmation: PlayerEyes must have at least one valid

        // unit quaternion at the known rotation offsets.

        if (LooksLikeQuaternionLoose(candidate + 0x50)) return 0x50;

        if (LooksLikeQuaternionLoose(candidate + 0x6C)) return 0x6C;

        if (LooksLikeQuaternionLoose(candidate + 0x7C)) return 0x7C;

        return -1;

    }



    // Walk the klass parent chain to determine if `klass` IS or INHERITS

    // from the cached PlayerEyes class. Obfuscated builds present a runtime

    // instance whose klass is a subclass with a scrambled name (e.g.

    // "%d7bed0a63d...") — strict equality to g_peClass misses these.

    // Walking the parent chain via il2cpp_class_get_parent catches them.

    static bool IsPlayerEyesKlass(uintptr_t klass) {

        if (g_peClass == nullptr || !il2cpp.class_get_parent) return false;

        if (!IsCanonicalUserPtr(klass)) return false;

        Il2CppClass* target = g_peClass;

        Il2CppClass* cur = (Il2CppClass*)klass;

        for (int i = 0; i < 8 && cur != nullptr; i++) {

            if (cur == target) return true;

            cur = il2cpp.class_get_parent(cur);

            if (!cur || !IsCanonicalUserPtr((uintptr_t)cur)) break;

        }

        return false;

    }



    // Try one slot on one player with a specific decrypt mode. Returns the

    // resolved PlayerEyes pointer on success, 0 on failure. On success,

    // also updates g_quatOffset to the offset where the Quaternion was found.

    static uintptr_t TrySlot(uintptr_t localPlayer, int off, int mode) {

        uint64_t raw = 0;

        if (!SafeRead(localPlayer + off, &raw, 8)) return 0;

        uintptr_t candidate = 0;

        switch (mode) {

            case 0: candidate = (uintptr_t)raw; break;

            case 1: candidate = (uintptr_t)DecryptEL(raw); break;

            case 2: candidate = (uintptr_t)DecryptCE(raw); break;

            default: return 0;

        }

        // After decryption, the value may be a small GC handle (not a raw

        // pointer). Confirmed for build 22830770: BasePlayer.player_eyes

        // uses the same shl+add+xor decrypt as entity_list AND returns a

        // GC handle. Resolve it via il2cpp_gchandle_get_target.

        if (mode != 0 && candidate < 0x10000000 && candidate != 0) {

            uintptr_t resolved = GcHandleResolve(candidate);

            if (resolved >= 0x1000) {

                candidate = resolved;

            }

        }

        if (!IsCanonicalUserPtr(candidate)) return 0;

        // Require the first qword to be a canonical pointer (klass vptr).

        // Most Unity/il2cpp objects have this; pure data structures don't.

        uintptr_t klass = 0;

        if (!SafeReadPtr(candidate, &klass)) return 0;

        if (!IsCanonicalUserPtr(klass)) return 0;



        // PRIMARY signal: klass IS or INHERITS from cached PlayerEyes klass.

        // Parent-chain walk catches obfuscated subclasses where the immediate

        // klass pointer doesn't match g_peClass but the class hierarchy

        // eventually reaches it. On a match, use the default bodyRotation

        // offset 0x50 from the field dump; shotgun write handles the rest.

        if (IsPlayerEyesKlass(klass)) {

            g_quatOffset = 0x50;

            return candidate;

        }



        // SECONDARY signal: combined PlayerEyes data + structural fingerprint.

        // Requires plausible Vector3 data at +0x28/+0x38 AND at least one

        // unit Quaternion at a known rotation offset (+0x50/+0x6C/+0x7C).

        // This combined signature is specific enough to avoid false matches

        // on generic objects (BaseEntity, PlayerInventory, containers, etc.).

        int qoff = FingerprintPlayerEyes(candidate);

        if (qoff > 0) {

            g_quatOffset = qoff;

            return candidate;

        }



        return 0;

    }



    // Negative-result caching: after a failed scan, don't re-scan until N

    // subsequent calls pass (amortizes cost during shooting when eyes may

    // never be resolvable on this build). Reset on cache miss to allow

    // re-resolution after player respawn.

    int g_eyesScanCooldown = 0;



    // Read the PlayerEyes instance pointer. Fast path uses the cached

    // offset/mode; slow path scans BasePlayer memory for the right combo.

    static uintptr_t ResolveEyesPtr(uintptr_t localPlayer) {

        if (localPlayer == 0) return 0;



        // Fast path: we already know the right offset and decrypt mode.

        if (g_eyesResolvedOffset != 0 && g_eyesResolvedMode >= 0) {

            uintptr_t p = TrySlot(localPlayer, g_eyesResolvedOffset, g_eyesResolvedMode);

            if (p) return p;

            // Cache miss (player respawn / eyes ptr moved) — reset cache

            // so a re-scan runs below.

            g_eyesResolvedOffset = 0;

            g_eyesResolvedMode   = -1;

            g_eyesScanCooldown   = 0;

        }



        // Negative-result cooldown: cheap early-exit when we've recently

        // scanned and found nothing. Avoids hammering CPU + log file on

        // obfuscated builds where PlayerEyes isn't reachable via BasePlayer

        // fields. Cooldown = ~120 calls ≈ 2 seconds at 60fps.

        if (g_eyesScanCooldown > 0) {

            g_eyesScanCooldown--;

            return 0;

        }



        // Slow path: scan BasePlayer[0x100..0x1000] in 8-byte steps, trying

        // all 3 decrypt modes at each slot. Extended range covers obfuscated

        // builds where eyes is relocated past the original +0x2E0 slot.

        for (int off = 0x100; off < 0x1000; off += 8) {

            for (int mode = 0; mode < 3; mode++) {

                uintptr_t p = TrySlot(localPlayer, off, mode);

                if (p) {

                    const char* modeName = (mode == 0) ? "RAW"

                                         : (mode == 1) ? "EL"

                                         : "CE";

                    // Read klass + name so we can verify this is really

                    // PlayerEyes (vs false positive on an unrelated class).

                    uintptr_t klass = 0;

                    SafeReadPtr(p, &klass);

                    const char* klassName = "?";

                    if (il2cpp.class_get_name && IsCanonicalUserPtr(klass)) {

                        __try {

                            klassName = il2cpp.class_get_name((Il2CppClass*)klass);

                            if (!klassName) klassName = "?";

                        } __except (EXCEPTION_EXECUTE_HANDLER) {

                            klassName = "?!";

                        }

                    }

                    bool chainMatch = IsPlayerEyesKlass(klass);

                    AimLog("[RH][aim] EYES SCAN HIT: BasePlayer+0x%X mode=%s -> %p "

                           "klass=%p name='%s' chain=%d quat@+0x%X",

                           off, modeName, (void*)p, (void*)klass, klassName,

                           (int)chainMatch, g_quatOffset);

                    g_eyesResolvedOffset = off;

                    g_eyesResolvedMode   = mode;

                    eyesOffset           = off;

                    rotationLookOffset   = g_quatOffset;

                    eyesIsEncrypted      = (mode != 0);

                    return p;

                }

            }

        }



        // Scan failed — suppress further scans for ~2 seconds to stop

        // hammering the CPU / log file. TryProbeEyes handles the one-time

        // diagnostic dump after enough repeated failures.

        g_eyesScanCooldown = 120;

        return 0;

    }



    // Called from Update(). Keeps scanning until eyes is found. On first

    // success logs the resolved offset + mode; otherwise keeps retrying

    // at a cheap rate (eyes may spawn late after server join).

    static void TryProbeEyes(uintptr_t localPlayer) {

        if (g_eyeProbeDone) return;

        if (localPlayer == 0) return;

        g_eyeProbeAttempts++;



        uintptr_t eyes = ResolveEyesPtr(localPlayer);

        if (eyes != 0) {

            const char* modeName = (g_eyesResolvedMode == 0) ? "RAW"

                                 : (g_eyesResolvedMode == 1) ? "EL"

                                 : "CE";

            AimLog("[RH][aim] EYES RESOLVED: BasePlayer+0x%X (%s) -> PlayerEyes@%p "

                   "bodyRotation@+0x%X",

                   g_eyesResolvedOffset, modeName, (void*)eyes, rotationLookOffset);

            g_eyeProbeDone = true;

        } else if (g_eyeProbeAttempts > 300) {

            // Scan failed. Dump a WIDE-RANGE diagnostic of every canonical

            // pointer in BasePlayer[0x100..0x1000], showing klass name,

            // whether it klass-chains to PlayerEyes, whether its field data

            // at +0x28/+0x38 matches the PlayerEyes fingerprint, and any

            // unit-Quaternion offsets found inside.

            AimLog("[RH][aim] EYES RESOLVE FAILED after %d attempts. "

                   "Dumping BasePlayer[0x100..0x1000]:",

                   g_eyeProbeAttempts);

            int dumped = 0;

            for (int off = 0x100; off < 0x1000 && dumped < 80; off += 8) {

                uint64_t raw = 0;

                if (!SafeRead(localPlayer + off, &raw, 8)) continue;

                if (raw == 0) continue;

                uintptr_t variants[3] = {

                    (uintptr_t)raw,

                    (uintptr_t)DecryptEL(raw),

                    (uintptr_t)DecryptCE(raw),

                };

                const char* modeNames[3] = { "RAW", "EL ", "CE " };

                for (int m = 0; m < 3; m++) {

                    uintptr_t p = variants[m];

                    if (!IsCanonicalUserPtr(p)) continue;

                    uintptr_t klass = 0;

                    if (!SafeReadPtr(p, &klass)) continue;

                    if (!IsCanonicalUserPtr(klass)) continue;

                    const char* klassName = "?";

                    if (il2cpp.class_get_name) {

                        __try {

                            klassName = il2cpp.class_get_name((Il2CppClass*)klass);

                            if (!klassName) klassName = "?";

                        } __except (EXCEPTION_EXECUTE_HANDLER) {

                            klassName = "?!";

                        }

                    }

                    bool chain   = IsPlayerEyesKlass(klass);

                    int  peQOff  = FingerprintPlayerEyes(p);

                    bool peData  = (peQOff > 0);

                    // Find all unit-Quaternion offsets inside this candidate

                    char quatLine[160] = {};

                    int  qpos = 0;

                    for (int qo = 0x20; qo < 0x120 && qpos < 120; qo += 4) {

                        if (LooksLikeQuaternionLoose(p + (uintptr_t)qo)) {

                            qpos += snprintf(quatLine + qpos,

                                             sizeof(quatLine) - qpos,

                                             " Q@+0x%X", qo);

                        }

                    }

                    AimLog("[RH][aim]   +0x%X %s %p klass=%p name=%s chain=%d peData=%d%s",

                           off, modeNames[m], (void*)p, (void*)klass, klassName,

                           (int)chain, (int)peData,

                           quatLine[0] ? quatLine : "");

                    dumped++;

                    if (dumped >= 80) break;

                }

            }

            AimLog("[RH][aim] EYES DUMP end (%d entries). peClass=%p", dumped, g_peClass);

            g_eyeProbeDone = true;

        }

    }



    void InitIL2CPPOffsets() {

        Il2CppImage* img = il2cpp.FindImage("Assembly-CSharp.dll");

        if (!img) {

            AimLog("[RH][aim] InitIL2CPPOffsets: Assembly-CSharp.dll not found — defaults used");

            return;

        }



        // InputState.current

        Il2CppClass* inputStateClass = il2cpp.class_from_name(img, "", "InputState");

        {

            const char* n[] = { "current" };

            ResolveField(inputStateClass, "InputState", &currentOffset, n, 1, "InputState.current");

        }



        // InputMessage.aimAngles

        Il2CppClass* inputMessageClass = il2cpp.class_from_name(img, "", "InputMessage");

        {

            const char* n[] = { "aimAngles" };

            ResolveField(inputMessageClass, "InputMessage", &aimAnglesOffset, n, 1, "InputMessage.aimAngles");

        }



        // BasePlayer.serverInput — fallback for hook cases where InputState isn't an arg.

        Il2CppClass* basePlayerClass = il2cpp.class_from_name(img, "", "BasePlayer");

        {

            const char* n[] = { "serverInput", "input", "clientInput" };

            int tmp = 0;

            ResolveField(basePlayerClass, "BasePlayer", &tmp, n, 3, "BasePlayer.serverInput");

            if (tmp != 0) serverInputOffset = tmp;

        }



        // BasePlayer.inventory

        {

            const char* n[] = { "inventory" };

            ResolveField(basePlayerClass, "BasePlayer", &dynInventoryOffset, n, 1, "BasePlayer.inventory");

        }



        // PlayerInventory.containerBelt

        Il2CppClass* piClass = il2cpp.class_from_name(img, "", "PlayerInventory");

        {

            const char* n[] = { "containerBelt", "belt" };

            ResolveField(piClass, "PlayerInventory", &dynContainerBeltOffset, n, 2, "PlayerInventory.containerBelt");

        }



        // ItemContainer.itemList

        Il2CppClass* icClass = il2cpp.class_from_name(img, "", "ItemContainer");

        {

            const char* n[] = { "itemList", "contents", "list" };

            ResolveField(icClass, "ItemContainer", &dynItemListOffset, n, 3, "ItemContainer.itemList");

        }



        // Item.heldEntity

        Il2CppClass* itemClass = il2cpp.class_from_name(img, "", "Item");

        {

            const char* n[] = { "heldEntity" };

            ResolveField(itemClass, "Item", &dynHeldEntityOffset, n, 1, "Item.heldEntity");

        }



        // BaseProjectile.*

        Il2CppClass* bpClass = il2cpp.class_from_name(img, "", "BaseProjectile");

        // Cache klass pointer for O(1) entity filtering (ApplyWeaponMods).

        // Global defined at file scope; :: avoids binding inside namespace Aim.

        ::g_bpKlass = (uintptr_t)bpClass;

        AimLog("[RH][aim] Cached BaseProjectile klass ptr: %p", bpClass);



        {

            const char* n[] = { "recoilProperties", "recoilProp", "recoil" };

            ResolveField(bpClass, "BaseProjectile", &recoilOffset, n, 3, "BaseProjectile.recoil*");

        }

        {

            const char* n[] = { "aimSway" };

            ResolveField(bpClass, "BaseProjectile", &aimSwayOffset, n, 1, "BaseProjectile.aimSway");

        }

        {

            const char* n[] = { "aimSwaySpeed" };

            ResolveField(bpClass, "BaseProjectile", &aimSwaySpeedOffset, n, 1, "BaseProjectile.aimSwaySpeed");

        }

        {

            const char* n[] = { "aimCone" };

            ResolveField(bpClass, "BaseProjectile", &aimConeOffset, n, 1, "BaseProjectile.aimCone");

        }

        {

            const char* n[] = { "hipAimCone" };

            ResolveField(bpClass, "BaseProjectile", &hipAimConeOffset, n, 1, "BaseProjectile.hipAimCone");

        }

        {

            const char* n[] = { "aimConePenaltyMax" };

            ResolveField(bpClass, "BaseProjectile", &aimConePenaltyMaxOffset, n, 1, "BaseProjectile.aimConePenaltyMax");

        }

        {

            // NOTE: in current dump the field is named "aimconePenaltyPerShot" (lowercase c).

            const char* n[] = { "aimconePenaltyPerShot", "aimPenaltyPerShot" };

            ResolveField(bpClass, "BaseProjectile", &aimPenaltyPerShotOffset, n, 2, "BaseProjectile.penaltyPerShot");

        }

        {

            const char* n[] = { "aimconePenalty", "aimConePenalty" };

            ResolveField(bpClass, "BaseProjectile", &aimconePenaltyOffset, n, 2, "BaseProjectile.aimconePenalty");

        }

        {

            const char* n[] = { "stancePenalty" };

            ResolveField(bpClass, "BaseProjectile", &stancePenaltyOffset, n, 1, "BaseProjectile.stancePenalty");

        }



        // RecoilProperties.*

        Il2CppClass* rpClass = il2cpp.class_from_name(img, "", "RecoilProperties");

        {

            const char* n[] = { "recoilYawMin" };

            ResolveField(rpClass, "RecoilProperties", &recoilYawMinOffset, n, 1, "RecoilProperties.recoilYawMin");

        }

        {

            const char* n[] = { "recoilYawMax" };

            ResolveField(rpClass, "RecoilProperties", &recoilYawMaxOffset, n, 1, "RecoilProperties.recoilYawMax");

        }

        {

            const char* n[] = { "recoilPitchMin" };

            ResolveField(rpClass, "RecoilProperties", &recoilPitchMinOffset, n, 1, "RecoilProperties.recoilPitchMin");

        }

        {

            const char* n[] = { "recoilPitchMax" };

            ResolveField(rpClass, "RecoilProperties", &recoilPitchMaxOffset, n, 1, "RecoilProperties.recoilPitchMax");

        }

        {

            const char* n[] = { "newRecoilOverride" };

            ResolveField(rpClass, "RecoilProperties", &newRecoilOverrideOffset, n, 1, "RecoilProperties.newRecoilOverride");

        }



        // PlayerEyes offsets for true silent aim.

        // BasePlayer.eyes -> PlayerEyes; PlayerEyes.bodyRotation -> Quaternion

        // In obfuscated builds these fields are renamed/stripped, so we keep

        // the 2026-04-17 build defaults (eyes=0x2E0 encrypted, rot=0x50) if

        // by-name resolution fails.

        if (int ev = FindEyesOffsetByName(basePlayerClass)) eyesOffset = ev;

        Il2CppClass* peClass = il2cpp.class_from_name(img, "", "PlayerEyes");

        g_peClass = peClass;   // cache for runtime probe (see ProbeEyesOffset)

        if (int rv = FindRotationLookOffsetByName(peClass))  rotationLookOffset = rv;



        // Dump PlayerEyes class layout: parent chain + every field with

        // name+offset. Critical for obfuscated builds where field names

        // are scrambled — we can still see offsets and infer Quaternion

        // fields by their spacing (typically 16-byte aligned, 16 bytes apart).

        if (peClass && il2cpp.class_get_parent) {

            char parentChain[512] = {0};

            int  pcPos = 0;

            Il2CppClass* cur = peClass;

            for (int i = 0; i < 6 && cur; i++) {

                const char* nm = il2cpp.class_get_name ? il2cpp.class_get_name(cur) : "?";

                if (!nm) nm = "?";

                pcPos += snprintf(parentChain + pcPos,

                                  sizeof(parentChain) - pcPos,

                                  "%s%s", (i == 0 ? "" : " -> "), nm);

                if (pcPos >= (int)sizeof(parentChain) - 4) break;

                cur = il2cpp.class_get_parent(cur);

            }

            AimLog("[RH][aim] PlayerEyes class chain: %s", parentChain);

        }

        if (peClass && il2cpp.class_get_fields && il2cpp.field_get_name &&

            il2cpp.field_get_offset) {

            void* iter = nullptr;

            Il2CppFieldInfo* f = nullptr;

            int fieldCount = 0;

            while ((f = il2cpp.class_get_fields(peClass, &iter)) != nullptr) {

                const char* fname = il2cpp.field_get_name(f);

                int foff = il2cpp.field_get_offset(f);

                AimLog("[RH][aim] PlayerEyes.field[%d]: name='%s' offset=0x%X",

                       fieldCount, fname ? fname : "?", foff);

                fieldCount++;

                if (fieldCount >= 64) break;  // cap log spam

            }

            AimLog("[RH][aim] PlayerEyes: %d fields enumerated", fieldCount);

        }



        AimLog("[RH][aim] Final offsets: recoil=0x%X aimSway=0x%X aimCone=0x%X hipAimCone=0x%X "

               "aimConePenaltyMax=0x%X aimPenaltyPerShot=0x%X aimconePenalty=0x%X stancePenalty=0x%X "

               "serverInput=0x%X current=0x%X aimAngles=0x%X eyes=0x%X rotationLook=0x%X",

               recoilOffset, aimSwayOffset, aimConeOffset, hipAimConeOffset,

               aimConePenaltyMaxOffset, aimPenaltyPerShotOffset,

               aimconePenaltyOffset, stancePenaltyOffset,

               serverInputOffset, currentOffset, aimAnglesOffset,

               eyesOffset, rotationLookOffset);



        ResolveTransformMethods();

    }



    static bool IsLocalParented() {

        if (localPlayer == 0) return false;

        uintptr_t msPtr = 0;

        if (!SafeReadPtr(localPlayer + offsets::BasePlayer::modelState, &msPtr))

            return false;

        if (!IsCanonicalUserPtr(msPtr)) return false;

        uint32_t flags = 0;

        if (!SafeRead(msPtr + offsets::ModelState::flags, &flags, 4))

            return false;

        const uint32_t kParented = offsets::ModelState::Mounted

                                 | offsets::ModelState::OnLadder;

        return (flags & kParented) != 0;

    }



    bool g_localParented = false;



    static bool ChoosePeekDirection(const float* real, float peekDist, bool forceVertical,

                                     bool parentedBypass,

                                     float* outPeek, const char** outLabel) {

        const float kEyeH = 1.6f;

        float ex = real[0], ey = real[1] + kEyeH, ez = real[2];



        if (forceVertical) {

            float candY = real[1] + peekDist;

            if (parentedBypass

                || IsLineVisible(ex, ey, ez, ex, candY + kEyeH, ez)) {

                outPeek[0] = real[0]; outPeek[1] = candY; outPeek[2] = real[2];

                *outLabel = parentedBypass ? "vertical_parented" : "vertical_forced";

                return true;

            }

            return false;

        }



        float dx = targetPosX - real[0];

        float dz = targetPosZ - real[2];

        float len = sqrtf(dx * dx + dz * dz);

        if (len < 0.5f) return false;

        float inv = 1.0f / len;

        dx *= inv; dz *= inv;



        if (parentedBypass) {

            outPeek[0] = real[0] + dx * peekDist;

            outPeek[1] = real[1];

            outPeek[2] = real[2] + dz * peekDist;

            *outLabel = "horizontal_parented";

            return true;

        }



        struct Cand { float vx, vy, vz; const char* name; };

        Cand cands[] = {

            {  dx,  0.0f,  dz, "horizontal" },

            { -dz,  0.0f,  dx, "perp_right" },

            {  dz,  0.0f, -dx, "perp_left"  },

            { 0.0f, 1.0f,  0.0f, "vertical" },

        };



        for (const auto& c : cands) {

            float cand[3] = {

                real[0] + c.vx * peekDist,

                real[1] + c.vy * peekDist,

                real[2] + c.vz * peekDist

            };

            if (IsLineVisible(ex, ey, ez,

                              cand[0], cand[1] + kEyeH, cand[2])) {

                outPeek[0] = cand[0];

                outPeek[1] = cand[1];

                outPeek[2] = cand[2];

                *outLabel = c.name;

                return true;

            }

        }

        return false;

    }



    static bool g_antiAimMasterEnabled = true;



    static inline float WrapYaw(float y) {

        while (y > 180.0f)  y -= 360.0f;

        while (y < -180.0f) y += 360.0f;

        return y;

    }



    static inline uint32_t FastRand(uint32_t* s) {

        *s = (*s) * 1664525u + 1013904223u;

        return *s;

    }



    static inline float RandF(uint32_t* s, float lo, float hi) {

        return lo + (hi - lo) * ((float)(FastRand(s) & 0xFFFFFF) / (float)0xFFFFFF);

    }



    static bool LocalPlayerMoving() {

        if (localPlayer == 0) return false;

        static float lastX = 0, lastY = 0, lastZ = 0;

        static unsigned long long lastTick = 0;

        static float cachedSpeed = 0.0f;



        unsigned long long now = GetTickCount64();

        if (now - lastTick < 16) return cachedSpeed > 0.6f;



        if (!g_fnGetTransform || !g_fnGetPosInjected) return false;

        void* tr = g_fnGetTransform((void*)localPlayer, nullptr);

        if (!tr || !IsCanonicalUserPtr((uintptr_t)tr)) return false;

        float p[3] = {};

        g_fnGetPosInjected(tr, p, nullptr);



        if (lastTick != 0) {

            float dt = (float)(now - lastTick) * 0.001f;

            if (dt > 0.0f && dt < 0.5f) {

                float dx = p[0] - lastX, dz = p[2] - lastZ;

                cachedSpeed = sqrtf(dx*dx + dz*dz) / dt;

            }

        }

        lastX = p[0]; lastY = p[1]; lastZ = p[2];

        lastTick = now;

        return cachedSpeed > 0.6f;

    }



    static bool ComputeFakeAngles(const float* real, float* fake) {

        auto& aa = Menu::Get().AntiAimCfg;



        fake[0] = real[0];

        fake[1] = real[1];

        fake[2] = real[2];



        if (!aa.Enable || !g_antiAimMasterEnabled) return false;



        if (aa.DisableOnAim && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return false;



        bool moving = LocalPlayerMoving();

        if (aa.OnlyMoving   && !moving) return false;

        if (aa.OnlyStanding &&  moving) return false;



        float t = (float)ImGui::GetTime();

        static uint32_t s_rng = 0xC0FFEEu;



        bool invertKeyHeld = aa.InverterKey != 0

                             && (GetAsyncKeyState(aa.InverterKey) & 0x8000) != 0;

        bool invert = invertKeyHeld ^ aa.InverterState;

        float invSign = invert ? -1.0f : 1.0f;



        float yawOffset = 0.0f;

        switch (aa.YawMode) {

            case 0: yawOffset = 0.0f; break;

            case 1: yawOffset = 180.0f + (aa.YawBase - 180.0f); break;

            case 2: yawOffset = 90.0f * invSign + (aa.YawBase - 180.0f); break;

            case 3: {

                float j = sinf(t * 18.0f) * aa.YawJitter * invSign;

                yawOffset = (aa.YawBase - 180.0f) + 180.0f + j;

                break;

            }

            case 4: yawOffset = fmodf(t * aa.SpinSpeed, 360.0f); break;

            case 5: {

                float r = RandF(&s_rng, -aa.YawJitter, aa.YawJitter);

                yawOffset = (aa.YawBase - 180.0f) + 180.0f + r;

                break;

            }

        }



        if (aa.FakeDesync) {

            float d = sinf(t * 7.0f) * aa.DesyncAmount * 0.5f * invSign;

            yawOffset += d;

        }



        float fakeYaw = WrapYaw(real[1] + yawOffset);



        float fakePitch = real[0];

        switch (aa.PitchMode) {

            case 0: fakePitch = real[0]; break;

            case 1: fakePitch = +aa.PitchValue; break;

            case 2: fakePitch = -aa.PitchValue; break;

            case 3: fakePitch = 0.0f; break;

            case 4: fakePitch = sinf(t * 9.0f) * aa.PitchJitter; break;

            case 5: fakePitch = RandF(&s_rng, -aa.PitchJitter, aa.PitchJitter); break;

        }



        float fakeRoll = 0.0f;

        if (aa.EnableRoll) {

            switch (aa.RollMode) {

                case 0: fakeRoll = aa.RollValue * invSign; break;

                case 1: fakeRoll = sinf(t * aa.RollJitterSpd) * aa.RollValue; break;

                case 2: {

                    static float lastR = 0.0f;

                    static float lastT = 0.0f;

                    if (t - lastT > 1.0f / (aa.RollJitterSpd + 0.1f)) {

                        lastR = RandF(&s_rng, -aa.RollValue, aa.RollValue);

                        lastT = t;

                    }

                    fakeRoll = lastR;

                    break;

                }

            }

        }



        fake[0] = fakePitch;

        fake[1] = fakeYaw;

        fake[2] = fakeRoll;

        return true;

    }



    static void ProcessAntiAimKeys() {

        auto& aa = Menu::Get().AntiAimCfg;

        static bool wasToggle = false;

        static bool wasInvert = false;



        if (aa.ToggleKey != 0) {

            bool now = (GetAsyncKeyState(aa.ToggleKey) & 0x8000) != 0;

            if (now && !wasToggle) g_antiAimMasterEnabled = !g_antiAimMasterEnabled;

            wasToggle = now;

        }

        if (aa.InverterKey != 0) {

            bool now = (GetAsyncKeyState(aa.InverterKey) & 0x8000) != 0;

            if (now && !wasInvert) aa.InverterState = !aa.InverterState;

            wasInvert = now;

        }

    }



    static void UpdateTargetVisibilityGameThread() {

        auto& acfg = Menu::Get().AimCfg;

        if (!acfg.VisibleCheck) { currentTargetVisible = true; return; }

        if (currentTarget == 0 || localPlayer == 0) return;



        static unsigned long long lastMs = 0;

        unsigned long long now = GetTickCount64();

        if (now - lastMs < 25) return;

        lastMs = now;



        currentTargetVisible = IsLineVisible(

            localEyeX, localEyeY, localEyeZ,

            targetPosX, targetPosY, targetPosZ);

    }



    static void SimulateAutoFire(bool wantFire) {

        static bool lmbHeld = false;

        static unsigned long long lastEdgeMs = 0;

        unsigned long long now = GetTickCount64();



        if (wantFire == lmbHeld) return;

        if (now - lastEdgeMs < 20) return;



        INPUT in = {};

        in.type = INPUT_MOUSE;

        in.mi.dwFlags = wantFire ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;

        SendInput(1, &in, sizeof(INPUT));

        lmbHeld = wantFire;

        lastEdgeMs = now;

    }



    void hkClientInput(void* a0, void* a1, void* a2, void* a3, void* a4) {

        if (!oClientInput) return;

        typedef void(*OrigFn)(void*, void*, void*, void*, void*);



        static unsigned long long callCount = 0;

        callCount++;

        if ((callCount % 3000ULL) == 1ULL) {

            AimLog("[RH][aim] hkClientInput call#%llu this=%p local=%p manip=%d",

                   callCount, a0, (void*)localPlayer, (int)g_manipActive);

        }



        UpdateTargetVisibilityGameThread();



        {

            auto& acfg = Menu::Get().AimCfg;

            bool wantFire = acfg.AutoShoot

                            && acfg.PerfectSilent

                            && currentTarget != 0

                            && localPlayer != 0

                            && (!acfg.VisibleCheck || currentTargetVisible)

                            && (HasShotHook || HasAimConeHook);

            SimulateAutoFire(wantFire);

        }



        auto& wcfg = Menu::Get().WeaponCfg;

        bool smartKey    = wcfg.ManipAutoKey != 0

                           && (GetAsyncKeyState(wcfg.ManipAutoKey) & 0x8000) != 0;

        bool verticalKey = wcfg.ManipVerticalKey != 0

                           && (GetAsyncKeyState(wcfg.ManipVerticalKey) & 0x8000) != 0;

        bool wantManip = wcfg.Manipulator

                         && (smartKey || verticalKey)

                         && currentTarget != 0

                         && localPlayer != 0

                         && g_fnGetTransform

                         && g_fnGetPosInjected

                         && g_fnSetPosInjected;



        g_localParented = IsLocalParented();

        bool parentedBypass = wcfg.AutoParent && g_localParented;

        float effectiveDist = wcfg.ManipPeekDist;

        if (parentedBypass && effectiveDist < 1.5f) effectiveDist = 1.5f;



        float savedPos[3] = {};

        void* transform = nullptr;

        bool applied = false;



        if (wantManip) {

            transform = g_fnGetTransform((void*)localPlayer, nullptr);

            if (transform && IsCanonicalUserPtr((uintptr_t)transform)) {

                g_fnGetPosInjected(transform, savedPos, nullptr);



                float peekPos[3] = {};

                const char* mode = "none";

                if (ChoosePeekDirection(savedPos, effectiveDist,

                                        verticalKey, parentedBypass,

                                        peekPos, &mode)) {

                    g_fnSetPosInjected(transform, peekPos, nullptr);

                    g_manipActive = true;

                    g_manipPeekEyePos[0] = peekPos[0];

                    g_manipPeekEyePos[1] = peekPos[1] + 1.6f;

                    g_manipPeekEyePos[2] = peekPos[2];

                    applied = true;



                    if ((callCount % 500ULL) == 1ULL) {

                        AimLog("[RH][manip] PEEK mode=%s parented=%d real=(%.1f,%.1f,%.1f) peek=(%.1f,%.1f,%.1f) dist=%.2f",

                               mode, (int)parentedBypass,

                               savedPos[0], savedPos[1], savedPos[2],

                               peekPos[0], peekPos[1], peekPos[2],

                               effectiveDist);

                    }

                } else if ((callCount % 500ULL) == 1ULL) {

                    AimLog("[RH][manip] BLOCKED all directions parented=%d real=(%.1f,%.1f,%.1f) dist=%.2f",

                           (int)parentedBypass,

                           savedPos[0], savedPos[1], savedPos[2], effectiveDist);

                }

            }

        }



        ProcessAntiAimKeys();



        float  realAngles[3] = {};

        float  fakeAngles[3] = {};

        bool   antiAimActive = false;

        uintptr_t playerInputAddr = 0;



        if (localPlayer != 0) {

            uintptr_t ipPtr = 0;

            if (SafeReadPtr(localPlayer + offsets::BasePlayer::playerInput, &ipPtr)

                && IsCanonicalUserPtr(ipPtr))

            {

                uintptr_t ipKlass1 = 0, ipKlass2 = 0;

                if (SafeReadPtr(ipPtr, &ipKlass1) && IsCanonicalUserPtr(ipKlass1)

                    && SafeReadPtr(ipPtr, &ipKlass2) && ipKlass1 == ipKlass2)

                {

                    uintptr_t angleAddr = ipPtr + offsets::PlayerInput::bodyAngles;

                    if (SafeRead(angleAddr, realAngles, 12)) {

                        bool finite = true;

                        for (int i = 0; i < 3; ++i) {

                            if (!std::isfinite(realAngles[i])) { finite = false; break; }

                        }

                        if (finite && ComputeFakeAngles(realAngles, fakeAngles)) {

                            for (int i = 0; i < 3; ++i) {

                                if (!std::isfinite(fakeAngles[i])) { finite = false; break; }

                            }

                            if (finite) {

                                SafeWrite(angleAddr, fakeAngles, 12);

                                antiAimActive = true;

                                playerInputAddr = ipPtr;

                            }

                        }

                    }

                }

            }

        }



        ((OrigFn)oClientInput)(a0, a1, a2, a3, a4);



        if (applied && transform) {

            g_fnSetPosInjected(transform, savedPos, nullptr);

            g_manipActive = false;

        }



        if (antiAimActive && playerInputAddr != 0) {

            uintptr_t angleAddr = playerInputAddr + offsets::PlayerInput::bodyAngles;

            float afterFake[3] = {};

            if (SafeRead(angleAddr, afterFake, 12)) {

                float delta[3] = {

                    afterFake[0] - fakeAngles[0],

                    WrapYaw(afterFake[1] - fakeAngles[1]),

                    afterFake[2] - fakeAngles[2],

                };

                float restoreReal[3] = {

                    realAngles[0] + delta[0],

                    WrapYaw(realAngles[1] + delta[1]),

                    realAngles[2] + delta[2],

                };

                bool ok = true;

                for (int i = 0; i < 3; ++i) {

                    if (!std::isfinite(restoreReal[i])) { ok = false; break; }

                }

                if (restoreReal[0] >  89.0f) restoreReal[0] =  89.0f;

                if (restoreReal[0] < -89.0f) restoreReal[0] = -89.0f;

                if (ok) SafeWrite(angleAddr, restoreReal, 12);

            }

        }

    }



    typedef void(__fastcall* fnSendTick_t)(void*, void*, void*, void*, void*);

    static fnSendTick_t oSendTick = nullptr;

    static bool HasSendTickHook = false;



    static uintptr_t ResolveAimAnglesAddr(uintptr_t basePlayer) {

        if (basePlayer == 0) return 0;

        uintptr_t playerInput = 0;

        if (!SafeReadPtr(basePlayer + offsets::BasePlayer::playerInput, &playerInput)

            || !IsCanonicalUserPtr(playerInput))

            return 0;

        uintptr_t state = 0;

        if (!SafeReadPtr(playerInput + offsets::PlayerInput::inputState, &state)

            || !IsCanonicalUserPtr(state))

            return 0;

        uintptr_t curMsg = 0;

        if (!SafeReadPtr(state + offsets::InputState::current, &curMsg)

            || !IsCanonicalUserPtr(curMsg))

            return 0;

        return curMsg + offsets::InputMessage::aimAngles;

    }



    void __fastcall hkSendTick(void* a0, void* a1, void* a2, void* a3, void* a4) {

        if (!oSendTick) return;



        uintptr_t bp = (uintptr_t)a0;

        if (bp != localPlayer || bp == 0) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        auto& aa = Menu::Get().AntiAimCfg;

        if (!aa.Enable || !g_antiAimMasterEnabled) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        if (aa.DisableOnAim && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        uintptr_t aimAddr = ResolveAimAnglesAddr(bp);

        if (aimAddr == 0) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        float realAim[3] = {};

        if (!SafeRead(aimAddr, realAim, 12)) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        bool finite = true;

        for (int i = 0; i < 3; ++i)

            if (!std::isfinite(realAim[i])) { finite = false; break; }



        float fakeAim[3] = {};

        if (!finite || !ComputeFakeAngles(realAim, fakeAim)) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        for (int i = 0; i < 3; ++i)

            if (!std::isfinite(fakeAim[i])) { finite = false; break; }

        if (!finite) {

            ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);

            return;

        }



        SafeWrite(aimAddr, fakeAim, 12);



        ((fnSendTick_t)oSendTick)(a0, a1, a2, a3, a4);



        SafeWrite(aimAddr, realAim, 12);

    }



    static bool HookFunctionRobust(void* fnPtr, void* hook, void** origOut, const char* label);

    static bool LooksLikeX64Prologue(const unsigned char* b);



    static bool TryInstallSendTickHook() {

        if (HasSendTickHook) return true;

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!ga) {

            AimLog("[RH][aim] SendTick: GameAssembly.dll not loaded");

            return false;

        }

        const uintptr_t kRVA = 0x4579D00;

        void* fnPtr = (void*)((uintptr_t)ga + kRVA);



        unsigned char bytes[16] = {0};

        if (SafeRead((uintptr_t)fnPtr, bytes, 16)) {

            char line[96] = {}; int pos = 0;

            for (int i = 0; i < 16; i++)

                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[i]);

            bool plausible = LooksLikeX64Prologue(bytes);

            AimLog("[RH][aim] SendTick: ga=%p fn=%p plausible=%d bytes=%s",

                   (void*)ga, fnPtr, (int)plausible, line);

            if (!plausible) {

                AimLog("[RH][aim] SendTick: prologue mismatch \u2014 RVA stale, skipping hook");

                return false;

            }

        } else {

            AimLog("[RH][aim] SendTick: failed to read bytes at fn=%p", fnPtr);

            return false;

        }



        if (HookFunctionRobust(fnPtr, (void*)&hkSendTick,

                               (void**)&oSendTick, "SendClientTick"))

        {

            HasSendTickHook = true;

            AimLog("[RH][aim] SendTick hook installed at %p", fnPtr);

            return true;

        }

        AimLog("[RH][aim] SendTick hook FAILED");

        return false;

    }



    // Try a list of candidate methods on BasePlayer and install the first one

    // that MinHook successfully hooks. Populates g_hookKind, g_hookedName,

    // g_hookedArgc and oClientInput.

    bool InstallInputHook() {

        Il2CppImage* img = il2cpp.FindImage("Assembly-CSharp.dll");

        if (!img) { AimLog("[RH][aim] InstallInputHook: image missing"); return false; }

        Il2CppClass* klass = il2cpp.class_from_name(img, "", "BasePlayer");

        if (!klass) { AimLog("[RH][aim] InstallInputHook: BasePlayer class missing"); return false; }



        // NOTE: this build of Rust has aggressive method-name obfuscation —

        // almost every BasePlayer method is renamed to %<hex>. The input

        // tick methods we traditionally target (ClientInput/PlayerTick/etc.)

        // are all renamed, so we fall back to non-obfuscated entry points.

        //

        // HK_NO_ARG means "hook receives only `this` (plus MethodInfo*) and

        // we don't get an InputState pointer — we work via PlayerInput.bodyAngles

        // instead, which is at BasePlayer.playerInput(0x670) + 0x44".

        struct Candidate { const char* name; int argc; HookedMethodKind kind; };

        Candidate cands[] = {

            // Preferred: per-tick local player update. Non-obfuscated in the

            // current dump. Signature: ClientUpdateLocalPlayer(bool something).

            { "ClientUpdateLocalPlayer", 1, HK_NO_ARG         },

            // Legacy / non-obfuscated builds.

            { "ClientInput",             1, HK_INPUTSTATE_ARG },

            { "PlayerTick",              1, HK_INPUTSTATE_ARG },

            { "OnInput",                 1, HK_INPUTSTATE_ARG },

            { "PlayerTick",              3, HK_INPUTSTATE_ARG },

            { "ClientInput",             0, HK_NO_ARG         },

            { "DoClientInput",           0, HK_NO_ARG         },

            // Additional non-obfuscated fallbacks that run per-frame on the

            // local player — any of these will let us do bodyAngles-based

            // silent aim even though we won't see the InputState directly.

            { "UpdateClothingItems",     1, HK_NO_ARG         },

            { "UpdateViewMode",          0, HK_NO_ARG         },

            { "ClientOnEnable",          0, HK_NO_ARG         },

        };



        for (const auto& c : cands) {

            void* method = il2cpp.class_get_method_from_name(klass, c.name, c.argc);

            if (!method) {

                AimLog("[RH][aim] cand BasePlayer::%s(argc=%d) not found", c.name, c.argc);

                continue;

            }

            void* fnPtr = *(void**)method;

            if (!fnPtr) {

                AimLog("[RH][aim] cand BasePlayer::%s(argc=%d) methodPointer is NULL", c.name, c.argc);

                continue;

            }

            MH_STATUS st = MH_CreateHook(fnPtr, (void*)&hkClientInput, (void**)&oClientInput);

            if (st != MH_OK) {

                AimLog("[RH][aim] MH_CreateHook FAILED for BasePlayer::%s(argc=%d) at %p: status=%d",

                       c.name, c.argc, fnPtr, (int)st);

                oClientInput = nullptr;

                continue;

            }

            g_hookKind = c.kind;

            g_hookedName = c.name;

            g_hookedArgc = c.argc;

            g_hookedFnPtr = fnPtr;

            AimLog("[RH][aim] MH_CreateHook OK: BasePlayer::%s(argc=%d) at %p kind=%d",

                   c.name, c.argc, fnPtr, (int)c.kind);

            if (c.kind == HK_NO_ARG && serverInputOffset == 0) {

                AimLog("[RH][aim] NOTE: 0-arg hook -- InputState not available; silent aim uses PlayerInput.bodyAngles write/restore");

            }

            return true;

        }

        AimLog("[RH][aim] InstallInputHook: no candidate method worked");

        return false;

    }



    // ---- Hardware breakpoint + Vectored Exception Handler (multi-hook) ----

    // Fallback when MH_EnableHook fails (EAC protects GameAssembly.dll pages

    // so MinHook can't write the 5-byte JMP patch). HW BP + VEH does NOT

    // modify target code, so it bypasses page protection.

    //

    // Up to 4 hooks supported (one per debug register DR0..DR3).

    // An OPTIONAL preHook runs inside the VEH handler BEFORE the RIP is

    // redirected to the C++ detour. It gets the raw EXCEPTION_POINTERS so it

    // can mutate CPU registers (Xmm, GPR) — needed when the compiler cached

    // values in callee-saved XMM registers that survive the original call

    // and that we need to override (e.g., velocity that gets serialized into

    // a protobuf packet AFTER our detour returns).

    typedef void(__stdcall* HwBpPreHookFn)(EXCEPTION_POINTERS* ep);

    struct HwBpEntry {

        void* target;

        void* detour;

        HwBpPreHookFn preHook;

    };

    static HwBpEntry g_hwBps[4] = {};

    static int       g_hwBpCount = 0;

    static void*     g_vehHandle = nullptr;



    // Flag: when true, the next EXCEPTION_BREAKPOINT (INT3) triggers

    // DR arm on the current thread. Used by ArmHwBpCurrentThread.

    static volatile bool g_armBpRequested = false;



    static int PlaceHwBpsInContext(CONTEXT& ctx, int* slotsBusyByOthers) {

        DWORD64* drs[4] = { &ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3 };

        int othersBefore = 0;

        for (int s = 0; s < 4; s++) {

            bool active = ((ctx.Dr7 >> (s * 2)) & 3) != 0;

            if (active) {

                bool isOurs = false;

                for (int i = 0; i < g_hwBpCount && i < 4; i++) {

                    if (*drs[s] == (DWORD64)g_hwBps[i].target) { isOurs = true; break; }

                }

                if (!isOurs) othersBefore++;

            }

        }

        if (slotsBusyByOthers) *slotsBusyByOthers = othersBefore;



        int placed = 0;

        for (int i = 0; i < g_hwBpCount && i < 4; i++) {

            int slot = -1;

            for (int s = 0; s < 4; s++) {

                bool active = ((ctx.Dr7 >> (s * 2)) & 3) != 0;

                if (active && *drs[s] == (DWORD64)g_hwBps[i].target) {

                    slot = s; break;

                }

            }

            if (slot < 0) {

                for (int s = 0; s < 4; s++) {

                    if (((ctx.Dr7 >> (s * 2)) & 3) == 0) { slot = s; break; }

                }

            }

            if (slot < 0) continue;

            DWORD64 ctlMask = (3ULL << (slot * 2)) | (0xFULL << (16 + slot * 4));

            ctx.Dr7 = (ctx.Dr7 & ~ctlMask) | (1ULL << (slot * 2));

            *drs[slot] = (DWORD64)g_hwBps[i].target;

            placed++;

        }

        return placed;

    }



    static LONG CALLBACK HwBpVehHandler(EXCEPTION_POINTERS* ep) {

        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && g_armBpRequested) {

            g_armBpRequested = false;

            CONTEXT tmp = {};

            tmp.Dr0 = ep->ContextRecord->Dr0;

            tmp.Dr1 = ep->ContextRecord->Dr1;

            tmp.Dr2 = ep->ContextRecord->Dr2;

            tmp.Dr3 = ep->ContextRecord->Dr3;

            tmp.Dr7 = ep->ContextRecord->Dr7;

            int othersBefore = 0;

            int placed = PlaceHwBpsInContext(tmp, &othersBefore);

            ep->ContextRecord->Dr0 = tmp.Dr0;

            ep->ContextRecord->Dr1 = tmp.Dr1;

            ep->ContextRecord->Dr2 = tmp.Dr2;

            ep->ContextRecord->Dr3 = tmp.Dr3;

            ep->ContextRecord->Dr7 = tmp.Dr7;

            AimLog("[RH][aim] arm-self tid=%u placed=%d/%d othersBefore=%d Dr7=0x%llX",

                   GetCurrentThreadId(), placed, g_hwBpCount, othersBefore,

                   (unsigned long long)tmp.Dr7);

            ep->ContextRecord->Rip += 1;

            return EXCEPTION_CONTINUE_EXECUTION;

        }



        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)

            return EXCEPTION_CONTINUE_SEARCH;

        void* rip = (void*)ep->ContextRecord->Rip;

        for (int i = 0; i < g_hwBpCount; i++) {

            if (g_hwBps[i].target == rip) {

                // Run the preHook FIRST (if any). It can mutate CPU state

                // (including callee-saved XMM regs that survive the call and

                // feed downstream protobuf writes). Doing this before the

                // RIP redirect means the detour AND the rest of the caller

                // pipeline observe our modifications.

                if (g_hwBps[i].preHook) {

                    g_hwBps[i].preHook(ep);

                }

                // Modify-only mode: when detour is null, the preHook already

                // did all the work (e.g. mutated an XMM register that the

                // original instruction is about to consume). Resume execution

                // at the same RIP — RF=1 prevents instant re-fire.

                if (g_hwBps[i].detour == nullptr) {

                    ep->ContextRecord->EFlags |= 0x10000; // RF

                    return EXCEPTION_CONTINUE_EXECUTION;

                }

                // Standard mode: redirect to our detour. Registers

                // (RCX/RDX/R8/R9/XMM0-3) are untouched so the detour receives

                // the caller's arguments.

                ep->ContextRecord->Rip = (DWORD64)g_hwBps[i].detour;

                ep->ContextRecord->EFlags |= 0x10000; // RF

                return EXCEPTION_CONTINUE_EXECUTION;

            }

        }

        return EXCEPTION_CONTINUE_SEARCH;

    }



    static int RefreshHwBpsAllThreads() {

        DWORD pid  = GetCurrentProcessId();

        DWORD self = GetCurrentThreadId();

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

        if (snap == INVALID_HANDLE_VALUE) return 0;

        int totalThreads      = 0;

        int armed             = 0;

        int skippedNoSlot     = 0;

        int threadsWithOthers = 0;

        int totalOtherSlots   = 0;

        THREADENTRY32 te = {};

        te.dwSize = sizeof(te);

        if (Thread32First(snap, &te)) {

            do {

                if (te.th32OwnerProcessID != pid) continue;

                if (te.th32ThreadID == self)     continue;

                HANDLE hThread = OpenThread(

                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,

                    FALSE, te.th32ThreadID);

                if (!hThread) continue;

                totalThreads++;

                if (SuspendThread(hThread) != (DWORD)-1) {

                    CONTEXT ctx = {};

                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

                    if (GetThreadContext(hThread, &ctx)) {

                        int othersBefore = 0;

                        int placed = PlaceHwBpsInContext(ctx, &othersBefore);

                        if (othersBefore > 0) {

                            threadsWithOthers++;

                            totalOtherSlots += othersBefore;

                        }

                        if (placed >= g_hwBpCount) {

                            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

                            if (SetThreadContext(hThread, &ctx)) armed++;

                        } else {

                            skippedNoSlot++;

                        }

                    }

                    ResumeThread(hThread);

                }

                CloseHandle(hThread);

            } while (Thread32Next(snap, &te));

        }

        CloseHandle(snap);

        AimLog("[RH][aim] RefreshHwBpsAllThreads: armed=%d skippedNoSlot=%d "

               "threadsWithOthers=%d totalOtherSlots=%d total=%d g_hwBpCount=%d",

               armed, skippedNoSlot, threadsWithOthers, totalOtherSlots,

               totalThreads, g_hwBpCount);

        return armed;

    }



    // Arm HW breakpoints on the CURRENT thread. RefreshHwBpsAllThreads

    // cannot suspend self, so we use a trick: set a flag and trigger INT3.

    // The VEH handler sees the flag, modifies the debug registers in the

    // exception context, and CONTINUE_EXECUTION applies them atomically.

    // This is the only reliable ring-3 x64 method for self-thread DR set.

    static bool ArmHwBpCurrentThread() {

        if (!g_vehHandle) {

            AimLog("[RH][aim] ArmHwBpCurrentThread: VEH not installed");

            return false;

        }

        g_armBpRequested = true;

        __debugbreak(); // triggers EXCEPTION_BREAKPOINT -> VEH sets DRs

        AimLog("[RH][aim] ArmHwBpCurrentThread: armed %d BPs on tid=%u",

               g_hwBpCount, GetCurrentThreadId());

        return true;

    }



    // Register a (target, detour) pair and re-arm HW BPs across all threads.

    // Returns true if the BP is now armed on at least one thread.

    static bool AddHwBpEx(void* target, void* detour, HwBpPreHookFn preHook,

                          const char* label) {

        if (g_hwBpCount >= 4) {

            AimLog("[RH][aim] HW BP table full (4/4); can't add '%s'", label ? label : "?");

            return false;

        }

        if (!g_vehHandle) {

            g_vehHandle = AddVectoredExceptionHandler(1, HwBpVehHandler);

            if (!g_vehHandle) {

                AimLog("[RH][aim] AddVectoredExceptionHandler failed");

                return false;

            }

        }

        g_hwBps[g_hwBpCount].target = target;

        g_hwBps[g_hwBpCount].detour = detour;

        g_hwBps[g_hwBpCount].preHook = preHook;

        int slot = g_hwBpCount;

        g_hwBpCount++;

        int n = RefreshHwBpsAllThreads();

        // Also arm on the current thread (RefreshHwBps skips self).

        if (ArmHwBpCurrentThread()) n++;

        AimLog("[RH][aim] HW BP[DR%d] '%s' target=%p detour=%p preHook=%p armed on %d threads",

               slot, label ? label : "?", target, detour, (void*)preHook, n);

        return n > 0;

    }



    static bool AddHwBp(void* target, void* detour, const char* label) {

        return AddHwBpEx(target, detour, nullptr, label);

    }



    // Public entry point (declared in aim.h) for "modify-only" HW BP hooks:

    // the preHook mutates CPU regs in EXCEPTION_POINTERS, then the original

    // instruction runs as-is. Use case: replacing the float value held in

    // XMM1 right before Camera::set_fieldOfView's native icall consumes it,

    // bypassing EAC's MinHook page-protect block on UnityPlayer.dll.

    bool AddHwBpModifyOnly(void* target,

                           void(__stdcall* preHook)(EXCEPTION_POINTERS*),

                           const char* label) {

        return AddHwBpEx(target, nullptr, (HwBpPreHookFn)preHook, label);

    }



    bool AddHwBpExecute(void* target, void* detour, const char* label) {

        return AddHwBpEx(target, detour, nullptr, label);

    }



    int GetHwBpUsedSlots() {

        return g_hwBpCount;

    }



    // Attach / replace the preHook for an already-installed HW BP target.

    // Useful when the hook was registered via HookFunctionRobust (which

    // doesn't know about preHooks) but we still need VEH-level register

    // patching — e.g., for Projectile.Create where callee-saved XMM regs

    // survive the call and feed the protobuf packet.

    static bool SetHwBpPreHookFor(void* target, HwBpPreHookFn preHook) {

        for (int i = 0; i < g_hwBpCount; i++) {

            if (g_hwBps[i].target == target) {

                g_hwBps[i].preHook = preHook;

                AimLog("[RH][aim] preHook %p attached to HW BP[DR%d] target=%p",

                       (void*)preHook, i, target);

                return true;

            }

        }

        AimLog("[RH][aim] SetHwBpPreHookFor: target %p not found in BP table", target);

        return false;

    }



    bool EnableInputHook() {

        if (!g_hookedFnPtr) {

            AimLog("[RH][aim] EnableInputHook: no hook installed");

            return false;

        }



        // Attempt 1: standard MH_EnableHook. Works on normal (writable) pages.

        MH_STATUS st = MH_EnableHook(g_hookedFnPtr);

        if (st == MH_OK) {

            AimLog("[RH][aim] MH_EnableHook OK for %s at %p", g_hookedName, g_hookedFnPtr);

            return true;

        }

        AimLog("[RH][aim] MH_EnableHook returned %d; falling back to HW breakpoint",

               (int)st);

        return AddHwBp(g_hookedFnPtr, (void*)&hkClientInput, "ClientInput");

    }



    // ---- Shot-level hooks (BaseProjectile) -------------------------------

    // These fire ONLY at the exact moment of shooting, so our save->write->

    // restore of bodyAngles stays local to the shot and doesn't wipe the

    // mouse delta / recoil applied elsewhere in the tick.

    //

    // IL2CPP x64 instance-method ABI: (this, [args...], MethodInfo*)

    // All three target methods are argc=0, so the signature collapses to

    // (this, MethodInfo*).

    // Use the universal 5-arg IL2CPP ABI pattern (same as hkClientInput): RCX,

    // RDX, R8, R9 + 1 stack slot. Declaring fewer params risks clobbering

    // arguments the generator may or may not emit (MethodInfo* / this-dispatch

    // sugar) depending on the build.

    typedef void(__fastcall *fnBP5_t)(void*, void*, void*, void*, void*);

    static fnBP5_t oDoAttack         = nullptr;

    static fnBP5_t oLaunchProjectile = nullptr;

    static fnBP5_t oSimulateAimcone  = nullptr;

    // Raw address of LaunchProjectile in the game module. Used by the

    // one-shot diagnostic dump so we can see the exact `[rcx+XX]` reads

    // the function performs on the BaseProjectile `this` pointer.

    static void*   g_launchProjectileFnAddr = nullptr;

    bool HasShotHook = false;



    // ---- Projectile-direction vmethod hook --------------------------------

    // IDA analysis of LaunchProjectile body (sub_184038C60) at address

    // 0x184038C60 shows a per-projectile loop that normalizes startVelocity,

    // passes it through a VIRTUAL method, and then stores the result as

    // projectile.modifiedDirection (which the network serializer reads).

    //

    // The virtual call is:

    //     v128 = *this;                                  ; klass

    //     v129 = (*(int64(**)(out, this, item, in, mi))

    //             (v128 + 0x3F08))                       ; vtable slot 2017

    //                (v173 /*out buf 40b*/,

    //                 a1   /*this*/,

    //                 v8   /*Item*/,

    //                 &v152 /*input Vector3 normalized dir*/,

    //                 *(vtable + 0x3F10) /*MethodInfo*/);

    //     v150 = *(_QWORD *)v129;       ; bytes 0..7 (x, y)

    //     v151 = *(float *)(v129 + 8);  ; byte 8..11 (z)

    //     sub_18539FD80(proj, &v150, 0); ; set modifiedDirection on projectile

    //

    // So if we hook vtable[0x3F08] and overwrite the first 12 bytes of the

    // output buffer (the Vector3 returned) with our target direction, the

    // projectile's modifiedDirection (and thus the velocity the server sees)

    // will point at the target — without ever touching camera or aim angles.

    //

    // NOTE: official Rust builds had this at offset 0x4348; our pirated build

    // has it at 0x3F08. We try BOTH slots and dump the target bytes so if the

    // first slot is wrong we can fall back.

    //

    // Signature: __int64 __fastcall(void* outBuf, void* this, __int64 item,

    //                               float* inputDir, void* methodInfo).

    //   Return value is a pointer to the filled Vector3 inside outBuf.

    typedef __int64 (__fastcall* tProjDirVM)(void* outBuf, void* thisPtr,

                                             __int64 item, float* inputDir,

                                             void* methodInfo);

    static tProjDirVM oProjDirVM = nullptr;

    static uintptr_t  g_projDirVMAddr = 0;

    static uintptr_t  g_projDirVMOffset = 0; // actual klass offset hooked

    static bool       g_projDirVMTried = false;

    // Detour + installer are defined further down next to ShouldSilentThisShot.



    // ---- AimConeUtil.GetModifiedAimConeDirection hook ----------------------

    // This is the function that applies aimcone spread to the bullet direction.

    // Signature: static Vector3 GetModifiedAimConeDirection(float aimCone,

    //            Vector3 inputVec, bool anywhereInside)

    // On x64 MSVC: Vector3 (12 bytes) returned via hidden first ptr param.

    // IL2CPP adds MethodInfo* at the end (unused).

    struct Vec3 {

        float x, y, z;

        Vec3() : x(0), y(0), z(0) {}

        Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }

        float Length() const { return sqrtf(x*x + y*y + z*z); }

        Vec3 Normalized() const {

            float l = Length();

            if (l < 0.0001f) return { 0, 0, 1 };

            return { x/l, y/l, z/l };

        }

    };



    typedef Vec3(__fastcall* fnAimCone_t)(float aimCone, Vec3 inputVec, bool anywhereInside);

    static fnAimCone_t oGetModifiedAimConeDirection = nullptr;

    bool HasAimConeHook = false;



    // ---- PlayerEyes.BodyForward / HeadForward hooks ------------------------

    // These are the getters LaunchProjectile calls internally to obtain the

    // bullet's aim direction. On x64 Windows, a Vector3 return (12 bytes > 8)

    // uses a hidden pointer as the FIRST arg:

    //   void BodyForward(Vector3* retval, PlayerEyes* this_, MethodInfo* mi);

    // We intercept and overwrite *retval with our target direction when

    // silent-aim is active AND we're currently executing inside

    // oLaunchProjectile (tracked via g_insideLaunchProjectile thread-local).

    // Without the inside-launch guard we'd poison EVERY BodyForward call the

    // game makes for rendering/animation/IK — that would visibly break the

    // player's head/body rotation in third-person view.

    typedef void(__fastcall* fnForward_t)(void* retval, void* self, void* mi,

                                          void* unused1, void* unused2);

    static fnForward_t oBodyForward = nullptr;

    static fnForward_t oHeadForward = nullptr;

    bool HasBodyForwardHook = false;

    bool HasHeadForwardHook = false;



    // Thread ID of the thread currently executing oLaunchProjectile, or 0.

    // BodyForward / HeadForward hooks compare against GetCurrentThreadId()

    // to know if they were called from inside an active shot (and thus

    // should hijack the return value).

    //

    // We deliberately avoid C++ thread_local here: in a dynamically

    // injected DLL on older Windows versions the TLS slot may not be

    // allocated on pre-existing game threads, which can crash on first

    // access. Plain 32-bit aligned writes are atomic on x86/x64.

    static volatile DWORD g_launchThreadId = 0;



    Vec3 __fastcall hkGetModifiedAimConeDirection(float aimCone, Vec3 inputVec, bool anywhereInside) {

        if (!oGetModifiedAimConeDirection) return inputVec;



        // Heartbeat log — shows the hook is actually firing at shot time.

        static unsigned long long acuHits = 0;

        ++acuHits;

        if ((acuHits % 20ULL) == 1ULL) {

            AimLog("[RH][aim] hkAimCone#%llu cone=%.3f in=(%.2f,%.2f,%.2f) target=%p",

                   acuHits, aimCone, inputVec.x, inputVec.y, inputVec.z, (void*)currentTarget);

        }



        auto& wcfg = Menu::Get().WeaponCfg;

        auto& acfg = Menu::Get().AimCfg;



        // Perfect Silent: redirect bullet to target WITHOUT moving camera.

        // Compute direction from local eye to target bone position.

        if (acfg.Enabled && acfg.PerfectSilent && currentTarget != 0) {

            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            if (lmbDown) {

                Vec3 eye = { localEyeX, localEyeY, localEyeZ };

                Vec3 tgt = { targetPosX, targetPosY, targetPosZ };

                Vec3 dir = (tgt - eye).Normalized();

                if ((acuHits % 20ULL) == 1ULL) {

                    AimLog("[RH][aim] hkAimCone: PSILENT dir=(%.3f,%.3f,%.3f)", dir.x, dir.y, dir.z);

                }

                // Call original with aimCone=0 and direction to target.

                return oGetModifiedAimConeDirection(0.0f, dir, anywhereInside);

            }

        }



        // NoSpread: pass aimCone=0 so no randomization is applied.

        if (wcfg.NoSpread) {

            return oGetModifiedAimConeDirection(0.0f, inputVec, anywhereInside);

        }



        return oGetModifiedAimConeDirection(aimCone, inputVec, anywhereInside);

    }



    // Convert Euler (pitch, yaw, roll=0) in degrees to a Unity Quaternion.

    // Unity uses ZXY rotation order. With roll=0 this collapses to:

    //   qx =  sin(p/2) * cos(y/2)

    //   qy =  cos(p/2) * sin(y/2)

    //   qz = -sin(p/2) * sin(y/2)

    //   qw =  cos(p/2) * cos(y/2)

    static inline void EulerToUnityQuat(float pitchDeg, float yawDeg, float out[4]) {

        const float D2R = 0.01745329251994329577f;

        float hx = pitchDeg * D2R * 0.5f;

        float hy = yawDeg   * D2R * 0.5f;

        float sx = sinf(hx), cx = cosf(hx);

        float sy = sinf(hy), cy = cosf(hy);

        out[0] =  sx * cy;   // x

        out[1] =  cx * sy;   // y

        out[2] = -sx * sy;   // z

        out[3] =  cx * cy;   // w

    }



    // Plausible offsets in PlayerEyes where bodyRotation (Quaternion) or

    // viewAngles (Vector3) might live across different Rust build revisions.

    // Aggressive obfuscation prevents us from nailing the exact one, so we

    // shotgun-write all of them and restore after. The game reads whichever

    // one controls bullet direction.

    static constexpr int kQuatOffsets[] = { 0x40, 0x48, 0x50, 0x54, 0x58, 0x60, 0x68, 0x70, 0x78 };

    static constexpr int kVec3Offsets[] = { 0x20, 0x24, 0x28, 0x30, 0x38 };

    // Bumped for the shotgun scanner that records every unit-quaternion /

    // unit-vector-looking slot it finds in PlayerEyes + BaseProjectile.

    static constexpr int kMaxSavedSlots = 96;



    struct SilentSlot {

        uintptr_t addr;

        int       size;           // 12 for Vec3, 16 for Quaternion

        uint8_t   oldBytes[16];

    };



    struct SilentState {

        SilentSlot slots[kMaxSavedSlots];

        int        count;

    };



    // Silent aim: save -> write -> restore a MINIMAL pair of rotation slots

    // around the oLaunchProjectile call.

    //

    // CRASH-HARDENING NOTE — DO NOT EXPAND THIS WRITE SET WITHOUT EVIDENCE.

    // Earlier versions wrote up to SIX slots: PlayerInput.bodyAngles +

    // 3 PlayerInput "candidate" quaternions {0x34, 0x64, 0xFC} + PlayerEyes

    // bodyRotation + PlayerEyes "unkQuanternion" @0x6C. Those four extra

    // slots had unverified semantics and corresponded to a long-standing

    // crash pattern: when the target's visibility changed rapidly (going

    // behind cover, mounting/dismounting a vehicle like a Minicopter while

    // we kept firing), the GAME thread reading our spoofed values from

    // an unknown-purpose field could feed garbage into Unity's animation

    // / IK / Quaternion.LookRotation paths and AV inside Unity native code.

    //

    // The Projectile.Create hook already redirects bullet direction (and

    // patches XMM6..15 so the server's protobuf gets matching velocity),

    // so the visual-and-server silent-aim path is fully covered without

    // any of these extra writes. We keep ONLY the two writes that have

    // a known, well-defined purpose:

    //   1. PlayerInput.bodyAngles  (Vector3 Euler) -> server validation

    //   2. PlayerEyes.bodyRotation (Quaternion)    -> client-side raycast

    //

    // `selfProjectile` is unused for now but kept in the signature so the

    // LaunchProjectile hook can pass its this-pointer — we may need to

    // touch weapon-level rotation caches later.

    static SilentState BeginSilentAngles(uintptr_t selfProjectile = 0) {

        (void)selfProjectile;

        SilentState s = {};

        if (localPlayer == 0) return s;



        // Defensive: verify localPlayer's klass pointer is canonical AND

        // stable across two consecutive reads. The crash pattern this

        // guards: target rapidly transitioning visibility (helicopter

        // occlusion, cover) can coincide with the game thread freeing

        // and re-allocating the local player's PlayerInput / PlayerEyes

        // sub-objects. Reading a stale localPlayer-derived pointer and

        // writing to it lands the spoofed quaternion in unrelated heap

        // memory, eventually crashing in Unity. Two-sample read filters

        // the torn-state window cheaply.

        uintptr_t klass1 = 0, klass2 = 0;

        if (!SafeReadPtr(localPlayer, &klass1)

            || !IsCanonicalUserPtr(klass1)

            || !SafeReadPtr(localPlayer, &klass2)

            || klass1 != klass2)

        {

            return s;

        }



        // HARD NaN / out-of-range GUARD: currentPitch / currentYaw are

        // set by Aim::Update on the render thread. If a race or a bad

        // frame slipped a NaN in (e.g. target swap mid-read, targetPos

        // zero while localEye is non-zero producing a degenerate atan2),

        // writing a NaN quaternion into PlayerEyes / PlayerInput would

        // flow into the game's Unity LookRotation / animation blending

        // and crash the process — often one shot later when the game

        // re-reads the corrupted field. Bail out cleanly instead: the

        // shot just fires with the game's natural aim (no silent).

        float pitch = currentPitch;

        float yaw   = currentYaw;

        if (!(pitch == pitch) || !(yaw == yaw)) return s;

        if (pitch < -90.0f || pitch > 90.0f)   return s;

        if (yaw   < -360.0f || yaw  > 360.0f)  return s;



        // Target position must also be finite — if currentTarget got

        // cleared / re-acquired between the ShouldSilentThisShot gate

        // and here, targetPosX/Y/Z could still carry the previous valid

        // sample, but in a torn-read edge case they could read back as

        // NaN. Float loads are atomic on x64 so this is rare but cheap

        // to guard.

        if (!(targetPosX == targetPosX) ||

            !(targetPosY == targetPosY) ||

            !(targetPosZ == targetPosZ)) return s;



        // Pre-compute target quaternion + Euler triple.

        float q[4];

        EulerToUnityQuat(pitch, yaw, q);

        // Validate the computed quaternion — EulerToUnityQuat only uses

        // sinf/cosf so the only way out is NaN or non-unit. Should never

        // trigger after the guards above, but we keep the check so a

        // broken future refactor of EulerToUnityQuat can never crash the

        // game.

        float qMag2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];

        if (!(qMag2 >= 0.9f) || !(qMag2 <= 1.1f)) return s;

        for (int i = 0; i < 4; i++) {

            if (!(q[i] == q[i])) return s;

        }

        float v3Euler[3] = { pitch, yaw, 0.0f };



        auto saveAndWrite = [&](uintptr_t addr, const void* newVal, int size) {

            if (s.count >= kMaxSavedSlots) return;

            if (!IsCanonicalUserPtr(addr)) return;

            // Deduplicate — don't record the same slot twice.

            for (int i = 0; i < s.count; i++) {

                if (s.slots[i].addr == addr) return;

            }

            SilentSlot& slot = s.slots[s.count];

            if (!SafeRead(addr, slot.oldBytes, size)) return;

            if (!SafeWrite(addr, newVal, size)) return;

            slot.addr = addr;

            slot.size = size;

            s.count++;

        };



        // Helper: write our quaternion at `addr` ONLY IF the existing 16

        // bytes already decode to a plausible unit quaternion AND the

        // owner-object's klass pointer (if `klassAnchor` provided) is

        // canonical-and-stable. Returns true on write.

        auto safeWriteQuatIfQuat = [&](uintptr_t addr, uintptr_t klassAnchor) -> bool {

            if (!IsCanonicalUserPtr(addr)) return false;

            if (klassAnchor) {

                uintptr_t k1 = 0, k2 = 0;

                if (!SafeReadPtr(klassAnchor, &k1) || !IsCanonicalUserPtr(k1))

                    return false;

                if (!SafeReadPtr(klassAnchor, &k2) || k1 != k2)

                    return false;

            }

            float cur[4];

            if (!SafeRead(addr, cur, 16)) return false;

            for (int i = 0; i < 4; i++) {

                if (!(cur[i] == cur[i]) || cur[i] < -1.5f || cur[i] > 1.5f) return false;

            }

            float m2 = cur[0]*cur[0] + cur[1]*cur[1] + cur[2]*cur[2] + cur[3]*cur[3];

            if (m2 < 0.90f || m2 > 1.10f) return false;

            if (fabsf(cur[3]) < 0.01f) return false; // rotations rarely have w≈0

            saveAndWrite(addr, q, 16);

            return true;

        };



        // 1. PlayerInput.bodyAngles  (Vector3 Euler, server validation).

        //    The ONLY PlayerInput field we touch. Earlier "candidate" probes

        //    at +0x34/+0x64/+0xFC are deliberately removed — see header

        //    comment: those slots have unverified semantics and matched a

        //    long-standing crash pattern when target visibility flickered.

        uintptr_t input = 0;

        if (SafeReadPtr(localPlayer + offsets::BasePlayer::playerInput, &input)

            && IsCanonicalUserPtr(input))

        {

            // Verify the PlayerInput object's own klass pointer is canonical

            // before writing into its body. Same torn-state filter as for

            // localPlayer above.

            uintptr_t ipKlass1 = 0, ipKlass2 = 0;

            if (SafeReadPtr(input, &ipKlass1) && IsCanonicalUserPtr(ipKlass1)

                && SafeReadPtr(input, &ipKlass2) && ipKlass1 == ipKlass2)

            {

                saveAndWrite(input + offsets::PlayerInput::bodyAngles, v3Euler, 12);

            }

        }



        // 2. PlayerEyes.bodyRotation  (Quaternion, drives client-side raycast).

        //    Only this single quaternion slot is written. The previous

        //    `unkQuanternion @0x6C` write is gone; its semantics were

        //    unverified and it was the prime suspect for the crash that

        //    fires when the target's model gets occluded by a Minicopter.

        //

        //    `safeWriteQuatIfQuat` fails closed if the eyes object has been

        //    re-allocated (klass pointer torn) — silent aim simply skips

        //    the write for that shot rather than risking a corrupted-state

        //    crash in the game's animation/IK code.

        uintptr_t eyesComp = ResolveEyesDirect(localPlayer);

        if (eyesComp == 0) eyesComp = ResolveEyesPtr(localPlayer);

        if (eyesComp != 0) {

            safeWriteQuatIfQuat(eyesComp + offsets::PlayerEyes::bodyRotation,

                                /*klassAnchor=*/eyesComp);

        }



        // Diagnostic: log every silent engagement, throttled to one line

        // per 500ms so bursts don't spam. Include the target pitch/yaw,

        // target world position, and the computed Quaternion so the math

        // can be verified visually in the log.

        static unsigned long long lastLogTick = 0;

        unsigned long long nowTick = GetTickCount64();

        if (s.count > 0 && (nowTick - lastLogTick) >= 500) {

            lastLogTick = nowTick;

            // Build a short list of which slot addresses we actually wrote.

            char slotList[256] = {};

            int slPos = 0;

            for (int i = 0; i < s.count && slPos < (int)sizeof(slotList) - 20; i++) {

                int off = -1;

                if (eyesComp && s.slots[i].addr >= eyesComp && s.slots[i].addr < eyesComp + 0x200) {

                    off = (int)(s.slots[i].addr - eyesComp);

                    slPos += snprintf(slotList + slPos, sizeof(slotList) - slPos,

                                      " E+0x%X", off);

                } else if (input && s.slots[i].addr >= input && s.slots[i].addr < input + 0x200) {

                    off = (int)(s.slots[i].addr - input);

                    slPos += snprintf(slotList + slPos, sizeof(slotList) - slPos,

                                      " I+0x%X", off);

                }

            }

            AimLog("[RH][aim] SILENT write count=%d eyes=%p input=%p "

                   "pitch=%.2f yaw=%.2f tgtPos=(%.2f,%.2f,%.2f) "

                   "quat=(%.3f,%.3f,%.3f,%.3f) slots=[%s]",

                   s.count, (void*)eyesComp, (void*)input,

                   currentPitch, currentYaw,

                   targetPosX, targetPosY, targetPosZ,

                   q[0], q[1], q[2], q[3],

                   slotList[0] ? slotList : "");

        }



        return s;

    }



    static void EndSilentAngles(const SilentState& s) {

        for (int i = 0; i < s.count; i++) {

            SafeWrite(s.slots[i].addr, s.slots[i].oldBytes, s.slots[i].size);

        }

    }



    // Decide whether this shot should be silently redirected to the locked

    // target. Called from inside shot hooks — the shot IS happening, so

    // we don't need to check LMB (semi-auto users release before the hook

    // runs, causing spurious misses). Only require aim+silent toggles and

    // a valid locked target.

    static bool ShouldSilentThisShot() {

        auto& cfg = Menu::Get().AimCfg;

        // PerfectSilent is a standalone feature — doesn't need Aimbot toggle.

        if (!cfg.PerfectSilent) return false;

        if (currentTarget == 0) return false;

        if (cfg.VisibleCheck && !currentTargetVisible) return false;

        return true;

    }



    // Detour for the virtual method at klass+0x3F08 (pirated build) or

    // klass+0x4348 (official). Called once per projectile inside the

    // LaunchProjectile inner loop. Takes a Vector3 inputDir and returns a

    // pointer to a Vector3 outDir; the caller immediately reads 12 bytes

    // from outDir and stores them as projectile.modifiedDirection (which

    // is what the network serializer reads). See sub_184038C60 decompile.

    static volatile unsigned long long g_projDirVMRawHits = 0;



    __int64 __fastcall hkProjDirVM(void* outBuf, void* thisPtr, __int64 item,

                                   float* inputDir, void* methodInfo) {

        unsigned long long rawHits = ++g_projDirVMRawHits;

        DWORD tid = GetCurrentThreadId();



        // IMPORTANT: do NOT call oProjDirVM here. Under the HW BP / VEH

        // hook path, invoking the original through oProjDirVM re-triggers

        // the same breakpoint on the same thread, causing infinite

        // recursion and a hang on the 4-5th shot (observed in user log).

        // Instead we fill outBuf ourselves — the caller (sub_184038C60)

        // only reads the first 12 bytes (Vector3 direction) of the 40-byte

        // buffer; the rest is unused.

        //

        // Behaviour summary:

        //   * silent aim active + we're inside LaunchProjectile

        //         -> outBuf = (target - eye).normalized

        //   * otherwise (no silent, wrong thread, etc.)

        //         -> outBuf = inputDir unchanged (identity; no spread)

        //

        // This means spread is implicitly removed from every shot that

        // hits the vmethod — which is desirable for a cheat (and matches

        // the NoSpread feature that's typically on anyway).



        if (!outBuf) return 0;



        // Default output = passthrough of inputDir.

        float dir[3] = { 0.0f, 0.0f, 0.0f };

        if (inputDir && IsCanonicalUserPtr((uintptr_t)inputDir)) {

            SafeRead((uintptr_t)inputDir, dir, 12);

        }



        bool overrode = false;

        if (ShouldSilentThisShot() &&

            g_launchThreadId == tid &&

            localPlayer != 0 && currentTarget != 0)

        {

            float dx = targetPosX - localEyeX;

            float dy = targetPosY - localEyeY;

            float dz = targetPosZ - localEyeZ;

            float len = sqrtf(dx*dx + dy*dy + dz*dz);

            if (len >= 0.01f) {

                float inv = 1.0f / len;

                dir[0] = dx * inv;

                dir[1] = dy * inv;

                dir[2] = dz * inv;

                overrode = true;

            }

        }



        bool wrote = SafeWrite((uintptr_t)outBuf, dir, 12);



        if ((rawHits % 10ULL) == 1ULL || overrode) {

            static unsigned long long lastLog = 0;

            if (overrode ? (++lastLog % 3ULL) == 1ULL : true) {

                AimLog("[RH][aim] hkProjDirVM #%llu tid=%u launchTid=%u "

                       "override=%d dir=(%.3f,%.3f,%.3f) ok=%d",

                       rawHits, (unsigned)tid, (unsigned)g_launchThreadId,

                       (int)overrode, dir[0], dir[1], dir[2], (int)wrote);

            }

        }



        return (__int64)outBuf;

    }



    // One-shot installer called from inside hkLaunchProjectile on first

    // entry. Reads klass from weapon, tries vtable slots 0x3F08 (pirated,

    // primary) then 0x4348 (official, fallback), verifies the target bytes

    // look like a real x64 prolog, then installs the hook.

    static bool HookFunctionRobust(void* fnPtr, void* hook, void** origOut, const char* label); // fwd



    static bool LooksLikeX64Prologue(const unsigned char* b) {

        return (b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x5C && b[3] == 0x24) || // mov [rsp+X], rbx

               (b[0] == 0x40 && b[1] == 0x53)                                 || // push rbx (rex)

               (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0xC4)                 || // mov rax, rsp

               (b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x4C && b[3] == 0x24) || // mov [rsp+X], rcx

               (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC)                 || // sub rsp, X

               (b[0] == 0x4C && b[1] == 0x89 && b[2] == 0x44 && b[3] == 0x24) || // mov [rsp+X], r8

               (b[0] == 0x55) || (b[0] == 0x56) || (b[0] == 0x57) ||             // push rbp/rsi/rdi

               (b[0] == 0x53);                                                    // push rbx

    }



    static void TryInstallProjDirVMHook(void* projectile) {

        if (g_projDirVMTried) return;

        g_projDirVMTried = true;

        if (!projectile) return;

        uintptr_t klass = 0;

        if (!SafeReadPtr((uintptr_t)projectile, &klass) || !IsCanonicalUserPtr(klass)) {

            AimLog("[RH][aim] ProjDirVM install: bad klass from proj=%p", projectile);

            return;

        }



        static const uintptr_t kVmOffsets[] = { 0x3F08, 0x4348 };

        for (uintptr_t off : kVmOffsets) {

            uintptr_t vm = 0;

            if (!SafeReadPtr(klass + off, &vm) || !IsCanonicalUserPtr(vm)) {

                AimLog("[RH][aim] ProjDirVM: klass+0x%llX -> unreadable/non-canon",

                       (unsigned long long)off);

                continue;

            }

            unsigned char bytes[16] = {0};

            bool readOk = SafeRead(vm, bytes, 16);

            bool plausible = readOk && LooksLikeX64Prologue(bytes);

            char line[96] = {}; int pos = 0;

            if (readOk) {

                for (int i = 0; i < 16; i++)

                    pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[i]);

            }

            AimLog("[RH][aim] ProjDirVM: klass=%p +0x%llX -> %p plausible=%d bytes=%s",

                   (void*)klass, (unsigned long long)off, (void*)vm,

                   (int)plausible, line);

            if (!plausible) continue;



            char label[64];

            _snprintf_s(label, sizeof(label), "ProjDirVM@+0x%llX",

                        (unsigned long long)off);

            if (HookFunctionRobust((void*)vm, (void*)&hkProjDirVM,

                                   (void**)&oProjDirVM, label)) {

                g_projDirVMAddr = vm;

                g_projDirVMOffset = off;

                AimLog("[RH][aim] ProjDirVM: ✓ hooked at klass+0x%llX fn=%p",

                       (unsigned long long)off, (void*)vm);

                return;

            }

        }

        AimLog("[RH][aim] ProjDirVM: all candidate vtable slots failed");

    }



    // Zero all spread fields on a weapon. Called BEFORE the original

    // DoAttack so the aim-cone randomization reads 0.

    //

    // GetAimCone() roughly does:

    //   aimCone + hipAimCone + aimconePenalty * stancePenaltyScale +

    //   burstScale + ...

    // So zeroing the multipliers (*Scale) is equivalent to zeroing the

    // runtime accumulators we can't locate (aimconePenalty). The

    // SimulateAimcone hook also prevents penalty from growing over time.

    static void ZeroConeFields(uintptr_t weapon) {

        if (!IsCanonicalUserPtr(weapon)) return;

        DirectWriteFloat(weapon + aimConeOffset,           0.0f);

        DirectWriteFloat(weapon + hipAimConeOffset,        0.0f);

        DirectWriteFloat(weapon + aimConePenaltyMaxOffset, 0.0f);

        DirectWriteFloat(weapon + aimPenaltyPerShotOffset, 0.0f);

        // stancePenaltyScale @ 0x3D0 — multiplier on stance spread

        // contribution. Zero it to kill the stance-based spread component

        // even without knowing where the raw stancePenalty runtime is.

        DirectWriteFloat(weapon + stancePenaltyScaleOffset, 0.0f);

        // internalBurstAimConeScale @ 0x3EC — spread scale used inside

        // burst-mode shots. Zero it so burst weapons also get no spread.

        DirectWriteFloat(weapon + 0x3EC,                    0.0f);

        // Runtime-accumulated penalty (unresolved offset on this build):

        if (aimconePenaltyOffset != 0)

            DirectWriteFloat(weapon + aimconePenaltyOffset, 0.0f);

        if (stancePenaltyOffset != 0)

            DirectWriteFloat(weapon + stancePenaltyOffset, 0.0f);



        // Targeted zeroing of KNOWN spread / penalty / scale multipliers.

        //

        // ============================================================

        // CRITICAL HISTORY — DO NOT REVERT TO BLIND-SWEEP.

        // ============================================================

        // Earlier versions blind-swept 0x3A0..0x420 and zeroed any 4-byte

        // float in the range [0.001, 20.0]. That caused crashes when the

        // player stepped behind cover mid-burst: the cover-induced weapon

        // state transition wrote a *non-multiplier* timing constant

        // (observed value 2.7436) to WPN+0x3F8 — likely

        // `attackLengthMax`, `recoilCompensationDelay`, or a similar

        // animation/timer field. Our filter passed it, we zeroed it,

        // game's `oLaunchProjectile` divided by it / used it as a lerp

        // duration, NaN propagated into Quaternion.LookRotation, hard

        // crash a few shots later.

        //

        // Fix: switch to an EXPLICIT ALLOWLIST of offsets that have been

        // verified across multiple sessions to be spread multipliers.

        // Anything we haven't positively identified as a spread/penalty

        // scale is left alone. New offsets get added to this list ONLY

        // after both:

        //   (a) we've observed the value is a small constant in [0.05, 1.5]

        //       across firing AND idle frames (timing constants change

        //       over time; multipliers don't), AND

        //   (b) zeroing it produces no observable side-effect after

        //       sustained firing (>200 shots) including movement and

        //       cover transitions.

        //

        // The named-field writes above (aimCone/hipAimCone/penaltyMax/

        // perShot/stancePenaltyScale/0x3EC) are RESOLVED via il2cpp

        // metadata so they're always correct for the current build —

        // those stay. The list below is the residual sweep set we

        // discovered empirically.

        //

        // KNOWN-SAFE allowlist:

        //   0x3C8 -> 0.3   (resolved: aim-cone scaling pair component)

        //   0x3CC -> 0.3   (resolved: aim-cone scaling pair component)

        //   0x3E4 -> 0.8   (resolved: stance/move-cone scale)

        //   0x3E8 -> 0.8   (resolved: stance/move-cone scale)

        //   0x3F0 -> 0.3   (resolved: aim-cone bonus multiplier)

        //   0x414 -> 0.8   (resolved: ADS spread multiplier)

        //   0x41C -> 1.0   (resolved: ADS spread base multiplier)

        //

        // KNOWN-UNSAFE (do NOT add to allowlist):

        //   0x3A8 -> recoilProperties* (8B pointer)

        //   0x3B0 -> aimconeCurve* (8B pointer)

        //   0x3F8 -> 2.7436 timing constant (CRASH SOURCE — see history)

        //   0x420+ -> ADS / scope / sway state.

        //

        // We still bulk-read+bulk-write the block in TWO syscalls so the

        // I/O cost stays at ~400ns total per shot, regardless of how many

        // offsets are in the allowlist. Bulk I/O via SafeRead/SafeWrite

        // is non-faulting (WPM/RPM on self-handle return false on bad

        // pages, never raise SEH) — required because manual-map APC has

        // no PDATA registered, so __try/__except is a no-op.

        constexpr int kSweepStart = 0x3A0;

        constexpr int kSweepEnd   = 0x420;

        constexpr int kSweepLen   = kSweepEnd - kSweepStart; // 0x80 bytes

        uint8_t buf[kSweepLen] = {};

        if (!SafeRead(weapon + (uintptr_t)kSweepStart, buf, kSweepLen)) {

            // Weapon pointer went invalid between our IsCanonicalUserPtr

            // check and the read — bail out cleanly, no zeros applied.

            return;

        }



        static const int kKnownSpreadOffsets[] = {

            0x3C8, 0x3CC,

            0x3E4, 0x3E8,

            0x3F0,

            0x414, 0x41C

        };

        static bool zeroedOffsetSeen[0x100] = {};  // indexed by (off - 0x3A0)/4

        bool dirty = false;

        for (int off : kKnownSpreadOffsets) {

            int localOff = off - kSweepStart;

            if (localOff < 0 || localOff + 4 > kSweepLen) continue;

            float v;

            memcpy(&v, buf + localOff, 4);

            // Defensive sanity: even on a known offset, only zero if the

            // current value LOOKS like a multiplier. If the game has

            // re-purposed the offset on this build (denormal, NaN, huge,

            // pointer-fragment), skip it. Tightened range [0.05, 1.5]

            // because all observed multipliers across multiple sessions

            // sit in [0.3, 1.0]; values outside that are by definition

            // not what we expect this offset to hold.

            uint32_t bits;

            memcpy(&bits, &v, 4);

            uint32_t exp = (bits >> 23) & 0xFF;

            if (exp == 0)    continue;

            if (exp == 0xFF) continue;

            if (v < 0.05f || v > 1.5f) continue;

            float zero = 0.0f;

            memcpy(buf + localOff, &zero, 4);

            dirty = true;

            int idx = localOff / 4;

            if (idx >= 0 && idx < 0x100 && !zeroedOffsetSeen[idx]) {

                zeroedOffsetSeen[idx] = true;

                AimLog("[RH][aim] SWEEP zero WPN+%03X was=%.4f", off, v);

            }

        }



        // Single bulk write-back. If the weapon got unmapped since the

        // read, SafeWrite returns false and we silently drop the update

        // (the next shot will retry).

        if (dirty) {

            SafeWrite(weapon + (uintptr_t)kSweepStart, buf, kSweepLen);

        }

    }



    // Hook for PlayerEyes::BodyForward() -> Vector3. x64 Windows ABI for a

    // Vector3-returning method: hidden pointer to retval in RCX, followed

    // by `this` in RDX, MethodInfo in R8. We always call the original first

    // (so the normal path has a sane default if our override is inactive),

    // then stomp *retval with the target direction if the guard says so.

    // Helper for the BodyForward / HeadForward override: produce a sane

    // (target - eye) unit vector, or return false if the result would be

    // NaN / Inf / zero-length. NEVER write a zero or NaN direction back

    // through the retval pointer — Unity's downstream Quaternion.LookRotation

    // crashes on degenerate inputs.

    static bool BuildSilentForwardDir(float outDir[3]) {

        float dx = targetPosX - localEyeX;

        float dy = targetPosY - localEyeY;

        float dz = targetPosZ - localEyeZ;

        if (!(dx == dx) || !(dy == dy) || !(dz == dz)) return false;

        float len2 = dx*dx + dy*dy + dz*dz;

        if (!(len2 >= 1e-6f) || !(len2 <= 1e12f)) return false;

        float inv = 1.0f / sqrtf(len2);

        outDir[0] = dx * inv;

        outDir[1] = dy * inv;

        outDir[2] = dz * inv;

        for (int i = 0; i < 3; i++) {

            if (!(outDir[i] == outDir[i])) return false;

        }

        return true;

    }



    void __fastcall hkBodyForward(void* retval, void* self, void* mi,

                                  void* u1, void* u2) {

        if (oBodyForward) oBodyForward(retval, self, mi, u1, u2);

        // Only hijack during an active shot with silent aim engaged.

        if (g_launchThreadId != GetCurrentThreadId()) return;

        if (!ShouldSilentThisShot()) return;

        if (!retval || !IsCanonicalUserPtr((uintptr_t)retval)) return;



        float dir[3];

        if (!BuildSilentForwardDir(dir)) return;



        static unsigned long long bfHits = 0;

        if ((++bfHits % 10ULL) == 1ULL) {

            float orig[3] = {0};

            SafeRead((uintptr_t)retval, orig, sizeof(orig));

            AimLog("[RH][aim] hkBodyForward#%llu: orig=(%.3f,%.3f,%.3f) "

                   "-> dir=(%.3f,%.3f,%.3f)",

                   bfHits, orig[0], orig[1], orig[2], dir[0], dir[1], dir[2]);

        }

        // SafeWrite (RPM/WPM on self handle) — non-faulting if `retval`

        // turned out to be a dangling stack pointer mid-unwind. Direct

        // *(Vec3*)retval = dir would AV the game.

        SafeWrite((uintptr_t)retval, dir, sizeof(dir));

    }



    // HeadForward is similar — some codepaths of LaunchProjectile use head

    // (camera) direction instead of body direction. Same override logic.

    void __fastcall hkHeadForward(void* retval, void* self, void* mi,

                                  void* u1, void* u2) {

        if (oHeadForward) oHeadForward(retval, self, mi, u1, u2);

        if (g_launchThreadId != GetCurrentThreadId()) return;

        if (!ShouldSilentThisShot()) return;

        if (!retval || !IsCanonicalUserPtr((uintptr_t)retval)) return;



        float dir[3];

        if (!BuildSilentForwardDir(dir)) return;



        static unsigned long long hfHits = 0;

        if ((++hfHits % 10ULL) == 1ULL) {

            float orig[3] = {0};

            SafeRead((uintptr_t)retval, orig, sizeof(orig));

            AimLog("[RH][aim] hkHeadForward#%llu: orig=(%.3f,%.3f,%.3f) "

                   "-> dir=(%.3f,%.3f,%.3f)",

                   hfHits, orig[0], orig[1], orig[2], dir[0], dir[1], dir[2]);

        }

        SafeWrite((uintptr_t)retval, dir, sizeof(dir));

    }



    // NOTE: hkDoAttack is kept as a backup shim only. In this build the

    // active shot hook is LaunchProjectile (DoAttack runs per-tick even

    // when not firing and doesn't actually build the projectile).

    void __fastcall hkDoAttack(void* a0, void* a1, void* a2, void* a3, void* a4) {

        if (!oDoAttack) return;

        auto& wcfg = Menu::Get().WeaponCfg;

        uintptr_t weapon = (uintptr_t)a0;

        if (wcfg.NoSpread) ZeroConeFields(weapon);

        oDoAttack(a0, a1, a2, a3, a4);

    }



    // ============================================================

    //  Projectile.Create hook — TRUE silent aim chokepoint

    // ============================================================

    // sub_184012E20 in pirated build (RVA 0x4012E20). All bullet

    // creation paths (sub_184014840, sub_184016FC0, sub_184036480,

    // sub_184037860, sub_184038C60, sub_18403ECD0 — every LaunchProjectile

    // variant) end up calling this. Signature observed in IDA decompile:

    //

    //   __int64 __fastcall sub_184012E20(

    //       __int64  a1,        // weapon (BaseProjectile / ItemMod)

    //       __int64  a2,        // ammo Item

    //       float*   a3,        // &position (Vector3)

    //       float*   a4,        // &direction (Vector3) <-- WE OVERWRITE

    //       float*   a5,        // &velocity (Vector3, length = speed)

    //       __int64  /*mi*/);   // IL2CPP MethodInfo*

    //

    // Inside the function:

    //   v13 = *a4;                                          // load Vector3

    //   v14(&v28, &v26, &v30); // Quaternion.LookRotation_Injected(direction, up, &outQuat)

    //   v19 = sub_184FB7A00(prefab, ammo, position, &v30);  // Instantiate

    //

    // So if *a4 is replaced with (target - eye).normalized BEFORE the

    // original runs, the spawned projectile's facing quaternion (and the

    // velocity vector that the network packet serializes) point at the

    // target — the camera and aim angles are never touched.

    //

    // We also rescale a5 (velocity) to preserve magnitude with the new

    // direction. Velocity = direction * speed in original code (lines

    // 581-583 of sub_184038C60), so we recompute a5 = newDir * |a5|.

    typedef __int64 (__fastcall* tProjCreate)(void* weapon, void* ammo,

                                              float* position, float* direction,

                                              float* velocity, void* mi);

    static tProjCreate oProjectileCreate = nullptr;

    static void*       g_projCreateAddr = nullptr;

    bool               HasProjCreateHook = false;

    static volatile unsigned long long g_projCreateRaw = 0;

    static volatile unsigned long long g_projCreateOverride = 0;



    // ===================================================================

    // Shared solver: compute new direction + velocity for silent aim.

    // Returns true if the shot should be overridden.

    //

    // Why this is a standalone helper (and not inline in hkProjectileCreate):

    // the protobuf packet that reports the shot to the server is serialized

    // from CALLEE-SAVED XMM registers (xmm6..xmm15) in the caller function

    // — NOT from the `*velocity` memory pointer that Projectile.Create

    // receives. Our C++ detour can only modify the memory; the XMM caches

    // survive the call intact (ABI guarantees). So we need a VEH preHook

    // that patches BOTH memory AND registers. Both sites share this solver.

    // ===================================================================

    // ---- Ammo ballistics auto-resolve ---------------------------------

    //

    // The Projectile prototype passed as `ammo` to Projectile.Create has

    // PUBLIC, NON-OBFUSCATED fields `gravityModifier` and `drag` (verified

    // via dnSpy). We resolve the offsets via il2cpp metadata once per

    // distinct klass and cache them; the values themselves are read fresh

    // every shot from the live ammo instance, so a player switching from

    // a rifle to a bow gets correct compensation immediately.

    //

    // Why this matters: the previous solver used a SINGLE config-driven

    // GravityScale (default 0.10) that was a guess at typical rifle ammo.

    // Long-range shots (>200m) drift into legs/pelvis because:

    //   (a) The config 0.10 happened to be lower than the real rifle

    //       gravityModifier (~0.13–0.18 depending on ammo).

    //   (b) Drag was completely ignored — bullets decelerate over flight

    //       and at 250m+ that's a meaningful extra ~10–15% time-of-flight

    //       which compounds into 20-30cm of additional vertical drop.

    //

    // We now read both per-shot from the actual ammo prototype.

    struct AmmoBallistics {

        float gravityMod;   // unitless, multiplier of Physics.gravity (9.81)

        float drag;         // unitless, exponential decay rate per second

        bool  resolved;     // true iff at least gravityMod was read live

    };



    static AmmoBallistics ResolveAmmoBallistics(void* ammo) {

        AmmoBallistics out = { 0.10f /*conservative fallback*/, 0.0f, false };

        if (!ammo || !IsCanonicalUserPtr((uintptr_t)ammo)) return out;

        uintptr_t klass = 0;

        if (!SafeReadPtr((uintptr_t)ammo, &klass) || !IsCanonicalUserPtr(klass))

            return out;



        // Cache field offsets keyed by klass — a single Projectile-derived

        // klass always has the same layout for its own fields, so resolving

        // once per klass is enough. Different ammo types may share or

        // differ in klass; we re-resolve when klass changes.

        static uintptr_t s_lastKlass = 0;

        static int       s_gravOff   = -1;

        static int       s_dragOff   = -1;

        if (klass != s_lastKlass) {

            s_lastKlass = klass;

            s_gravOff   = -1;

            s_dragOff   = -1;

            if (il2cpp.class_get_field_from_name && il2cpp.field_get_offset) {

                if (auto f = il2cpp.class_get_field_from_name(

                                (Il2CppClass*)klass, "gravityModifier")) {

                    s_gravOff = il2cpp.field_get_offset(f);

                }

                if (auto f = il2cpp.class_get_field_from_name(

                                (Il2CppClass*)klass, "drag")) {

                    s_dragOff = il2cpp.field_get_offset(f);

                }

            }

            AimLog("[RH][aim] ammo klass=%p resolved fields: grav@0x%X drag@0x%X",

                   (void*)klass, s_gravOff, s_dragOff);

        }



        if (s_gravOff > 0) {

            float v = 0.0f;

            if (SafeRead((uintptr_t)ammo + (uintptr_t)s_gravOff, &v, 4)

                && (v == v) /*not NaN*/

                && v >= 0.0f && v <= 5.0f /*sane bound*/) {

                out.gravityMod = v;

                out.resolved   = true;

            }

        }

        if (s_dragOff > 0) {

            float v = 0.0f;

            if (SafeRead((uintptr_t)ammo + (uintptr_t)s_dragOff, &v, 4)

                && (v == v)

                && v >= 0.0f && v <= 5.0f) {

                out.drag = v;

            }

        }

        return out;

    }



    // Diagnostic stash for the raw (unscaled) gravity multiplier and drag

    // value used by the most recent SolveProjectileBallistic call. Read by

    // ProjectileCreatePreHook so the per-shot log line can show them.

    static float g_lastSolvedGravMod = 0.0f;

    static float g_lastSolvedDrag    = 0.0f;



    // Drag-aware time-of-flight for a bullet that travels distance `dist`

    // starting at speed `v0` while velocity decays exponentially as

    //     v(t) = v0 * exp(-drag * t)

    // Integrating yields traveled distance

    //     d(t) = (v0 / drag) * (1 - exp(-drag * t))

    // Inverting:

    //     t    = -ln(1 - d * drag / v0) / drag

    // For drag→0 we fall back to the trivial t = d / v0.

    // For drag so large that the bullet would never reach `dist`, we

    // clamp to t = d / v0 (the linear approximation) instead of returning

    // NaN — the override-safety check in the solver will catch that case.

    static inline float DragAwareFlightTime(float dist, float v0, float drag) {

        if (!(dist > 0.0f) || !(v0 > 0.001f)) return 0.0f;

        if (drag < 0.001f) return dist / v0;

        float arg = 1.0f - dist * drag / v0;

        if (!(arg > 0.001f)) return dist / v0;  // unreachable / clamp

        return -logf(arg) / drag;

    }



    static bool SolveProjectileBallistic(

        void*   weapon,          // in (for logging only)

        void*   ammo,            // in: Projectile prototype — read grav/drag

        float*  position,        // in: pointer to muzzle position Vector3 (may be null)

        float*  direction,       // in: pointer to original direction Vector3

        float*  velocity,        // in: pointer to original velocity Vector3 (may be null)

        float   outOrigDir[3],

        float   outOrigVel[3],

        float   outOrigPos[3],

        float   outNewDir[3],

        float   outNewVel[3],

        float*  outSpeed,

        float*  outDist,

        float*  outFlightT,

        float*  outDrop,

        float*  outGravity,

        float   outPred[3])

    {

        auto& acfg = Menu::Get().AimCfg;

        const bool wantPredict  = true;

        const bool wantGravComp = acfg.GravityScale > 0.001f;

        if (!wantPredict && !wantGravComp) return false;



        if (!ShouldSilentThisShot() || localPlayer == 0 || currentTarget == 0)

            return false;

        if (!direction || !IsCanonicalUserPtr((uintptr_t)direction))

            return false;



        // Read args from memory.

        if (!SafeRead((uintptr_t)direction, outOrigDir, 12)) return false;

        // NaN-guard the original direction — if the game hasn't initialised

        // it yet (happens on rare close-range spawn races), a direct

        // memcpy-write of derived values would propagate NaN to the

        // network packet and can crash the game's LookRotation.

        for (int i = 0; i < 3; i++) {

            if (!(outOrigDir[i] == outOrigDir[i])) return false; // NaN check

        }

        float bulletSpeed = 0.0f;

        if (velocity && IsCanonicalUserPtr((uintptr_t)velocity)

            && SafeRead((uintptr_t)velocity, outOrigVel, 12)) {

            // NaN-guard velocity components too.

            bool velOk = true;

            for (int i = 0; i < 3; i++) {

                if (!(outOrigVel[i] == outOrigVel[i])) { velOk = false; break; }

            }

            if (velOk) {

                bulletSpeed = sqrtf(outOrigVel[0]*outOrigVel[0]

                                  + outOrigVel[1]*outOrigVel[1]

                                  + outOrigVel[2]*outOrigVel[2]);

            }

        }

        if (!(bulletSpeed >= 10.0f) || bulletSpeed > 50000.0f) bulletSpeed = 300.0f;



        auto& wcfgFB = Menu::Get().WeaponCfg;

        const float fbFactor = (wcfgFB.FastBullet && wcfgFB.FastBulletSpeed > 1.01f)

                             ? wcfgFB.FastBulletSpeed : 1.0f;

        bulletSpeed *= fbFactor;



        bool havePos = false;

        if (position && IsCanonicalUserPtr((uintptr_t)position)) {

            if (SafeRead((uintptr_t)position, outOrigPos, 12)) {

                havePos = true;

                for (int i = 0; i < 3; i++) {

                    if (!(outOrigPos[i] == outOrigPos[i])) { havePos = false; break; }

                }

            }

        }

        float px = havePos ? outOrigPos[0] : localEyeX;

        float py = havePos ? outOrigPos[1] : localEyeY;

        float pz = havePos ? outOrigPos[2] : localEyeZ;



        float tx = targetPosX, ty = targetPosY, tz = targetPosZ;

        // Target must be a finite world position — a NaN target (e.g. a

        // race where ESP cleared currentTarget mid-write) would silently

        // produce a NaN direction. Bail out instead.

        if (!(tx == tx) || !(ty == ty) || !(tz == tz)) return false;



        float vx = wantPredict ? targetVelX : 0.0f;

        float vy = wantPredict ? targetVelY : 0.0f;

        float vz = wantPredict ? targetVelZ : 0.0f;

        // Prediction velocity NaN-guard.

        if (!(vx == vx)) vx = 0.0f;

        if (!(vy == vy)) vy = 0.0f;

        if (!(vz == vz)) vz = 0.0f;



        // Resolve per-shot ammo ballistics (gravityModifier, drag) —

        // ALWAYS read live from the Projectile prototype. The previous

        // AutoGravity / ApplyDrag toggles were removed (forced on); the

        // user-facing GravityScale slider now acts purely as a fine-tune

        // multiplier on top of the auto-detected gravity (default 1.0).

        AmmoBallistics ab = ResolveAmmoBallistics(ammo);

        const float kGravity = wantGravComp

                             ? 9.81f * ab.gravityMod * acfg.GravityScale

                             : 0.0f;

        const float drag = wantGravComp ? ab.drag : 0.0f;

        // Stash the raw resolved values so the calling pre-hook can log

        // them alongside its other diagnostics (avoids growing the

        // already-long solver signature).

        g_lastSolvedGravMod = ab.gravityMod;

        g_lastSolvedDrag    = drag;



        float dist;

        {

            float ddx = tx - px, ddy = ty - py, ddz = tz - pz;

            dist = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);

        }

        // If target is directly on top of the muzzle (|dist|<1cm) or dist

        // is NaN, the direction cannot be computed — passthrough.

        if (!(dist >= 0.01f)) return false;



        float t = DragAwareFlightTime(dist, bulletSpeed, drag);



        auto DropAt = [&](float tt) -> float {

            if (!(tt > 0.0f) || !(kGravity > 0.0f)) return 0.0f;

            if (drag < 0.001f) return 0.5f * kGravity * tt * tt;

            float gd = kGravity / drag;

            return gd * tt - (gd / drag) * (1.0f - expf(-drag * tt));

        };



        float aimX = tx, aimY = ty, aimZ = tz;

        float prevT = t;

        for (int iter = 0; iter < 12; iter++) {

            float fx = tx + vx * t;

            float fy = ty + vy * t;

            float fz = tz + vz * t;

            float dropCur = DropAt(t);

            aimX = fx;

            aimY = fy + dropCur;

            aimZ = fz;

            float ddx = aimX - px, ddy = aimY - py, ddz = aimZ - pz;

            float nd = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);

            if (!(nd >= 0.01f) || !(bulletSpeed >= 0.001f)) break;

            float newT = DragAwareFlightTime(nd, bulletSpeed, drag);

            if (!(newT <= 5.0f)) { t = 5.0f; break; }

            float deltaT = newT - prevT;

            if (deltaT < 0.0f) deltaT = -deltaT;

            t = newT;

            prevT = newT;

            if (deltaT < 0.0001f) break;

        }



        float ddx = aimX - px, ddy = aimY - py, ddz = aimZ - pz;

        float dlen = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);

        // Use inverted comparison so NaN returns false (NaN is NOT >=).

        if (!(dlen >= 0.01f)) return false;



        float inv = 1.0f / dlen;

        outNewDir[0] = ddx * inv;

        outNewDir[1] = ddy * inv;

        outNewDir[2] = ddz * inv;

        // Final NaN-guard on the computed direction — catches any

        // degenerate case that slipped through above.

        for (int i = 0; i < 3; i++) {

            if (!(outNewDir[i] == outNewDir[i])) return false;

        }



        // Sanity: refuse override if >30° off the original (cos(30°)=0.866).

        // Inverted comparison so NaN dot also bails out.

        float dot = outOrigDir[0]*outNewDir[0]

                  + outOrigDir[1]*outNewDir[1]

                  + outOrigDir[2]*outNewDir[2];

        if (!(dot >= 0.866f)) return false;



        outNewVel[0] = outNewDir[0] * bulletSpeed;

        outNewVel[1] = outNewDir[1] * bulletSpeed;

        outNewVel[2] = outNewDir[2] * bulletSpeed;



        if (outSpeed)    *outSpeed    = bulletSpeed;

        if (outDist)     *outDist     = dist;

        if (outFlightT)  *outFlightT  = t;

        if (outDrop)     *outDrop     = DropAt(t);

        if (outGravity)  *outGravity  = kGravity;

        outPred[0] = tx + vx * t;

        outPred[1] = ty + vy * t;

        outPred[2] = tz + vz * t;

        return true;

    }



    // ===================================================================

    // VEH preHook: runs INSIDE the HW-BP exception handler, right when the

    // CPU is about to execute Projectile.Create's first instruction. We

    // read the raw CPU state, run the ballistic solver, and patch:

    //

    //   (1) *direction and *velocity in memory — so Projectile.Create itself

    //       spawns the bullet with our modified vector.

    //   (2) CONTEXT.Xmm6..Xmm15 — because the callers of Projectile.Create

    //       cache velocity components in callee-saved XMM registers and

    //       later serialize them into the ProjectileShoot protobuf packet

    //       that the server replays. If we skip (2), server sees ORIGINAL

    //       velocity => rejects the shot => no damage.

    //

    // Implementation note for (2): we don't know which specific XMM reg the

    // caller picked (it varies by weapon path — 6 callers each with their

    // own register allocation). Instead we scan low-dword of Xmm6..Xmm15 and

    // replace any float that matches an original velocity component with

    // the new value. False-positive risk is negligible because (a) velocity

    // components are large floats in the 100-500 range, very unlikely to

    // collide with incidental register contents, and (b) we use a tight

    // match epsilon of 1e-6 relative to magnitude.

    // ===================================================================

    static void __stdcall ProjectileCreatePreHook(EXCEPTION_POINTERS* ep) {

        CONTEXT* ctx = ep->ContextRecord;



        void*  weapon   = (void*)ctx->Rcx;

        void*  ammo     = (void*)ctx->Rdx;

        float* position = (float*)ctx->R8;

        float* direction= (float*)ctx->R9;

        // Stack args at callee entry (right after CALL pushed return addr):

        //   [rsp+0x00] = return address

        //   [rsp+0x08..0x20] = shadow space for rcx/rdx/r8/r9

        //   [rsp+0x28] = arg5 (velocity pointer)

        //   [rsp+0x30] = arg6 (methodInfo)

        //

        // We're executing inside a VEH handler on the game thread — a page

        // fault here would recursively invoke our VEH (the original handler

        // won't match SINGLE_STEP / BREAKPOINT codes and returns CONTINUE_SEARCH),

        // eventually blowing the stack or crashing unhandled. Manual-map

        // APC-injected DLLs have no SEH unwind info registered so we CANNOT

        // use __try/__except here either. Use SafeRead (ReadProcessMemory

        // on self) which is guaranteed non-faulting.

        float* velocity = nullptr;

        if (ctx->Rsp) {

            uintptr_t slotAddr = (uintptr_t)ctx->Rsp + 0x28;

            if (IsCanonicalUserPtr(slotAddr)) {

                uintptr_t slotVal = 0;

                if (SafeRead(slotAddr, &slotVal, sizeof(slotVal))

                    && IsCanonicalUserPtr(slotVal))

                {

                    velocity = (float*)slotVal;

                }

            }

        }



        float origDir[3] = {0}, origVel[3] = {0}, origPos[3] = {0};

        float newDir[3]  = {0}, newVel[3]  = {0};

        float speed = 0, dist = 0, flightT = 0, drop = 0, kGrav = 0;

        float pred[3] = {0};

        bool ok = SolveProjectileBallistic(

            weapon, ammo, position, direction, velocity,

            origDir, origVel, origPos,

            newDir, newVel,

            &speed, &dist, &flightT, &drop, &kGrav, pred);



        ++g_projCreateRaw;



        auto& wcfgPH = Menu::Get().WeaponCfg;

        const float fbFact = (wcfgPH.FastBullet && wcfgPH.FastBulletSpeed > 1.01f)

                           ? wcfgPH.FastBulletSpeed : 1.0f;



        if (!ok) {

            if (fbFact > 1.01f && velocity && IsCanonicalUserPtr((uintptr_t)velocity)) {

                float vel[3] = {0};

                if (SafeRead((uintptr_t)velocity, vel, 12)) {

                    float boosted[3] = { vel[0]*fbFact, vel[1]*fbFact, vel[2]*fbFact };

                    SafeWrite((uintptr_t)velocity, boosted, 12);



                    M128A* xmms[] = {

                        &ctx->Xmm6, &ctx->Xmm7, &ctx->Xmm8, &ctx->Xmm9,

                        &ctx->Xmm10, &ctx->Xmm11, &ctx->Xmm12, &ctx->Xmm13,

                        &ctx->Xmm14, &ctx->Xmm15,

                    };

                    int patchedFB = 0;

                    for (M128A* xmm : xmms) {

                        float low = *(float*)xmm;

                        if (fabsf(low) < 1e-3f) continue;

                        for (int j = 0; j < 3; j++) {

                            if (fabsf(vel[j]) < 1e-3f) continue;

                            float eps = fabsf(vel[j]) * 1e-5f + 1e-6f;

                            if (fabsf(low - vel[j]) <= eps) {

                                *(float*)xmm = boosted[j];

                                patchedFB++;

                                goto next_xmm_fb;

                            }

                        }

                    next_xmm_fb:;

                    }

                    AimLog("[RH][aim] hkProjCreate FB-only raw=%llu factor=%.2f xmmPatch=%d",

                           (unsigned long long)g_projCreateRaw, fbFact, patchedFB);

                }

            } else {

                if ((g_projCreateRaw % 50ULL) == 1ULL) {

                    AimLog("[RH][aim] hkProjCreate raw=%llu silent=%d tgt=%p (passthrough)",

                           (unsigned long long)g_projCreateRaw,

                           (int)ShouldSilentThisShot(), (void*)currentTarget);

                }

            }

            return;

        }



        SafeWrite((uintptr_t)direction, newDir, 12);

        if (velocity) SafeWrite((uintptr_t)velocity, newVel, 12);



        M128A* xmms[] = {

            &ctx->Xmm6, &ctx->Xmm7, &ctx->Xmm8, &ctx->Xmm9,

            &ctx->Xmm10, &ctx->Xmm11, &ctx->Xmm12, &ctx->Xmm13,

            &ctx->Xmm14, &ctx->Xmm15,

        };

        int patched = 0;

        for (M128A* xmm : xmms) {

            float low = *(float*)xmm;

            if (fabsf(low) < 1e-3f) continue;

            for (int j = 0; j < 3; j++) {

                float refV = origVel[j];

                if (fabsf(refV) < 1e-3f) continue;

                float eps = fabsf(refV) * 1e-5f + 1e-6f;

                if (fabsf(low - refV) <= eps) {

                    *(float*)xmm = newVel[j];

                    patched++;

                    goto next_xmm;

                }

                float refD = origDir[j];

                if (fabsf(refD) >= 1e-3f) {

                    float epsD = fabsf(refD) * 1e-5f + 1e-6f;

                    if (fabsf(low - refD) <= epsD) {

                        *(float*)xmm = newDir[j];

                        patched++;

                        goto next_xmm;

                    }

                }

            }

        next_xmm:;

        }



        ++g_projCreateOverride;

        unsigned long long n = g_projCreateOverride;

        if ((n % 5ULL) == 1ULL) {

            AimLog("[RH][aim] hkProjCreate OVR#%llu raw=%llu wp=%p ammo=%p "

                   "speed=%.1f dist=%.1f t=%.3fs drop=%.2fm g=%.2f gMod=%.3f drag=%.3f "

                   "xmmPatched=%d fbFact=%.2f "

                   "muzzle=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) "

                   "tvel=(%.2f,%.2f,%.2f) pred=(%.2f,%.2f,%.2f) "

                   "orig=(%.3f,%.3f,%.3f) -> new=(%.3f,%.3f,%.3f)",

                   n, (unsigned long long)g_projCreateRaw, weapon, ammo,

                   speed, dist, flightT, drop, kGrav,

                   g_lastSolvedGravMod, g_lastSolvedDrag,

                   patched, fbFact,

                   origPos[0], origPos[1], origPos[2],

                   targetPosX, targetPosY, targetPosZ,

                   targetVelX, targetVelY, targetVelZ,

                   pred[0], pred[1], pred[2],

                   origDir[0], origDir[1], origDir[2],

                   newDir[0], newDir[1], newDir[2]);

        }

    }



    static int g_projectilesOff     = 0x20;

    static int g_proj_startPosOff   = 0x14;

    static int g_proj_startVelOff   = 0x20;

    static int g_proj_gravityModOff = -1;

    static int g_proj_dragOff       = -1;

    static int g_proj_thicknessOff   = -1;

    static int g_proj_curThicknessOff = -1;

    static bool g_projFieldsResolved = false;



    static void ResolveProjectileFields(Il2CppClass* klass) {

        if (!klass) return;

        if (il2cpp.class_get_field_from_name) {

            if (auto f = il2cpp.class_get_field_from_name(klass, "startPos")) {

                g_proj_startPosOff = il2cpp.field_get_offset(f);

            }

            if (auto f = il2cpp.class_get_field_from_name(klass, "startVel")) {

                g_proj_startVelOff = il2cpp.field_get_offset(f);

            }

            if (auto f = il2cpp.class_get_field_from_name(klass, "gravityModifier")) {

                g_proj_gravityModOff = il2cpp.field_get_offset(f);

            }

            if (auto f = il2cpp.class_get_field_from_name(klass, "drag")) {

                g_proj_dragOff = il2cpp.field_get_offset(f);

            }

            if (auto f = il2cpp.class_get_field_from_name(klass, "thickness")) {

                g_proj_thicknessOff = il2cpp.field_get_offset(f);

            }

            if (auto f = il2cpp.class_get_field_from_name(klass, "currentThickness")) {

                g_proj_curThicknessOff = il2cpp.field_get_offset(f);

            }

        }

        if (g_proj_curThicknessOff <= 0 && g_proj_thicknessOff == 0x3C) {

            g_proj_curThicknessOff = 0x158;

            AimLog("[RH][aim] Projectile.currentThickness obfuscated; using fallback 0x158 "

                   "(verified via Projectile.InitializeVelocity copy: a1+344 = a1+60)");

        }

        g_projFieldsResolved = true;

        AimLog("[RH][aim] Projectile fields: startPos=0x%X startVel=0x%X "

               "gravityMod=0x%X drag=0x%X thick=0x%X curThick=0x%X",

               g_proj_startPosOff, g_proj_startVelOff,

               g_proj_gravityModOff, g_proj_dragOff,

               g_proj_thicknessOff, g_proj_curThicknessOff);

    }



    __int64 __fastcall hkProjectileCreate(void* weapon, void* ammo,

                                          float* position, float* direction,

                                          float* velocity, void* mi)

    {

        if (!oProjectileCreate) return 0;

        __int64 entity = oProjectileCreate(weapon, ammo, position, direction, velocity, mi);



        if (entity && g_projFieldsResolved) {

            auto& wcfg = Menu::Get().WeaponCfg;

            uintptr_t e = (uintptr_t)entity;

            static unsigned long long modCount = 0;

            bool anyMod = false;



            (void)wcfg.FastBullet;

            if (wcfg.ThickBullet && (g_proj_thicknessOff > 0 || g_proj_curThicknessOff > 0)) {

                float thick = wcfg.ThickBulletSize;

                if (thick < 0.01f) thick = 0.01f;

                if (thick > 1.5f) thick = 1.5f;

                if (g_proj_thicknessOff > 0)

                    SafeWrite(e + (uintptr_t)g_proj_thicknessOff, &thick, 4);

                if (g_proj_curThicknessOff > 0)

                    SafeWrite(e + (uintptr_t)g_proj_curThicknessOff, &thick, 4);

                anyMod = true;

            }

            if (anyMod) {

                modCount++;

                if ((modCount % 5ULL) == 1ULL) {

                    float th = 0, cth = 0;

                    if (g_proj_thicknessOff > 0) SafeRead(e + (uintptr_t)g_proj_thicknessOff, &th, 4);

                    if (g_proj_curThicknessOff > 0) SafeRead(e + (uintptr_t)g_proj_curThicknessOff, &cth, 4);

                    AimLog("[RH][aim] ProjCreate entity mod #%llu: thick=%.3f curThick=%.3f "

                           "(fb=%d tb=%d)",

                           (unsigned long long)modCount, th, cth,

                           (int)wcfg.FastBullet, (int)wcfg.ThickBullet);

                }

            }

        }



        return entity;

    }



    // Install the Projectile.Create hook by direct RVA inside GameAssembly.dll.

    // We don't go through IL2CPP class_get_method_from_name because (a) the

    // class containing this method is obfuscated in the pirated build and (b)

    // the RVA is stable across runs of the same binary.

    static bool TryInstallProjectileCreateHook() {

        if (HasProjCreateHook) return true;

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!ga) {

            AimLog("[RH][aim] ProjCreate: GameAssembly.dll not loaded");

            return false;

        }

        const uintptr_t kRVA = 0x4012E20; // sub_184012E20 in IDA (image base 0x180000000)

        void* fnPtr = (void*)((uintptr_t)ga + kRVA);



        // Quick sanity check: the function should start with a recognisable

        // x64 prologue. From IDA: 48 89 5C 24 ?? 48 89 6C 24 ?? ...

        unsigned char bytes[16] = {0};

        if (SafeRead((uintptr_t)fnPtr, bytes, 16)) {

            char line[96] = {}; int pos = 0;

            for (int i = 0; i < 16; i++)

                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[i]);

            bool plausible = LooksLikeX64Prologue(bytes);

            AimLog("[RH][aim] ProjCreate: ga=%p fn=%p plausible=%d bytes=%s",

                   (void*)ga, fnPtr, (int)plausible, line);

            if (!plausible) {

                AimLog("[RH][aim] ProjCreate: bytes don't match a function prolog — RVA may be wrong for this build");

                return false;

            }

        } else {

            AimLog("[RH][aim] ProjCreate: failed to read bytes at fn=%p", fnPtr);

            return false;

        }



        if (HookFunctionRobust(fnPtr, (void*)&hkProjectileCreate,

                               (void**)&oProjectileCreate, "Projectile.Create"))

        {

            g_projCreateAddr = fnPtr;

            HasProjCreateHook = true;

            // Attach the VEH preHook so we can patch callee-saved XMM regs

            // that feed the protobuf packet (server-side damage validation).

            // If the hook landed on MinHook (rare on this build), SetHwBpPreHookFor

            // simply returns false and we log a warning — silent aim will still

            // work visually but server may reject damage.

            if (!SetHwBpPreHookFor(fnPtr, &ProjectileCreatePreHook)) {

                AimLog("[RH][aim] WARN: Projectile.Create not on HW BP path; "

                       "server-side velocity patch DISABLED; damage may not register");

            }

            AimLog("[RH][aim] ProjCreate: ✓ hook installed at %p (RVA 0x%llX)",

                   fnPtr, (unsigned long long)kRVA);

            return true;

        }

        AimLog("[RH][aim] ProjCreate: hook install FAILED");

        return false;

    }



    typedef void (__fastcall* tProjInitVel)(void* projectile, void* velocity, void* mi);

    static tProjInitVel oProjectileInitVel = nullptr;

    bool HasProjInitVelHook = false;

    static volatile unsigned long long g_projInitVelCount = 0;

    void __fastcall hkProjectileInitVel(void* projectile, void* velocity, void* mi) {

        if (oProjectileInitVel) oProjectileInitVel(projectile, velocity, mi);

        if (!projectile || !IsCanonicalUserPtr((uintptr_t)projectile)) return;

        auto& wcfg = Menu::Get().WeaponCfg;

        if (!wcfg.ThickBullet) return;

        float thick = wcfg.ThickBulletSize;

        if (thick < 0.01f) thick = 0.01f;

        if (thick > 1.5f)  thick = 1.5f;

        uintptr_t e = (uintptr_t)projectile;

        if (g_proj_thicknessOff > 0)

            SafeWrite(e + (uintptr_t)g_proj_thicknessOff, &thick, 4);

        if (g_proj_curThicknessOff > 0)

            SafeWrite(e + (uintptr_t)g_proj_curThicknessOff, &thick, 4);

        unsigned long long c = ++g_projInitVelCount;

        if ((c % 5ULL) == 1ULL) {

            float t1 = 0, t2 = 0;

            if (g_proj_thicknessOff > 0)

                SafeRead(e + (uintptr_t)g_proj_thicknessOff, &t1, 4);

            if (g_proj_curThicknessOff > 0)

                SafeRead(e + (uintptr_t)g_proj_curThicknessOff, &t2, 4);

            AimLog("[RH][aim] InitVel hook #%llu proj=%p thick=%.3f curThick=%.3f",

                   (unsigned long long)c, projectile, t1, t2);

        }

    }

    static bool TryInstallProjectileInitVelHook() {

        if (HasProjInitVelHook) return true;

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!ga) {

            AimLog("[RH][aim] ProjInitVel: GameAssembly.dll not loaded");

            return false;

        }

        const uintptr_t kRVA = 0x11AC170;

        void* fnPtr = (void*)((uintptr_t)ga + kRVA);

        unsigned char bytes[16] = {};

        if (!SafeRead((uintptr_t)fnPtr, bytes, 16)) {

            AimLog("[RH][aim] ProjInitVel: cannot read prologue at %p", fnPtr);

            return false;

        }

        char line[96] = {}; int pos = 0;

        for (int i = 0; i < 16; i++)

            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[i]);

        bool plausible = LooksLikeX64Prologue(bytes);

        AimLog("[RH][aim] ProjInitVel: ga=%p fn=%p plausible=%d bytes=%s",

               (void*)ga, fnPtr, (int)plausible, line);

        if (!plausible) {

            AimLog("[RH][aim] ProjInitVel: prologue mismatch — RVA stale, skipping");

            return false;

        }

        if (HookFunctionRobust(fnPtr, (void*)&hkProjectileInitVel,

                               (void**)&oProjectileInitVel, "Projectile.InitializeVelocity"))

        {

            HasProjInitVelHook = true;

            AimLog("[RH][aim] ProjInitVel: ✓ hook installed at %p (RVA 0x%llX)",

                   fnPtr, (unsigned long long)kRVA);

            return true;

        }

        AimLog("[RH][aim] ProjInitVel: hook install FAILED");

        return false;

    }



    static thread_local bool g_inMultiShot = false;



    void __fastcall hkLaunchProjectile(void* a0, void* a1, void* a2, void* a3, void* a4) {

        static unsigned long long c = 0;

        ++c;

        if (!oLaunchProjectile) return;



        // NOTE: ProjDirVM hook (vtable+0x3F08) was DISABLED — it was hooking

        // a Projectile.Simulate-style accumulator (sub_18539FD80 was a +=

        // not a setter), so writes had no effect on bullet trajectory; it

        // also caused infinite recursion through HW BP and hung the game on

        // the 4-5th shot. Replaced by the Projectile.Create hook below

        // (sub_184012E20) which rewrites the direction Vector3 *before*

        // Quaternion.LookRotation_Injected and Instantiate run.

        // TryInstallProjDirVMHook(a0); // disabled — see above



        bool doSilent = ShouldSilentThisShot();

        if ((c % 10ULL) == 1ULL) {

            AimLog("[RH][aim] hkLP#%llu wp=%p tgt=%p silent=%d vmOff=0x%llX vmRaw=%llu",

                   c, a0, (void*)currentTarget, (int)doSilent,

                   (unsigned long long)g_projDirVMOffset,

                   (unsigned long long)g_projDirVMRawHits);

        }



        auto& wcfg = Menu::Get().WeaponCfg;

        uintptr_t weapon = (uintptr_t)a0;

        if (wcfg.NoSpread) ZeroConeFields(weapon);



        DWORD prevTid = g_launchThreadId;

        g_launchThreadId = GetCurrentThreadId();



        uint32_t savedFlags = 0;

        uintptr_t flagsAddr = 0;

        bool jumpShootApplied = false;

        if (Menu::Get().MiscCfg.JumpShoot && localPlayer) {

            uintptr_t msPtr = 0;

            if (SafeReadPtr(localPlayer + offsets::BasePlayer::modelState, &msPtr)

                && IsCanonicalUserPtr(msPtr))

            {

                flagsAddr = msPtr + offsets::ModelState::flags;

                if (SafeRead(flagsAddr, &savedFlags, 4)) {

                    const uint32_t clearMask = offsets::ModelState::Jumped

                                             | offsets::ModelState::Flying;

                    uint32_t faked = (savedFlags & ~clearMask)

                                   | offsets::ModelState::OnGround;

                    if (faked != savedFlags) {

                        SafeWrite(flagsAddr, &faked, 4);

                        jumpShootApplied = true;

                    }

                }

            }

        }



        SilentState silent = {};

        if (doSilent) {

            silent = BeginSilentAngles((uintptr_t)a0);

        }



        oLaunchProjectile(a0, a1, a2, a3, a4);



        if (wcfg.DoubleTap && !g_inMultiShot) {

            static unsigned long long s_lastShotTick = 0;

            const unsigned long long kFreshClickGapMs = 180;

            unsigned long long nowTick = GetTickCount64();

            bool freshClick = (nowTick - s_lastShotTick) > kFreshClickGapMs;

            s_lastShotTick = nowTick;



            if (freshClick) {

                g_inMultiShot = true;

                oLaunchProjectile(a0, a1, a2, a3, a4);

                g_inMultiShot = false;

            }

        }



        if (silent.count > 0) EndSilentAngles(silent);

        g_launchThreadId = prevTid;



        if (jumpShootApplied && flagsAddr) {

            SafeWrite(flagsAddr, &savedFlags, 4);

        }



        if (Menu::Get().WorldCfg.BulletTracer) {

            float pos[3] = {};

            float angles[3] = {};

            bool gotPos = false, gotAngles = false;

            uintptr_t lp = localPlayer;

            if (lp && g_fnGetTransform && g_fnGetPosInjected) {

                void* tr = g_fnGetTransform((void*)lp, nullptr);

                if (tr && IsCanonicalUserPtr((uintptr_t)tr)) {

                    g_fnGetPosInjected(tr, pos, nullptr);

                    pos[1] += 1.6f;

                    gotPos = true;

                }

            }

            if (lp) {

                uintptr_t ipPtr = 0;

                if (SafeReadPtr(lp + offsets::BasePlayer::playerInput, &ipPtr)

                    && IsCanonicalUserPtr(ipPtr))

                {

                    if (SafeRead(ipPtr + offsets::PlayerInput::bodyAngles, angles, 12)

                        && std::isfinite(angles[0]) && std::isfinite(angles[1]))

                    {

                        gotAngles = true;

                    }

                }

            }

            if (gotPos && gotAngles) {

                float pitch = angles[0] * 0.0174533f;

                float yaw   = angles[1] * 0.0174533f;

                float fwd[3] = {

                    cosf(pitch) * sinf(yaw),

                    -sinf(pitch),

                    cosf(pitch) * cosf(yaw)

                };

                const float defaultSpeed = 300.0f;

                PushTracer(pos[0], pos[1], pos[2],

                           fwd[0] * defaultSpeed,

                           fwd[1] * defaultSpeed,

                           fwd[2] * defaultSpeed);

            }

        }

    }



    // ============================================================

    //  ProtoBuf.ProjectileShoot.WriteToStream — TRUE SILENT AIM

    // ============================================================

    // The client sends a ProjectileShoot protobuf packet to the server

    // describing every bullet spawned per tick. The message contains a

    //     List<ProjectileShoot.Projectile> projectiles

    // where each Projectile has:

    //     int     projectileID

    //     Vector3 startPos   (spawn position in world space)

    //     Vector3 startVel   (initial velocity — determines trajectory)

    //     float   travelTime

    //

    // WriteToStream is the protobuf serializer invoked immediately

    // before the bytes go out on the wire. By rewriting startVel

    // (and keeping its magnitude) to point at our locked target we

    // make every bullet head at the target on the server's trace,

    // without touching the local camera / bodyAngles / eyes rotation.

    //

    // Hooked via HookFunctionRobust (MinHook → HW BP fallback).

    typedef void(__fastcall* fnProjShootWrite_t)(void* this_, void* stream, void* mi);

    static fnProjShootWrite_t oProjectileShoot_WriteToStream = nullptr;

    bool HasProjShootHook = false;





    void __fastcall hkProjectileShoot_WriteToStream(void* this_, void* stream, void* mi) {

        static unsigned long long c = 0;

        ++c;

        if (!oProjectileShoot_WriteToStream) return;



        struct Saved { uintptr_t addr; uint8_t size; uint8_t data[12]; };

        Saved saved[64] = {};

        int nSaved = 0;



        bool doSilent = ShouldSilentThisShot();



        if (doSilent && this_ != nullptr) {

            uintptr_t list = 0;

            if (SafeReadPtr((uintptr_t)this_ + g_projectilesOff, &list) && IsCanonicalUserPtr(list)) {

                uintptr_t items = 0;

                uint32_t  size  = 0;

                SafeReadPtr(list + 0x10, &items);

                SafeRead  (list + 0x18, &size, 4);



                if (IsCanonicalUserPtr(items) && size > 0 && size <= 32) {

                    float aimDx = 0.0f, aimDy = 0.0f, aimDz = 0.0f;

                    bool haveAimDir = false;

                    if (localPlayer != 0) {

                        aimDx = targetPosX - localEyeX;

                        aimDy = targetPosY - localEyeY;

                        aimDz = targetPosZ - localEyeZ;

                        float dlen = sqrtf(aimDx*aimDx + aimDy*aimDy + aimDz*aimDz);

                        if (dlen >= 0.01f) {

                            float inv = 1.0f / dlen;

                            aimDx *= inv; aimDy *= inv; aimDz *= inv;

                            haveAimDir = true;

                        }

                    }



                    if (haveAimDir) {

                        int rewroteVel = 0;

                        int rewrotePos = 0;

                        for (uint32_t i = 0; i < size && nSaved < 62; i++) {

                            uintptr_t proj = 0;

                            if (!SafeReadPtr(items + 0x20 + i * 8ULL, &proj)) break;

                            if (!IsCanonicalUserPtr(proj)) continue;



                            uintptr_t velAddr = proj + (uintptr_t)g_proj_startVelOff;

                            float origVel[3] = { 0 };

                            if (!SafeRead(velAddr, origVel, 12)) continue;



                            float speed = sqrtf(origVel[0]*origVel[0] +

                                                origVel[1]*origVel[1] +

                                                origVel[2]*origVel[2]);

                            if (speed < 50.0f || speed > 5000.0f) continue;



                            float newVel[3] = { aimDx * speed, aimDy * speed, aimDz * speed };

                            saved[nSaved].addr = velAddr;

                            saved[nSaved].size = 12;

                            memcpy(saved[nSaved].data, origVel, 12);

                            nSaved++;

                            SafeWrite(velAddr, newVel, 12);

                            rewroteVel++;



                            if (g_manipActive && g_proj_startPosOff > 0) {

                                uintptr_t posAddr = proj + (uintptr_t)g_proj_startPosOff;

                                float origPos[3] = { 0 };

                                if (SafeRead(posAddr, origPos, 12)) {

                                    saved[nSaved].addr = posAddr;

                                    saved[nSaved].size = 12;

                                    memcpy(saved[nSaved].data, origPos, 12);

                                    nSaved++;

                                    SafeWrite(posAddr, g_manipPeekEyePos, 12);

                                    rewrotePos++;

                                }

                            }

                        }



                        if ((c % 10ULL) == 1ULL) {

                            AimLog("[RH][aim] hkPSWrite#%llu silent vel rewrite: "

                                   "size=%u nSaved=%d velRw=%d posRw=%d manip=%d",

                                   c, size, nSaved, rewroteVel, rewrotePos, (int)g_manipActive);

                        }

                    }

                }

            }

        }



        oProjectileShoot_WriteToStream(this_, stream, mi);



        for (int i = nSaved - 1; i >= 0; i--) {

            SafeWrite(saved[i].addr, saved[i].data, saved[i].size);

        }

    }



    // Find the ProtoBuf.ProjectileShoot class + resolve projectiles field

    // offset + install the WriteToStream hook. Returns true on success.

    //

    // The class name on this build might be obfuscated. We try the clean

    // name first across all images, then fall back to a class-name scan

    // (looking for names containing "ProjectileShoot" or "Projectile")

    // that match our structural fingerprint.

    static bool InstallProjectileShootHook_Inner(Il2CppImage* primaryImg) {

        AimLog("[RH][aim] ProjShoot: InstallProjectileShootHook entering");



        auto tryInstall = [&](Il2CppClass* c, const char* origin) -> bool {

            if (!c) return false;

            // Resolve projectiles field offset.

            auto f = il2cpp.class_get_field_from_name(c, "projectiles");

            if (!f) return false;

            int fo = il2cpp.field_get_offset(f);

            if (fo <= 0 || fo > 0x80) return false;



            // Find WriteToStream(argc=1) — instance method taking Stream.

            void* method = il2cpp.class_get_method_from_name(c, "WriteToStream", 1);

            if (!method) return false;

            void* fnPtr = *(void**)method;

            if (!fnPtr) return false;



            const char* cname = il2cpp.class_get_name ? il2cpp.class_get_name(c) : "?";

            AimLog("[RH][aim] ProjShoot(%s): class='%s' projectiles@0x%X WriteToStream fn=%p",

                   origin, cname ? cname : "?", fo, fnPtr);



            g_projectilesOff = fo;



            if (HookFunctionRobust(fnPtr,

                                   (void*)&hkProjectileShoot_WriteToStream,

                                   (void**)&oProjectileShoot_WriteToStream,

                                   "ProjectileShoot.WriteToStream")) {

                HasProjShootHook = true;

                return true;

            }

            return false;

        };



        auto domain = il2cpp.domain_get();

        if (!domain) { AimLog("[RH][aim] ProjShoot: no domain"); return false; }

        size_t acount = 0;

        auto asms = il2cpp.domain_get_assemblies(domain, &acount);

        if (!asms || acount == 0) { AimLog("[RH][aim] ProjShoot: no assemblies"); return false; }



        // Pass 1: direct class_from_name with the canonical name across ALL

        // images (not just Assembly-CSharp — the ProtoBuf type may live in

        // a separate assembly on some builds).

        const char* nsCandidates[] = { "ProtoBuf", "", "Rust", "Facepunch" };

        for (size_t ai = 0; ai < acount; ai++) {

            auto aimg = il2cpp.assembly_get_image(asms[ai]);

            if (!aimg) continue;

            for (const char* ns : nsCandidates) {

                Il2CppClass* c = il2cpp.class_from_name(aimg, ns, "ProjectileShoot");

                if (c && tryInstall(c, "by-name")) return true;

            }

        }

        AimLog("[RH][aim] ProjShoot: class_from_name failed across all images");



        // Pass 2: enumerate classes whose NAME contains "ProjectileShoot".

        // This only reads class names (cheap) + does the field / method

        // check on the small number that match.

        if (!il2cpp.image_get_class || !il2cpp.image_get_class_count ||

            !il2cpp.class_get_name) {

            AimLog("[RH][aim] ProjShoot scan: iter API missing");

            return false;

        }



        int totalChecked = 0;

        int projNameHits = 0;

        for (size_t ai = 0; ai < acount; ai++) {

            auto aimg = il2cpp.assembly_get_image(asms[ai]);

            if (!aimg) continue;

            const char* iname = il2cpp.image_get_name ? il2cpp.image_get_name(aimg) : "?";

            size_t ccount = il2cpp.image_get_class_count(aimg);

            if (ccount == 0 || ccount > 500000) continue;



            for (size_t ci = 0; ci < ccount; ci++) {

                Il2CppClass* c = il2cpp.image_get_class(aimg, ci);

                if (!c) continue;

                totalChecked++;

                const char* cname = il2cpp.class_get_name(c);

                if (!cname) continue;

                // Look for any class with "ProjectileShoot" in its name.

                if (!strstr(cname, "ProjectileShoot")) continue;

                projNameHits++;

                AimLog("[RH][aim] ProjShoot candidate: image='%s' class='%s'",

                       iname ? iname : "?", cname);

                if (tryInstall(c, "by-name-substr")) return true;

            }

        }

        AimLog("[RH][aim] ProjShoot scan: checked=%d nameHits=%d — no match",

               totalChecked, projNameHits);



        // ===================================================================

        // Pass 3 (UNFILTERED WIDE-SCAN):

        //   Previous attempts with class-name filters failed. Enumerate EVERY

        //   class in EVERY assembly, try `WriteToStream` at argc=1 AND argc=2

        //   (ProtoBuf-net sometimes generates static helpers with the type

        //   itself as first arg). Log ALL candidates so we can match them in

        //   IDA later via their RVA. Auto-hook the first candidate whose

        //   class has a plausible struct layout (any field at offset

        //   [0x08..0x100]).

        //

        //   If zero candidates are found, the method name "WriteToStream" is

        //   NOT in IL2CPP metadata for any class — it's either obfuscated

        //   to a Beebyte hash, or protobuf serialization is routed through

        //   a different API (e.g. ProtoBuf.Serializer.Serialize<T>). In that

        //   case we log a DIAGNOSTIC listing the first ~30 method names on

        //   a few obfuscated classes to see if names are hashed.

        // ===================================================================

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        uintptr_t gaBase = (uintptr_t)ga;

        AimLog("[RH][aim] ProjShoot WIDE-SCAN: GameAssembly base=%p (use this to map RVA in IDA)", ga);

        if (gaBase == 0) {

            AimLog("[RH][aim] ProjShoot WIDE-SCAN: no GameAssembly handle, abort");

            return false;

        }



        struct Cand {

            Il2CppClass* klass;

            void*        fnPtr;

            uintptr_t    rva;

            int          firstObjFieldOff;

            int          argc;

            const char*  klassName;

            const char*  imgName;

        };

        constexpr int kMaxCands = 128;

        Cand cands[kMaxCands] = {};

        int nCands = 0;

        int totalObfClasses = 0;



        bool haveFieldIter = il2cpp.class_get_fields && il2cpp.field_get_offset;

        bool haveMethodIter = il2cpp.class_get_methods && il2cpp.method_get_name;



        // Try both argc signatures. ProtoBuf-net generated code:

        //   instance void WriteToStream(Stream s) -> argc=1

        //   static void Serialize(T inst, Stream s) -> argc=2 (some variants)

        const int kArgcVariants[] = { 1, 2 };



        for (size_t ai = 0; ai < acount && nCands < kMaxCands; ai++) {

            auto aimg = il2cpp.assembly_get_image(asms[ai]);

            if (!aimg) continue;

            const char* iname = il2cpp.image_get_name ? il2cpp.image_get_name(aimg) : "?";

            size_t ccount = il2cpp.image_get_class_count(aimg);

            if (ccount == 0 || ccount > 500000) continue;



            for (size_t ci = 0; ci < ccount && nCands < kMaxCands; ci++) {

                Il2CppClass* c = il2cpp.image_get_class(aimg, ci);

                if (!c) continue;

                const char* cname = il2cpp.class_get_name(c);

                if (!cname) continue;

                if (cname[0] == '%') totalObfClasses++;



                void* method = nullptr;

                int   matchedArgc = -1;

                for (int ac : kArgcVariants) {

                    method = il2cpp.class_get_method_from_name(c, "WriteToStream", ac);

                    if (method) { matchedArgc = ac; break; }

                }

                if (!method) continue;



                void* fnPtr = *(void**)method;

                if (!fnPtr) continue;

                uintptr_t fnAddr = (uintptr_t)fnPtr;

                if (fnAddr <= gaBase) continue;

                uintptr_t rva = fnAddr - gaBase;



                // Find first field at offset in [0x08..0x100] — widen range.

                int firstObjOff = -1;

                if (haveFieldIter) {

                    void* it = nullptr;

                    Il2CppFieldInfo* f = nullptr;

                    while ((f = il2cpp.class_get_fields(c, &it)) != nullptr) {

                        int fo = il2cpp.field_get_offset(f);

                        if (fo >= 0x08 && fo <= 0x100) {

                            firstObjOff = fo;

                            break;

                        }

                    }

                }

                // DON'T skip if firstObjOff < 0 — log anyway; hook will use default 0x20.



                cands[nCands].klass    = c;

                cands[nCands].fnPtr    = fnPtr;

                cands[nCands].rva      = rva;

                cands[nCands].firstObjFieldOff = (firstObjOff >= 0) ? firstObjOff : 0x20;

                cands[nCands].argc     = matchedArgc;

                cands[nCands].klassName = cname;

                cands[nCands].imgName   = iname;

                nCands++;

            }

        }



        AimLog("[RH][aim] ProjShoot WIDE-SCAN: scanned ~%d classes (obf=%d), found %d WriteToStream candidates (argc=1|2)",

               totalChecked, totalObfClasses, nCands);

        int logLimit = nCands > 40 ? 40 : nCands;

        for (int i = 0; i < logLimit; i++) {

            AimLog("[RH][aim]   cand[%d] class='%s' img='%s' WriteToStream(argc=%d)=%p RVA=0x%llX firstField@+0x%X",

                   i, cands[i].klassName, cands[i].imgName, cands[i].argc, cands[i].fnPtr,

                   (unsigned long long)cands[i].rva, cands[i].firstObjFieldOff);

        }

        if (nCands > logLimit) {

            AimLog("[RH][aim]   ... %d more candidates (log truncated to %d)",

                   nCands - logLimit, logLimit);

        }



        // ---- DIAGNOSTIC: if 0 candidates, print method names of a few

        // obfuscated classes to understand whether method names are hashed ---

        if (nCands == 0 && haveMethodIter) {

            AimLog("[RH][aim] ProjShoot DIAG: zero WriteToStream candidates. "

                   "Dumping method names of first 5 obf classes to check if "

                   "method names are obfuscated too:");

            int dumped = 0;

            for (size_t ai = 0; ai < acount && dumped < 5; ai++) {

                auto aimg = il2cpp.assembly_get_image(asms[ai]);

                if (!aimg) continue;

                size_t ccount = il2cpp.image_get_class_count(aimg);

                for (size_t ci = 0; ci < ccount && dumped < 5; ci++) {

                    Il2CppClass* c = il2cpp.image_get_class(aimg, ci);

                    if (!c) continue;

                    const char* cname = il2cpp.class_get_name(c);

                    if (!cname || cname[0] != '%') continue;

                    // Count fields; skip classes with very few fields.

                    int nfields = 0;

                    if (haveFieldIter) {

                        void* it = nullptr;

                        while (il2cpp.class_get_fields(c, &it)) nfields++;

                    }

                    if (nfields < 3) continue;



                    dumped++;

                    char names[512] = {};

                    int  pos = 0;

                    int  mcount = 0;

                    void* it = nullptr;

                    Il2CppMethodInfo* m = nullptr;

                    while ((m = il2cpp.class_get_methods(c, &it)) != nullptr && mcount < 12) {

                        const char* mn = il2cpp.method_get_name(m);

                        if (!mn) continue;

                        pos += snprintf(names + pos, sizeof(names) - pos,

                                        " %s", mn);

                        mcount++;

                        if (pos > (int)sizeof(names) - 40) break;

                    }

                    AimLog("[RH][aim]   obfClass[%d] name='%s' fields=%d methods:%s",

                           dumped - 1, cname, nfields,

                           names[0] ? names : " (none)");

                }

            }

        }



        if (nCands == 0) {

            AimLog("[RH][aim] ProjShoot WIDE-SCAN: no candidates — silent aim packet hook unavailable");

            return false;

        }



        // Auto-hook the FIRST candidate. The hook's structural validation

        // (list[0].startVel in [50..5000]) ensures non-ProjectileShoot

        // messages pass through cleanly — so if we pick wrong, no harm.

        int nHooked = 0;

        if (nCands > 0) {

            g_projectilesOff = cands[0].firstObjFieldOff;

            if (HookFunctionRobust(cands[0].fnPtr,

                                   (void*)&hkProjectileShoot_WriteToStream,

                                   (void**)&oProjectileShoot_WriteToStream,

                                   "ProjectileShoot.WriteToStream(SCAN)")) {

                HasProjShootHook = true;

                nHooked++;

                AimLog("[RH][aim] ProjShoot WIDE-SCAN: HOOKED cand[0] '%s' projectiles@+0x%X RVA=0x%llX argc=%d",

                       cands[0].klassName, cands[0].firstObjFieldOff,

                       (unsigned long long)cands[0].rva, cands[0].argc);

            } else {

                AimLog("[RH][aim] ProjShoot WIDE-SCAN: cand[0] hook FAILED");

            }

        }

        return nHooked > 0;

    }



    static bool InstallProjectileShootHook(Il2CppImage* img) {

        bool ok = false;

        __try {

            ok = InstallProjectileShootHook_Inner(img);

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            AimLog("[RH][aim] ProjShoot: InstallProjectileShootHook CRASHED");

            ok = false;

        }

        return ok;

    }



    // SimulateAimcone in this build is argc=0. Its job is to advance the

    // per-shot cone penalty. Skip it when NoSpread is on to prevent the cone

    // from growing during auto-fire.

    void __fastcall hkSimulateAimcone(void* a0, void* a1, void* a2, void* a3, void* a4) {

        if (!oSimulateAimcone) return;

        auto& wcfg = Menu::Get().WeaponCfg;

        if (wcfg.NoSpread) return;  // swallow the call

        oSimulateAimcone(a0, a1, a2, a3, a4);

    }



    // Find the function pointer for AimConeUtil.GetModifiedAimConeDirection.

    // Strategy:

    //   1. class_from_name("AimConeUtil") across all images and namespaces.

    //   2. Fallback: iterate all classes in each image and look for a class

    //      that has a method literally named "GetModifiedAimConeDirection".

    //      This handles EAC-style class-name obfuscation where the class

    //      itself is renamed but the method name is kept (for reflection or

    //      internal IL2CPP metadata).

    static void* FindAimConeDirectionFn() {

        auto domain = il2cpp.domain_get();

        if (!domain) return nullptr;

        size_t acount = 0;

        auto asms = il2cpp.domain_get_assemblies(domain, &acount);

        if (!asms || acount == 0) return nullptr;



        // Pass 1: class_from_name with common namespaces.

        const char* nsCandidates[] = { "", "Facepunch", "Rust", "Rust.Ai" };

        for (size_t i = 0; i < acount; i++) {

            auto aimg = il2cpp.assembly_get_image(asms[i]);

            if (!aimg) continue;

            for (const char* ns : nsCandidates) {

                Il2CppClass* c = il2cpp.class_from_name(aimg, ns, "AimConeUtil");

                if (!c) continue;

                void* m = il2cpp.class_get_method_from_name(c, "GetModifiedAimConeDirection", 3);

                if (!m) m = il2cpp.class_get_method_from_name(c, "GetModifiedAimConeDirection", 0);

                if (!m) m = il2cpp.class_get_method_from_name(c, "GetModifiedAimConeDirection", -1);

                if (m) {

                    void* fn = *(void**)m;

                    if (fn) {

                        const char* iname = il2cpp.image_get_name(aimg);

                        AimLog("[RH][aim] AimConeUtil via name-lookup: image='%s' ns='%s' fn=%p",

                               iname ? iname : "?", ns, fn);

                        return fn;

                    }

                }

            }

        }

        AimLog("[RH][aim] AimConeUtil not found by class_from_name; scanning all classes...");



        // Pass 2: iterate every class in every image looking for a method

        // literally named "GetModifiedAimConeDirection", OR any method whose

        // name contains "Cone" (case-insensitive) — to reveal renamed methods.

        if (!il2cpp.image_get_class || !il2cpp.image_get_class_count ||

            !il2cpp.class_get_methods || !il2cpp.method_get_name) {

            AimLog("[RH][aim] class iteration API not available");

            return nullptr;

        }



        auto containsCI = [](const char* s, const char* needle) -> bool {

            if (!s || !needle) return false;

            size_t slen = strlen(s), nlen = strlen(needle);

            if (nlen > slen) return false;

            for (size_t i = 0; i <= slen - nlen; i++) {

                size_t j = 0;

                for (; j < nlen; j++) {

                    char a = s[i+j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);

                    char b = needle[j]; if (b >= 'A' && b <= 'Z') b = (char)(b + 32);

                    if (a != b) break;

                }

                if (j == nlen) return true;

            }

            return false;

        };



        void* exactMatch = nullptr;

        int totalClasses = 0;

        int totalMethods = 0;

        int coneHits = 0;

        const int MAX_CONE_HITS_LOGGED = 30;



        for (size_t i = 0; i < acount; i++) {

            auto aimg = il2cpp.assembly_get_image(asms[i]);

            if (!aimg) continue;

            const char* iname = il2cpp.image_get_name(aimg);

            size_t ccount = il2cpp.image_get_class_count(aimg);

            if (ccount == 0) continue;

            if (ccount > 500000) {

                AimLog("[RH][aim] skipping image '%s' with huge class count %zu",

                       iname ? iname : "?", ccount);

                continue;

            }

            for (size_t ci = 0; ci < ccount; ci++) {

                Il2CppClass* c = il2cpp.image_get_class(aimg, ci);

                if (!c) continue;

                totalClasses++;

                void* iter = nullptr;

                Il2CppMethodInfo* m;

                while ((m = il2cpp.class_get_methods(c, &iter)) != nullptr) {

                    totalMethods++;

                    const char* mname = il2cpp.method_get_name(m);

                    if (!mname) continue;



                    if (strcmp(mname, "GetModifiedAimConeDirection") == 0) {

                        void* fn = *(void**)m;

                        const char* cname = il2cpp.class_get_name ? il2cpp.class_get_name(c) : "?";

                        AimLog("[RH][aim] AimConeUtil via method-scan: image='%s' class='%s' fn=%p",

                               iname ? iname : "?", cname ? cname : "?", fn);

                        if (fn && !exactMatch) exactMatch = fn;

                    } else if (containsCI(mname, "cone")) {

                        // Log any method with "cone" substring so we can spot

                        // renamed candidates.

                        if (coneHits < MAX_CONE_HITS_LOGGED) {

                            uint32_t argc = il2cpp.method_get_param_count

                                ? il2cpp.method_get_param_count(m) : 0xFFFFFFFFu;

                            void* fn = *(void**)m;

                            const char* cname = il2cpp.class_get_name ? il2cpp.class_get_name(c) : "?";

                            AimLog("[RH][aim]   cone-candidate: %s.%s(argc=%u) -> %p",

                                   cname ? cname : "?", mname, argc, fn);

                        }

                        coneHits++;

                    }

                }

            }

        }

        AimLog("[RH][aim] Scan: totalClasses=%d totalMethods=%d coneCandidates=%d",

               totalClasses, totalMethods, coneHits);

        if (exactMatch) return exactMatch;

        AimLog("[RH][aim] GetModifiedAimConeDirection NOT FOUND anywhere");

        return nullptr;

    }



    // Helper: hook a resolved fn pointer. Tries MinHook first (low overhead),

    // then falls back to HW BP + VEH if MH_EnableHook is blocked by EAC.

    // The trampoline goes into *origOut regardless, so the detour can call

    // it to execute the original.

    static bool HookFunctionRobust(void* fnPtr, void* hook, void** origOut, const char* label) {

        if (!fnPtr) {

            AimLog("[RH][aim] hook: %s fnPtr NULL", label);

            return false;

        }

        MH_STATUS st = MH_CreateHook(fnPtr, hook, origOut);

        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {

            AimLog("[RH][aim] MH_CreateHook %s failed: %d", label, (int)st);

            return false;

        }

        st = MH_EnableHook(fnPtr);

        if (st == MH_OK) {

            AimLog("[RH][aim] ✓ %s hooked via MinHook at %p", label, fnPtr);

            return true;

        }

        AimLog("[RH][aim] MH_EnableHook %s failed: %d — falling back to HW BP", label, (int)st);

        if (AddHwBp(fnPtr, hook, label)) {

            AimLog("[RH][aim] ✓ %s hooked via HW BP at %p", label, fnPtr);

            return true;

        }

        AimLog("[RH][aim] ✗ %s failed to hook by both methods", label);

        return false;

    }



    bool InstallShotHooks() {

        Il2CppImage* img = il2cpp.FindImage("Assembly-CSharp.dll");

        if (!img) { AimLog("[RH][aim] InstallShotHooks: image missing"); return false; }



        // === PRIORITY 1: AimConeUtil.GetModifiedAimConeDirection ===

        // This single hook handles BOTH NoSpread AND Perfect Silent aim by

        // replacing the bullet direction vector directly. Gets DR0.

        void* acuFn = FindAimConeDirectionFn();

        if (acuFn) {

            if (HookFunctionRobust(acuFn,

                                   (void*)&hkGetModifiedAimConeDirection,

                                   (void**)&oGetModifiedAimConeDirection,

                                   "AimConeUtil.GetModifiedAimConeDirection")) {

                HasAimConeHook = true;

            }

        }



        // === PRIORITY 2: BaseProjectile shot hooks ===

        // LaunchProjectile: the real per-shot entry. Reads the aim cone,

        //                   ammo spread, and builds the actual projectile.

        //                   DoAttack was a misfire — it's a per-tick input

        //                   handler that runs every frame while the weapon

        //                   is held, never touches spread. Hooking Launch-

        //                   Projectile catches the ACTUAL bullet spawn.

        // SimulateAimcone:  swallow to prevent aimcone-penalty growth

        //                   (backup NoSpread safety).

        Il2CppClass* bp = il2cpp.class_from_name(img, "", "BaseProjectile");

        int ok = 0;

        if (!bp) {

            AimLog("[RH][aim] BaseProjectile class missing; skipping shot hooks");

        } else {

            struct Target { const char* name; int argc; void* hookFn; void** origOut; bool* flag; };

            Target targets[] = {

                { "LaunchProjectile", 0, (void*)&hkLaunchProjectile, (void**)&oLaunchProjectile, &HasShotHook },

            };

            for (auto& t : targets) {

                void* method = il2cpp.class_get_method_from_name(bp, t.name, t.argc);

                if (!method) {

                    AimLog("[RH][aim] BaseProjectile::%s(argc=%d) not found", t.name, t.argc);

                    continue;

                }

                void* fnPtr = *(void**)method;

                char label[64];

                _snprintf_s(label, sizeof(label), "BaseProjectile::%s", t.name);

                if (HookFunctionRobust(fnPtr, t.hookFn, t.origOut, label)) {

                    if (t.flag) *t.flag = true;

                    ok++;

                    // Remember the raw module address for LaunchProjectile so

                    // the per-shot diagnostic can dump its opening bytes.

                    if (_stricmp(t.name, "LaunchProjectile") == 0) {

                        g_launchProjectileFnAddr = fnPtr;

                    }

                }

            }

        }



        // === Resolve Projectile entity class fields ===

        // The Projectile entity class has UN-OBFUSCATED field names for

        // gravityModifier, drag, thickness, penetrationPower. Resolve

        // offsets now so hkProjectileCreate can modify them per-entity

        // for No Bullet Drop / Wallbang exploits.

        // NOTE: proto ProjectileShoot does NOT contain these fields —

        // they are client-side entity fields that affect simulation.

        if (!g_projFieldsResolved) {

            Il2CppClass* projClass = il2cpp.class_from_name(img, "", "Projectile");

            if (projClass) {

                ResolveProjectileFields(projClass);

                AimLog("[RH][aim] Projectile entity class resolved OK");

            } else {

                AimLog("[RH][aim] WARNING: Projectile entity class not found — "

                       "NoDrop/Wallbang will not work");

            }

        }



        // === PRIORITY 3: Projectile.Create (sub_184012E20) ===

        // TRUE silent aim chokepoint + exploit field modification.

        // Hooks the bullet-spawn function directly via RVA (0x4012E20)

        // and rewrites direction Vector3 (silent aim) + entity fields

        // (gravity/drag/penetration) after creation.

        AimLog("[RH][aim] InstallShotHooks: calling TryInstallProjectileCreateHook...");

        bool pcOk = TryInstallProjectileCreateHook();

        AimLog("[RH][aim] InstallShotHooks: TryInstallProjectileCreateHook returned %d", (int)pcOk);



        AimLog("[RH][aim] InstallShotHooks: calling TryInstallProjectileInitVelHook (thick bullet)...");

        bool pivOk = TryInstallProjectileInitVelHook();

        AimLog("[RH][aim] InstallShotHooks: TryInstallProjectileInitVelHook returned %d", (int)pivOk);



        // === PRIORITY 4: ProtoBuf.ProjectileShoot.WriteToStream ===

        // Backup TRUE silent aim: rewrite projectile velocity vectors in

        // the network packet just before it goes out on the wire. See the

        // hook itself for full details. Kept enabled in case the

        // Projectile.Create hook misses some code path.

        AimLog("[RH][aim] InstallShotHooks: calling InstallProjectileShootHook...");

        bool psOk = InstallProjectileShootHook(img);

        AimLog("[RH][aim] InstallShotHooks: InstallProjectileShootHook returned %d", (int)psOk);



        AimLog("[RH][aim] Installing ClientInput hook for manipulator tick desync...");

        if (InstallInputHook()) {

            if (EnableInputHook()) {

                AimLog("[RH][aim] ✓ ClientInput hook ACTIVE (manipulator will send peek in tick)");

            } else {

                AimLog("[RH][aim] ✗ ClientInput EnableInputHook failed (hwBpSlots=%d/4)", g_hwBpCount);

            }

        } else {

            AimLog("[RH][aim] ✗ ClientInput InstallInputHook failed");

        }



        AimLog("[RH][aim] Installing SendTick hook for server-side anti-aim...");

        if (TryInstallSendTickHook()) {

            AimLog("[RH][aim] ✓ SendTick hook ACTIVE (anti-aim angles will be sent to server)");

        } else {

            AimLog("[RH][aim] ✗ SendTick hook failed (anti-aim will remain client-side only)");

        }



        AimLog("[RH][aim] ================================");

        AimLog("[RH][aim] FINAL: ShotHook=%d AimConeHook=%d "

               "ProjCreate=%d ProjInitVel=%d ProjShoot=%d SendTick=%d hwBpSlots=%d/4",

               (int)HasShotHook, (int)HasAimConeHook,

               (int)HasProjCreateHook, (int)HasProjInitVelHook,

               (int)HasProjShootHook, (int)HasSendTickHook, g_hwBpCount);

        AimLog("[RH][aim] ================================");

        return ok > 0 || HasAimConeHook || HasProjCreateHook || HasProjShootHook;

    }



    void Update() {

        TryProbeEyes(localPlayer);



        auto& cfg = Menu::Get().AimCfg;

        // Bail only if BOTH aimbot and silent-aim are off. We still need

        // to compute target pitch/yaw for silent-aim shot-hook writes

        // even when the classic magnetic aimbot is disabled.

        if (!cfg.Enabled && !cfg.PerfectSilent) return;

        if (currentTarget == 0 || localPlayer == 0) return;

        if (cfg.VisibleCheck && !currentTargetVisible) return;



        uintptr_t input = 0;

        if (!SafeReadPtr(localPlayer + offsets::BasePlayer::playerInput, &input) || !input)

            return;



        float dx = targetPosX - localEyeX;

        float dy = targetPosY - localEyeY;

        float dz = targetPosZ - localEyeZ;

        float horizDist = sqrtf(dx*dx + dz*dz);

        if (horizDist < 0.01f) return;



        float yaw = atan2f(dx, dz) * 57.295779513f;

        float pitch = -atan2f(dy, horizDist) * 57.295779513f;

        if (pitch > 89.0f) pitch = 89.0f;

        if (pitch < -89.0f) pitch = -89.0f;



        currentPitch = pitch;

        currentYaw = yaw;



        bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;



        // Aim mode dispatch:

        //   PerfectSilent + hooks -> TRUE SILENT: hkLaunchProjectile writes

        //       bodyAngles + quaternion around oLaunchProjectile, ProjDirVM

        //       overrides direction vector. Camera doesn't move.

        //       Do NOT write bodyAngles here — hkLaunchProjectile handles it.

        //   PerfectSilent + no hooks -> LMB fallback (visible snap).

        //   not PerfectSilent -> RBUTTON-held aimbot (standard magnetic aim).

        //   AutoShoot -> always aim when target in FOV.

        shouldSilentAim = false;



        bool doAimbotWrite = false;

        if (cfg.PerfectSilent) {

            if (HasShotHook || HasAimConeHook) {

                doAimbotWrite = false;

            } else if (lmbDown || cfg.AutoShoot) {

                doAimbotWrite = true;

            }

        } else if (cfg.AutoShoot) {

            doAimbotWrite = true;

        } else if (rmbDown) {

            doAimbotWrite = true;

        }



        if (doAimbotWrite) {

            SafeWriteFloat(input + offsets::PlayerInput::bodyAngles,     pitch);

            SafeWriteFloat(input + offsets::PlayerInput::bodyAngles + 4, yaw);

        }

    }



    // Class-hierarchy check: walks parents until it finds "BaseProjectile".

    // Safe against non-projectile held items (rocks, medicals) so we don't

    // corrupt their memory by blindly writing at recoil offsets.

    bool IsBaseProjectile(uintptr_t heldEntity) {

        if (!IsCanonicalUserPtr(heldEntity)) return false;

        uintptr_t klass = 0;

        if (!SafeReadPtr(heldEntity, &klass)) return false;



        // Walk parent chain via DIRECT MEMORY READS (no il2cpp internals).

        // Il2CppClass layout on Unity 2021+ IL2CPP x64:

        //   +0x10: name  (const char*)

        //   +0x58: parent (Il2CppClass*)

        // esp.cpp already uses classPtr+0x10 to read class names safely --

        // same pattern here. Manual-mapped DLL has no SEH unwind info

        // registered, so we cannot rely on __try/__except; pure SafeRead

        // never faults (RPM on self handles bad addresses gracefully).

        // Use IsCanonicalUserPtr (pure math) inside the loop -- VirtualQuery

        // here would cost ~5us per iteration and kill FPS.

        for (int depth = 0; depth < 16; depth++) {

            if (!IsCanonicalUserPtr(klass)) return false;

            uintptr_t namePtr = 0;

            if (SafeReadPtr(klass + 0x10, &namePtr) && IsCanonicalUserPtr(namePtr)) {

                char name[64] = {0};

                if (SafeRead(namePtr, name, sizeof(name) - 1) &&

                    strcmp(name, "BaseProjectile") == 0) return true;

            }

            uintptr_t parent = 0;

            if (!SafeReadPtr(klass + 0x58, &parent) || parent == 0) return false;

            klass = parent;

        }

        return false;

    }



    // clActiveItem is encrypted in this build (see 2026-04-17 dump), so we

    // can't trust a raw uint32_t read as a valid item UID. Instead, walk the

    // whole inventory and return the first item whose heldEntity is a

    // BaseProjectile. In Rust only the active weapon has a non-null

    // heldEntity, so this reliably finds the wielded gun without needing UID.

    static uintptr_t FindHeldWeapon() {

        if (localPlayer == 0) return 0;



        static int diagDumps = 0;

        const bool diag = (diagDumps < 1);



        // ---- Step 1: read inventory pointer ----

        uintptr_t inventory = 0;

        if (!SafeReadPtr(localPlayer + dynInventoryOffset, &inventory) || !inventory) {

            if (diag) {

                AimLog("[RH][aim] FHW: inventory=0 at localPlayer(%p)+0x%X",

                       (void*)localPlayer, dynInventoryOffset);

                diagDumps++;

            }

            return 0;

        }



        // One-time deep diagnostic of the inventory chain.

        // All reads use direct SafeRead (no il2cpp calls) because manual-mapped

        // DLL has no SEH unwind info, so any il2cpp internal access violation

        // crashes the process instead of being caught.

        auto readClassName = [](uintptr_t obj, char* out, size_t outSize) {

            out[0] = '?'; out[1] = 0;

            if (!IsUserReadablePtr(obj)) return;

            uintptr_t klass = 0, namePtr = 0;

            if (!SafeReadPtr(obj, &klass) || !IsUserReadablePtr(klass)) return;

            if (!SafeReadPtr(klass + 0x10, &namePtr) || !IsUserReadablePtr(namePtr)) return;

            SafeRead(namePtr, out, outSize - 1);

            out[outSize - 1] = 0;

        };



        if (diag) {

            char invClass[64];

            readClassName(inventory, invClass, sizeof(invClass));

            AimLog("[RH][aim] FHW: inventory=%p class=%s (localPlayer+0x%X)",

                   (void*)inventory, invClass, dynInventoryOffset);



            // Hex dump of inventory (safe -- SafeRead handles bad ptrs).

            unsigned char rawInv[0x80];

            if (SafeRead(inventory, rawInv, sizeof(rawInv))) {

                char hexLine[256];

                for (int row = 0; row < 8; row++) {

                    int pos = 0;

                    pos += snprintf(hexLine + pos, sizeof(hexLine) - pos,

                                    "[RH][aim] FHW inv+0x%02X:", row * 16);

                    for (int col = 0; col < 16; col += 8) {

                        uintptr_t val = *(uintptr_t*)(rawInv + row * 16 + col);

                        pos += snprintf(hexLine + pos, sizeof(hexLine) - pos,

                                        " %016llX", (unsigned long long)val);

                    }

                    AimLog("%s", hexLine);

                }

            }



            // NOTE: field enumeration via il2cpp.class_get_fields was removed --

            // all inventory field names in this build are obfuscated hashes

            // (%d6fbe...) anyway, and it's a crash risk under manual-map.



            // For every pointer-like field in inventory, dump class + first 0x80 bytes.

            for (int off = 0x10; off <= 0x70; off += 8) {

                uintptr_t ptr = 0;

                SafeReadPtr(inventory + off, &ptr);

                if (!IsUserReadablePtr(ptr)) continue;  // skips encrypted/garbage

                char cname[64];

                readClassName(ptr, cname, sizeof(cname));

                AimLog("[RH][aim] FHW: inv+0x%X -> %p class=%s",

                       off, (void*)ptr, cname);

                unsigned char rawCont[0x80];

                if (SafeRead(ptr, rawCont, sizeof(rawCont))) {

                    char hx[256];

                    for (int row = 0; row < 8; row++) {

                        int pos = 0;

                        pos += snprintf(hx + pos, sizeof(hx) - pos,

                                        "[RH][aim] FHW   cont+0x%02X:", row * 16);

                        for (int col = 0; col < 16; col += 8) {

                            uintptr_t val = *(uintptr_t*)(rawCont + row * 16 + col);

                            pos += snprintf(hx + pos, sizeof(hx) - pos,

                                            " %016llX", (unsigned long long)val);

                        }

                        AimLog("%s", hx);

                    }

                }



                // DEEP DUMP: for each candidate list pointer inside container,

                // dump its first 0x40 bytes + try to interpret as List<T>.

                const int listCandidates[] = { 0x10, 0x20, 0x58 };

                for (int li = 0; li < 3; li++) {

                    uintptr_t cand = 0;

                    if (!SafeReadPtr(ptr + listCandidates[li], &cand)) continue;

                    if (!IsUserReadablePtr(cand)) continue;



                    char candClass[64];

                    readClassName(cand, candClass, sizeof(candClass));



                    unsigned char listBuf[0x40] = {0};

                    if (!SafeRead(cand, listBuf, sizeof(listBuf))) continue;



                    uintptr_t itemsPtr = *(uintptr_t*)(listBuf + 0x10);

                    uint32_t size = *(uint32_t*)(listBuf + 0x18);

                    AimLog("[RH][aim]   cont+0x%02X -> %p class=%s | as-List: items=%p size=%u",

                           listCandidates[li], (void*)cand, candClass,

                           (void*)itemsPtr, size);



                    // If it LOOKS like a valid list (items readable, reasonable size),

                    // dump first few elements.

                    if (IsUserReadablePtr(itemsPtr) && size > 0 && size <= 32) {

                        for (uint32_t i = 0; i < size && i < 6; i++) {

                            uintptr_t el = 0;

                            if (!SafeReadPtr(itemsPtr + 0x20 + i * 8, &el)) break;

                            char elClass[64];

                            readClassName(el, elClass, sizeof(elClass));

                            AimLog("[RH][aim]     [%u] %p class=%s",

                                   i, (void*)el, elClass);

                            // For each element, also dump first 0x30 bytes so

                            // we can find heldEntity field.

                            unsigned char elBuf[0x48] = {0};

                            if (IsUserReadablePtr(el) && SafeRead(el, elBuf, sizeof(elBuf))) {

                                for (int r = 0; r < 9; r++) {

                                    uintptr_t v = *(uintptr_t*)(elBuf + r * 8);

                                    AimLog("[RH][aim]       el+0x%02X: %016llX",

                                           r * 8, (unsigned long long)v);

                                }

                            }

                        }

                    }

                }

            }

        }



        // ---- Step 2: scan containers × lists × items × heldEntity ----

        const uintptr_t containerOffsets[] = {

            (uintptr_t)dynContainerBeltOffset,

            (uintptr_t)offsets::PlayerInventory::container2,

            (uintptr_t)offsets::PlayerInventory::container3,

            0x30, 0x38, 0x40, 0x48, 0x50, 0x68, 0x70,

        };

        const uintptr_t listOffsets[] = {

            (uintptr_t)dynItemListOffset,

            0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,

            0x48, 0x50, 0x58, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90,

        };

        // Keep the heldEntity probe tight -- every extra offset produces

        // false-positive pointers that reach IsBaseProjectile and risk crashes.

        const uintptr_t heldEntityOffsets[] = {

            (uintptr_t)dynHeldEntityOffset,             // 0x40 (May 2026 build)

            (uintptr_t)offsets::Item::heldEntity2,      // 0x80

            (uintptr_t)offsets::Item::heldEntityAlt,    // 0xB0 (legacy fallback)

            0x48, 0x50, 0x60, 0x70, 0x98, 0xA8,

        };



        static int lastFoundContainerOff = -1;

        static int lastFoundListOff = -1;

        static int lastFoundHeldOff = -1;



        for (int c = 0; c < (int)(sizeof(containerOffsets) / sizeof(containerOffsets[0])); c++) {

            uintptr_t container = 0;

            if (!SafeReadPtr(inventory + containerOffsets[c], &container)) continue;

            if (!IsCanonicalUserPtr(container)) continue;



            for (int lo = 0; lo < (int)(sizeof(listOffsets) / sizeof(listOffsets[0])); lo++) {

                uintptr_t itemList = 0;

                if (!SafeReadPtr(container + listOffsets[lo], &itemList)) continue;

                if (!IsCanonicalUserPtr(itemList)) continue;



                uintptr_t itemsArray = 0;

                if (!SafeReadPtr(itemList + 0x10, &itemsArray) || !IsCanonicalUserPtr(itemsArray)) continue;



                uint32_t count = 0;

                SafeRead(itemList + 0x18, &count, sizeof(uint32_t));

                if (count == 0 || count > 64) continue;



                for (uint32_t i = 0; i < count; i++) {

                    uintptr_t item = 0;

                    if (!SafeReadPtr(itemsArray + 0x20 + (i * 8), &item)) continue;

                    if (!IsCanonicalUserPtr(item)) continue;



                    for (int ho = 0; ho < (int)(sizeof(heldEntityOffsets) / sizeof(heldEntityOffsets[0])); ho++) {

                        uintptr_t held = 0;

                        if (!SafeReadPtr(item + heldEntityOffsets[ho], &held)) continue;

                        if (!IsCanonicalUserPtr(held)) continue;

                        if (!IsBaseProjectile(held)) continue;



                        if (lastFoundContainerOff != (int)containerOffsets[c] ||

                            lastFoundListOff != (int)listOffsets[lo] ||

                            lastFoundHeldOff != (int)heldEntityOffsets[ho]) {

                            AimLog("[RH][aim] FindHeldWeapon: matched cont+0x%X list+0x%X held+0x%X -> %p",

                                   (int)containerOffsets[c], (int)listOffsets[lo],

                                   (int)heldEntityOffsets[ho], (void*)held);

                            lastFoundContainerOff = (int)containerOffsets[c];

                            lastFoundListOff = (int)listOffsets[lo];

                            lastFoundHeldOff = (int)heldEntityOffsets[ho];

                        }

                        return held;

                    }

                }

            }

        }

        if (diag) diagDumps++;

        return 0;

    }



    // Cache of klasses previously confirmed to be BaseProjectile or subclass.

    // Keeps the entity-list scan O(1) per entity after the first occurrence

    // of each weapon type.

    static uintptr_t g_projKlassCache[16] = { 0 };

    static int       g_projKlassCount = 0;



    // Negative cache: klasses we've already rejected. Without this every

    // non-projectile entity (trees, rocks, doors, players, ...) runs the

    // 16-step parent walk every frame -> ~2000 SafeReads for a ~130-entity

    // list -> FPS collapse from 144 to 30. With it the per-frame cost drops

    // to ~130 array scans + a handful of parent walks when new klasses appear.

    static uintptr_t g_nonProjKlassCache[256] = { 0 };

    static int       g_nonProjKlassCount = 0;



    static bool IsCachedProjKlass(uintptr_t k) {

        for (int i = 0; i < g_projKlassCount; i++)

            if (g_projKlassCache[i] == k) return true;

        return false;

    }



    static void CacheProjKlass(uintptr_t k) {

        if (!k || IsCachedProjKlass(k)) return;

        if (g_projKlassCount < 16) g_projKlassCache[g_projKlassCount++] = k;

    }



    static bool IsCachedNonProjKlass(uintptr_t k) {

        for (int i = 0; i < g_nonProjKlassCount; i++)

            if (g_nonProjKlassCache[i] == k) return true;

        return false;

    }



    static void CacheNonProjKlass(uintptr_t k) {

        if (!k || IsCachedNonProjKlass(k)) return;

        if (g_nonProjKlassCount < 256) g_nonProjKlassCache[g_nonProjKlassCount++] = k;

    }



    // Walk the klass parent chain via direct memory reads, comparing pointers

    // (fast) to the cached BaseProjectile klass. Returns true for exact match

    // or any subclass (AssaultRifle, Pistol, BoltActionRifle, etc.).

    static bool IsProjectileKlassByPtr(uintptr_t klass) {

        if (!::g_bpKlass || !IsCanonicalUserPtr(klass)) return false;

        for (int d = 0; d < 16; d++) {

            if (klass == ::g_bpKlass) return true;

            uintptr_t parent = 0;

            if (!SafeReadPtr(klass + 0x58, &parent) || parent == 0) return false;

            if (!IsCanonicalUserPtr(parent)) return false;

            klass = parent;

        }

        return false;

    }



    // Apply no-recoil / no-sway / no-spread writes to one BaseProjectile.

    // Uses DirectWriteFloat (no syscall) — these are normal GC-heap objects

    // whose klass pointer we've already validated.

    static void ApplyModsToWeapon(uintptr_t weapon, bool noRec, bool noSway, bool noSpread) {

        if (noRec) {

            uintptr_t recoilProp = 0;

            if (SafeReadPtr(weapon + recoilOffset, &recoilProp) && IsCanonicalUserPtr(recoilProp)) {

                DirectWriteFloat(recoilProp + recoilYawMinOffset,   0.0f);

                DirectWriteFloat(recoilProp + recoilYawMaxOffset,   0.0f);

                DirectWriteFloat(recoilProp + recoilPitchMinOffset, 0.0f);

                DirectWriteFloat(recoilProp + recoilPitchMaxOffset, 0.0f);



                uintptr_t newRecoil = 0;

                if (SafeReadPtr(recoilProp + newRecoilOverrideOffset, &newRecoil) &&

                    IsCanonicalUserPtr(newRecoil))

                {

                    DirectWriteFloat(newRecoil + recoilYawMinOffset,   0.0f);

                    DirectWriteFloat(newRecoil + recoilYawMaxOffset,   0.0f);

                    DirectWriteFloat(newRecoil + recoilPitchMinOffset, 0.0f);

                    DirectWriteFloat(newRecoil + recoilPitchMaxOffset, 0.0f);

                }

            }

        }

        if (noSway) {

            DirectWriteFloat(weapon + aimSwayOffset,      0.0f);

            DirectWriteFloat(weapon + aimSwaySpeedOffset, 0.0f);

        }

        if (noSpread) {

            DirectWriteFloat(weapon + aimConeOffset,            0.0f);

            DirectWriteFloat(weapon + hipAimConeOffset,         0.0f);

            DirectWriteFloat(weapon + aimConePenaltyMaxOffset,  0.0f);

            DirectWriteFloat(weapon + aimPenaltyPerShotOffset,  0.0f);

            if (aimconePenaltyOffset != 0)

                DirectWriteFloat(weapon + aimconePenaltyOffset, 0.0f);

            if (stancePenaltyOffset != 0)

                DirectWriteFloat(weapon + stancePenaltyOffset, 0.0f);

        }

    }



    void ApplyWeaponMods() {

        auto& wcfg = Menu::Get().WeaponCfg;

        if (!wcfg.NoRecoil && !wcfg.NoSway && !wcfg.NoSpread) return;

        if (::g_bpKlass == 0) return;  // il2cpp init hasn't resolved BaseProjectile yet



        static uint64_t tickCount = 0;

        tickCount++;



        // Rate-limit: entity scan every 8 frames. NoSpread is done at shot

        // time in hkDoAttack (zero-cost), so the per-frame scan is just a

        // safety net. NoRecoil/NoSway values are static config — game only

        // restores them on weapon equip, not every frame, so 8-frame interval

        // is plenty. Eliminates 87.5% of the per-frame WPM overhead.

        if ((tickCount & 7ULL) != 1ULL) return;



        const bool verbose = (tickCount % 600ULL) == 1ULL;



        // Iterate ESP's entity-list snapshot. The inventory-based path doesn't

        // work in this build (containers are behind per-field encryption that

        // we don't have the keys for), but BaseProjectile instances appear in

        // the client entity list so we can reach them that way.

        const uintptr_t* entities = nullptr;

        size_t entityCount = ESP::GetEntityList(&entities);

        if (!entities || entityCount == 0) {

            if (verbose) AimLog("[RH][aim] ApplyWeaponMods: entity list empty (not on server yet?)");

            return;

        }



        int found = 0;

        int slowPathRuns = 0;

        for (size_t i = 0; i < entityCount; i++) {

            uintptr_t ent = entities[i];

            if (!IsCanonicalUserPtr(ent)) continue;

            uintptr_t klass = 0;

            if (!SafeReadPtr(ent, &klass) || !IsCanonicalUserPtr(klass)) continue;



            // Fast path (positive): klass already confirmed projectile.

            if (IsCachedProjKlass(klass)) {

                ApplyModsToWeapon(ent, wcfg.NoRecoil, wcfg.NoSway, wcfg.NoSpread);

                found++;

                continue;

            }

            // Fast path (negative): klass already rejected. O(1), no reads.

            if (IsCachedNonProjKlass(klass)) continue;



            // Slow path: parent walk. Happens at most a few times per session

            // per unique klass, then the result is cached.

            slowPathRuns++;

            if (!IsProjectileKlassByPtr(klass)) {

                CacheNonProjKlass(klass);

                continue;

            }

            CacheProjKlass(klass);

            ApplyModsToWeapon(ent, wcfg.NoRecoil, wcfg.NoSway, wcfg.NoSpread);

            found++;

        }



        if (verbose) {

            AimLog("[RH][aim] ApplyWeaponMods: ents=%zu hits=%d slowRuns=%d "

                   "cache=[proj=%d nonProj=%d] (noRec=%d noSway=%d noSpread=%d)",

                   entityCount, found, slowPathRuns,

                   g_projKlassCount, g_nonProjKlassCount,

                   (int)wcfg.NoRecoil, (int)wcfg.NoSway, (int)wcfg.NoSpread);

        }

    }



    void RenderFOV() {

        auto& cfg = Menu::Get().AimCfg;

        if (!cfg.ShowFOV) return;



        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        int w = GetSystemMetrics(SM_CXSCREEN);

        int h = GetSystemMetrics(SM_CYSCREEN);

        dl->AddCircle(ImVec2(w / 2.0f, h / 2.0f), cfg.FOV, IM_COL32(255, 255, 255, 120), 64, 1.2f);

    }

}

