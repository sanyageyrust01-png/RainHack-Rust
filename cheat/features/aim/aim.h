#pragma once
#include <windows.h>
#include <cstdint>

namespace Aim {
    void Update();
    void ApplyWeaponMods();
    void RenderFOV();

    void InitIL2CPPOffsets();

    bool IsLineVisible(float sx, float sy, float sz,
                       float tx, float ty, float tz);

    extern bool currentTargetVisible;

    // Local BasePlayer pointer, populated by ESP::Render every frame from the
    // entity sweep. Exposed so world.cpp can dereference BasePlayer + 0x3F8
    // (ViewModel) to find the viewmodel camera — Rust doesn't update its FOV
    // through Camera::set_fieldOfView at runtime so we have to walk the object
    // graph manually.
    extern uintptr_t localPlayer;

    // Install all shot-level / network-serialization hooks:
    //   - BaseProjectile.LaunchProjectile   (per-shot entry, used for NoSpread)
    //   - BaseProjectile.SimulateAimcone    (NoSpread safety, swallows the call)
    //   - AimConeUtil.GetModifiedAimConeDirection (NoSpread direction override)
    //   - ProtoBuf.ProjectileShoot.WriteToStream  (TRUE silent aim — rewrites
    //     bullet velocity in the network packet before it's sent to server)
    bool InstallShotHooks();

    // True iff LaunchProjectile was successfully hooked (needed for NoSpread).
    extern bool HasShotHook;
    // True iff AimConeUtil.GetModifiedAimConeDirection was hooked (NoSpread).
    extern bool HasAimConeHook;
    // True iff ProtoBuf.ProjectileShoot.WriteToStream was hooked — this is
    // the ONLY mechanism that actually provides silent aim on this build.
    extern bool HasProjShootHook;

    // Install a "modify-only" Hardware Breakpoint at `target`. When the BP
    // fires, the VEH handler invokes `preHook` (which can mutate CPU state
    // via EXCEPTION_POINTERS, e.g. overwrite XMM1 to substitute a float
    // argument) and then resumes the original instruction in place — no
    // detour is invoked. Used for hooks where MinHook page-write fails due
    // to EAC protection (e.g. UnityPlayer.dll Camera::set_fieldOfView).
    // Returns true if the BP was installed on at least one thread; consumes
    // one of the 4 DR slots.
    bool AddHwBpModifyOnly(void* target,
                           void(__stdcall* preHook)(EXCEPTION_POINTERS*),
                           const char* label);

    // Install an "execute-redirect" Hardware Breakpoint at `target`. When
    // it fires, the VEH handler rewrites RIP to `detour`, so the detour
    // function runs INSTEAD of the original (no trampoline, no return-to-
    // original mechanism — caller must implement substitution semantics
    // themselves). Use case: SteamID spoofing on EAC-protected
    // Facepunch.Steamworks methods where MinHook page-write is blocked.
    // Returns true if installed on at least one thread.
    bool AddHwBpExecute(void* target, void* detour, const char* label);

    // How many of the 4 hardware breakpoint slots are currently consumed.
    // UI uses this to gate "Apply & Reconnect" when no slot is free.
    int  GetHwBpUsedSlots();

    struct TracerEvent {
        float startX, startY, startZ;
        float velX,   velY,   velZ;
        unsigned long long spawnTick;
    };

    static const int kMaxTracers = 128;
    extern TracerEvent  g_tracers[kMaxTracers];
    extern volatile int g_tracerHead;
    void PushTracer(float sx, float sy, float sz, float vx, float vy, float vz);
}
