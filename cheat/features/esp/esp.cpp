#include "esp.h"

#include "../offset/offsets.hpp"

#include "../Menu/Menu.h"

#include "../aim/aim.h"

#include <imgui.h>

#include <windows.h>

#include <cstdint>

#include <cstring>

#include <cmath>

#include <cstdio>

#include <cstdarg>

#include <unordered_map>

#include <vector>



bool ESP::bESP = true;

bool ESP::bName = true;

bool ESP::bBox = true;

bool ESP::bHealth = true;

bool ESP::bDistance = true;



// Shared snapshot of the current entity list. Populated inside ESP::Render

// each frame (after decrypting clientEntities/entityList). Other features

// (e.g. aim's no-recoil) read this via ESP::GetEntityList.

uintptr_t g_espEntityPtrs[15000];

size_t    g_espEntityCount = 0;



size_t ESP::GetEntityList(const uintptr_t** outPtr) {

    if (outPtr) *outPtr = g_espEntityPtrs;

    return g_espEntityCount;

}



// Add toggle hotkey

static void CheckHotkeys() {

    static bool f4Pressed = false;

    if (GetAsyncKeyState(VK_F4) & 0x8000) {

        if (!f4Pressed) {

            ESP::bESP = !ESP::bESP;

            f4Pressed = true;

        }

    } else {

        f4Pressed = false;

    }

}



struct Vector2 { float x, y; };

struct Vector3 {

    float x, y, z;

    float Distance(const Vector3& o) const {

        float dx = x - o.x, dy = y - o.y, dz = z - o.z;

        return sqrtf(dx*dx + dy*dy + dz*dz);

    }

};

struct Matrix4x4 { float m[4][4]; };



static void Log(const char* fmt, ...) {

    char buf[512];

    va_list args;

    va_start(args, fmt);

    vsnprintf(buf, sizeof(buf), fmt, args);

    va_end(args);

    OutputDebugStringA(buf);

    OutputDebugStringA("\n");

}



// ---- Safe memory read using ReadProcessMemory ----

// Using RPM on GetCurrentProcess() is safe against access violations and bypasses user-mode VEH

static HANDLE g_hProcess = GetCurrentProcess();



static bool SafeRead(uintptr_t addr, void* out, size_t size) {

    if (!addr || !out) return false;

    SIZE_T bytesRead = 0;

    return ReadProcessMemory(g_hProcess, (LPCVOID)addr, out, size, &bytesRead) && bytesRead == size;

}



static bool SafeReadPtr(uintptr_t addr, uintptr_t* out) {

    return SafeRead(addr, out, sizeof(uintptr_t));

}



static bool SafeReadU32(uintptr_t addr, uint32_t* out) {

    return SafeRead(addr, out, sizeof(uint32_t));

}



static bool SafeReadU64(uintptr_t addr, uint64_t* out) {

    return SafeRead(addr, out, sizeof(uint64_t));

}



static bool SafeReadI32(uintptr_t addr, int32_t* out) {

    return SafeRead(addr, out, sizeof(int32_t));

}



static ImU32 ColorVec4ToU32(const float c[4]) {

    int r = (int)(c[0] * 255.0f); if (r < 0) r = 0; if (r > 255) r = 255;

    int g = (int)(c[1] * 255.0f); if (g < 0) g = 0; if (g > 255) g = 255;

    int b = (int)(c[2] * 255.0f); if (b < 0) b = 0; if (b > 255) b = 255;

    int a = (int)(c[3] * 255.0f); if (a < 0) a = 0; if (a > 255) a = 255;

    return IM_COL32(r, g, b, a);

}



namespace RustFlags {

    static constexpr uint32_t Wounded   = 1u << 6;   // PlayerFlags.Wounded

    static constexpr uint32_t Connected = 1u << 8;   // PlayerFlags.Connected

    static constexpr uint32_t SafeZone  = 1u << 16;  // PlayerFlags.SafeZone

    static constexpr uint32_t Sleeping  = 1u << 4;   // PlayerFlags.Sleeping

}



static bool SafeReadFloat(uintptr_t addr, float* out) {

    return SafeRead(addr, out, sizeof(float));

}



static bool SafeReadVec3(uintptr_t addr, Vector3* out) {

    return SafeRead(addr, out, sizeof(Vector3));

}



static bool SafeReadMatrix(uintptr_t addr, Matrix4x4* out) {

    return SafeRead(addr, out, sizeof(Matrix4x4));

}



// Forward declarations of the Il2Cpp:: globals used by WorldToScreen and

// GetViewMatrix below. The full namespace block (with definitions) lives

// further down in this file.

namespace Il2Cpp {

    typedef void* (*fn_camera_get_main_t)();

    typedef void  (*fn_get_w2c_matrix_inj_t)(void* self, void* outMatrix4x4);

    typedef void  (*fn_get_proj_matrix_inj_t)(void* self, void* outMatrix4x4);

    typedef void  (*fn_get_nonjit_proj_inj_t)(void* self, void* outMatrix4x4);

    typedef void  (*fn_w2s_inj_t)(void* self, void* posVec3, int eye, void* outVec3);

    typedef void* (*fn_get_transform_fwd_t)(void* self, void* method);

    typedef void  (*fn_get_pos_injected_fwd_t)(void* self, float* ret, void* method);

    extern fn_camera_get_main_t       g_fnCameraGetMain;

    extern fn_get_w2c_matrix_inj_t    g_fnGetW2CMatrixInj;

    extern fn_get_proj_matrix_inj_t   g_fnGetProjMatrixInj;

    extern fn_get_nonjit_proj_inj_t   g_fnGetNonJitProjMatrixInj;

    extern fn_w2s_inj_t               g_fnW2SInjected;

    extern fn_get_transform_fwd_t     g_fnGetTransform;

    extern fn_get_pos_injected_fwd_t  g_fnGetPosInjected;

    bool ReadEntityPosition(uintptr_t entity, float out[3]);

}



// Cached pointer to the live UnityEngine.Camera object, refreshed at the start

// of every ESP::Render() call. WorldToScreen() needs it for the icall path.

static volatile uintptr_t g_currentCam = 0;



// ---- World to Screen ----

// Primary path: cached non-jittered VP matrix from GetViewMatrix() (built via

// icalls each frame). This avoids TAA's per-frame sub-pixel jitter that

// `WorldToScreenPoint_Injected` would inherit from the active jittered

// projection — the symptom is ESP boxes shimmering / floating over otherwise

// stationary entities.

//

// Fallback (only if the matrix is uninitialised, e.g. very first frames):

// `WorldToScreenPoint_Injected` icall, which Unity implements correctly but

// uses whichever projection is currently active (jittered when TAA is on).

static bool WorldToScreen(const Vector3& pos, Vector2& screen, const Matrix4x4& matrix, int width, int height) {

    if (std::isnan(pos.x) || std::isnan(pos.y) || std::isnan(pos.z)) return false;

    if (std::isinf(pos.x) || std::isinf(pos.y) || std::isinf(pos.z)) return false;



    float w = matrix.m[0][3] * pos.x + matrix.m[1][3] * pos.y + matrix.m[2][3] * pos.z + matrix.m[3][3];

    // The (w < 0.01) test also catches a zero/uninitialised matrix (w==0),

    // so we skip drawing instead of dividing by zero.

    if (w >= 0.01f) {

        float x = matrix.m[0][0] * pos.x + matrix.m[1][0] * pos.y + matrix.m[2][0] * pos.z + matrix.m[3][0];

        float y = matrix.m[0][1] * pos.x + matrix.m[1][1] * pos.y + matrix.m[2][1] * pos.z + matrix.m[3][1];



        float invw = 1.0f / w;

        screen.x = (width / 2.0f) * (1.0f + x * invw);

        screen.y = (height / 2.0f) * (1.0f - y * invw);

        return true;

    }



    // Fallback: if the matrix produced w<0 (point behind camera) AND we have

    // an icall available, double-check via Unity's own implementation. This

    // handles the rare case where our matrix is stale by a frame.

    if (Il2Cpp::g_fnW2SInjected && g_currentCam) {

        Vector3 inPos = pos;

        Vector3 outPos = {0.0f, 0.0f, 0.0f};

        __try {

            Il2Cpp::g_fnW2SInjected((void*)g_currentCam, &inPos, /*Mono*/2, &outPos);

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            return false;

        }

        if (outPos.z <= 0.01f) return false;

        if (std::isnan(outPos.x) || std::isnan(outPos.y)) return false;

        screen.x = outPos.x;

        screen.y = (float)height - outPos.y;

        return true;

    }



    return false;

}



// ---- ViewMatrix from camera ----

// Column-major 4x4 multiply: out = a * b. Both inputs and output use Unity's

// Matrix4x4 column-major layout (m[col][row]).

static void Mul4x4(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& out) {

    for (int col = 0; col < 4; ++col) {

        for (int row = 0; row < 4; ++row) {

            float s = 0.0f;

            for (int k = 0; k < 4; ++k) {

                s += a.m[k][row] * b.m[col][k];

            }

            out.m[col][row] = s;

        }

    }

}



// Resolve the live UnityEngine.Camera object via the icall the game itself

// uses. Stale GameAssembly typeinfo+staticFields chains break across patches;

// the icall is build-stable.

static uintptr_t GetMainCameraViaIcall() {

    if (!Il2Cpp::g_fnCameraGetMain) return 0;

    __try {

        return (uintptr_t)Il2Cpp::g_fnCameraGetMain();

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return 0;

    }

}



static bool GetViewMatrix(Matrix4x4& outMatrix) {

    uintptr_t cam = GetMainCameraViaIcall();

    if (!cam) {

        // Last-resort fallback to the legacy stale-typeinfo chain. Will

        // typically fail on May 2026+ builds where main_camera::typeinfo

        // points at the wrong static slot.

        uintptr_t ga = (uintptr_t)GetModuleHandleA("GameAssembly.dll");

        if (!ga) return false;

        uintptr_t camClass = 0;

        if (!SafeReadPtr(ga + offsets::main_camera::typeinfo, &camClass) || !camClass) return false;

        uintptr_t staticFields = 0;

        if (!SafeReadPtr(camClass + offsets::main_camera::static_fields, &staticFields) || !staticFields) return false;

        uintptr_t singleton = 0;

        if (!SafeReadPtr(staticFields + offsets::main_camera::instance, &singleton) || !singleton) return false;

        if (!SafeReadPtr(singleton + 0x10, &cam) || !cam) return false;

    }



    // Publish for WorldToScreen()'s icall path.

    g_currentCam = cam;



    // Preferred path: build VP = P * V via icalls. Use the NON-jittered

    // projection when available — Unity's plain projectionMatrix has TAA

    // jitter baked in (sub-pixel offset that flips every frame), which makes

    // ESP boxes shimmer / float over otherwise stationary entities.

    auto projGetter = Il2Cpp::g_fnGetNonJitProjMatrixInj ? Il2Cpp::g_fnGetNonJitProjMatrixInj

                                                        : Il2Cpp::g_fnGetProjMatrixInj;

    if (Il2Cpp::g_fnGetW2CMatrixInj && projGetter) {

        Matrix4x4 view{}, proj{};

        __try {

            Il2Cpp::g_fnGetW2CMatrixInj((void*)cam, &view);

            projGetter((void*)cam, &proj);

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            return false;

        }

        Mul4x4(proj, view, outMatrix);

        return true;

    }



    // Legacy fallback: read Unity's internal cached matrix at +0x30c. Brittle

    // because Unity's Camera C++ struct shifts between patches; only used if

    // the icalls above couldn't be resolved.

    return SafeReadMatrix(cam + offsets::main_camera::viewMatrix, &outMatrix);

}



// Shared data for Aim

namespace Aim {

    uintptr_t currentTarget = 0;

    float bestTargetFOV = 0.0f;

    // Target position (head)

    float targetPosX = 0, targetPosY = 0, targetPosZ = 0;

    // Target world velocity in m/s. Sampled per-frame in ESP::Render from

    // position deltas, smoothed with an EMA filter, clamped to a human-

    // plausible magnitude. Used by the ballistic solver in

    // hkProjectileCreate to lead moving targets.

    float targetVelX = 0, targetVelY = 0, targetVelZ = 0;

    // Local eyes position

    float localEyeX = 0, localEyeY = 0, localEyeZ = 0;

    // Local player pointer

    uintptr_t localPlayer = 0;

    // Last tick (GetTickCount64) when a target was successfully acquired.

    // Shot-hook uses this for sticky targeting — if we saw a target within

    // the last ~500ms we still have valid target coordinates in memory,

    // even if the current frame didn't re-acquire (e.g. ESP::Render skipped

    // the player due to a transient occlusion / frame drop).

    unsigned long long lastTargetTick = 0;

    bool currentTargetVisible = true;

}



static void DrawCornerBox(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float thickness) {

    float w = max.x - min.x;

    float h = max.y - min.y;

    float l = w / 4.0f;



    // Top left

    dl->AddLine(min, ImVec2(min.x + l, min.y), col, thickness);

    dl->AddLine(min, ImVec2(min.x, min.y + l), col, thickness);



    // Top right

    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x - l, min.y), col, thickness);

    dl->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, min.y + l), col, thickness);



    // Bottom left

    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x + l, max.y), col, thickness);

    dl->AddLine(ImVec2(min.x, max.y), ImVec2(min.x, max.y - l), col, thickness);



    // Bottom right

    dl->AddLine(max, ImVec2(max.x - l, max.y), col, thickness);

    dl->AddLine(max, ImVec2(max.x, max.y - l), col, thickness);

}



// Returns the world-space position of the active camera (eye level). XZ is

// identical to the local player's body footprint, so callers use it for

// self-skip via horizontal distance. Tries the build-stable icall path first

// (Camera::get_main + Component::get_transform + Transform::get_position) and

// falls back to the legacy static-chain only if g_currentCam isn't published

// yet (e.g. very first frame before GetViewMatrix runs).

static bool GetLocalPosition(Vector3& outPos) {

    // Icall path — preferred. g_currentCam is refreshed each frame inside

    // GetViewMatrix() so it stays valid even if the static-singleton offset

    // shifts between Rust builds.

    uintptr_t cam = g_currentCam;

    if (cam && Il2Cpp::g_fnGetTransform && Il2Cpp::g_fnGetPosInjected) {

        float p[3] = {0,0,0};

        if (Il2Cpp::ReadEntityPosition(cam, p)) {

            outPos.x = p[0]; outPos.y = p[1]; outPos.z = p[2];

            if (outPos.x != 0.0f || outPos.y != 0.0f || outPos.z != 0.0f) return true;

        }

    }



    // Legacy static-chain fallback (often fails on May 2026+ where the

    // typeinfo RVA shifted).

    uintptr_t ga = (uintptr_t)GetModuleHandleA("GameAssembly.dll");

    if (!ga) return false;



    uintptr_t camClass = 0;

    if (!SafeReadPtr(ga + offsets::main_camera::typeinfo, &camClass) || !camClass) return false;



    uintptr_t staticFields = 0;

    if (!SafeReadPtr(camClass + offsets::main_camera::static_fields, &staticFields) || !staticFields) return false;



    uintptr_t singleton = 0;

    if (!SafeReadPtr(staticFields + offsets::main_camera::instance, &singleton) || !singleton) return false;



    uintptr_t camInstance = 0;

    if (!SafeReadPtr(singleton + 0x10, &camInstance) || !camInstance) return false;



    return SafeReadVec3(camInstance + offsets::main_camera::position, &outPos);

}



// ---- Il2Cpp Dynamic Resolver ----

namespace Il2Cpp {

    typedef void* (*t_il2cpp_domain_get)();

    typedef void** (*t_il2cpp_domain_get_assemblies)(void* domain, size_t* size);

    typedef void* (*t_il2cpp_assembly_get_image)(void* assembly);

    typedef void* (*t_il2cpp_class_from_name)(void* image, const char* namespaze, const char* name);

    typedef void* (*t_il2cpp_class_get_method_from_name)(void* klass, const char* name, int argsCount);

    typedef void* (*t_il2cpp_runtime_invoke)(void* method, void* obj, void** params, void** exc);

    typedef void* (*t_il2cpp_thread_attach)(void* domain);

    typedef void* (*t_il2cpp_class_get_fields)(void* klass, void** iter);

    typedef const char* (*t_il2cpp_field_get_name)(void* field);

    typedef size_t (*t_il2cpp_field_get_offset)(void* field);

    typedef void* (*t_il2cpp_class_get_methods)(void* klass, void** iter);

    typedef const char* (*t_il2cpp_method_get_name)(void* method);

    typedef void* (*t_il2cpp_gchandle_get_target)(uint32_t handle);



    typedef void* (*t_il2cpp_class_get_type)(void* klass);

    typedef void* (*t_il2cpp_type_get_object)(void* type);



    typedef uint32_t (*t_il2cpp_method_get_param_count)(const void* method);

    typedef const void* (*t_il2cpp_method_get_return_type)(const void* method);

    typedef void* (*t_il2cpp_class_from_type)(const void* type);

    typedef const char* (*t_il2cpp_class_get_name)(void* klass);

    typedef void* (*t_il2cpp_resolve_icall)(const char* name);

    typedef void* (*t_il2cpp_object_new)(void* klass);

    typedef void* (*t_il2cpp_string_new)(const char* str);

    typedef void* (*t_il2cpp_array_new)(void* elementTypeInfo, size_t length);



    typedef void* (*fn_get_transform_t)(void* self, void* method);

    typedef void  (*fn_get_pos_injected_t)(void* self, float* ret, void* method);

    typedef void* (*fn_camera_get_main_t)();

    typedef void  (*fn_get_w2c_matrix_inj_t)(void* self, void* outMatrix4x4);

    typedef void  (*fn_get_proj_matrix_inj_t)(void* self, void* outMatrix4x4);



    t_il2cpp_domain_get domain_get = nullptr;

    t_il2cpp_domain_get_assemblies domain_get_assemblies = nullptr;

    t_il2cpp_assembly_get_image assembly_get_image = nullptr;

    t_il2cpp_class_from_name class_from_name = nullptr;

    t_il2cpp_class_get_method_from_name class_get_method_from_name = nullptr;

    t_il2cpp_runtime_invoke runtime_invoke = nullptr;

    t_il2cpp_thread_attach thread_attach = nullptr;

    t_il2cpp_class_get_fields class_get_fields = nullptr;

    t_il2cpp_field_get_name field_get_name = nullptr;

    t_il2cpp_field_get_offset field_get_offset = nullptr;

    t_il2cpp_class_get_methods class_get_methods = nullptr;

    t_il2cpp_method_get_name method_get_name = nullptr;

    t_il2cpp_gchandle_get_target gchandle_get_target = nullptr;

    t_il2cpp_class_get_type class_get_type = nullptr;

    t_il2cpp_type_get_object type_get_object = nullptr;

    t_il2cpp_method_get_param_count method_get_param_count = nullptr;

    t_il2cpp_method_get_return_type method_get_return_type = nullptr;

    t_il2cpp_class_from_type class_from_type = nullptr;

    t_il2cpp_class_get_name class_get_name = nullptr;

    t_il2cpp_resolve_icall resolve_icall = nullptr;

    t_il2cpp_object_new    object_new    = nullptr;

    t_il2cpp_string_new    string_new    = nullptr;

    t_il2cpp_array_new     array_new     = nullptr;



    fn_get_transform_t    g_fnGetTransform   = nullptr;

    fn_get_pos_injected_t g_fnGetPosInjected = nullptr;

    fn_camera_get_main_t       g_fnCameraGetMain          = nullptr;

    fn_get_w2c_matrix_inj_t    g_fnGetW2CMatrixInj        = nullptr;

    fn_get_proj_matrix_inj_t   g_fnGetProjMatrixInj       = nullptr;

    fn_get_nonjit_proj_inj_t   g_fnGetNonJitProjMatrixInj = nullptr;

    fn_w2s_inj_t               g_fnW2SInjected            = nullptr;



    bool ReadEntityPosition(uintptr_t entity, float out[3]) {

        if (!entity || !g_fnGetTransform || !g_fnGetPosInjected) return false;

        __try {

            void* tr = g_fnGetTransform((void*)entity, nullptr);

            if (!tr) return false;

            if ((uintptr_t)tr < 0x10000ULL || (uintptr_t)tr >= 0x0000800000000000ULL) return false;

            float p[3] = {0,0,0};

            g_fnGetPosInjected(tr, p, nullptr);

            if (std::isnan(p[0]) || std::isnan(p[1]) || std::isnan(p[2])) return false;

            out[0] = p[0]; out[1] = p[1]; out[2] = p[2];

            return true;

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            return false;

        }

    }



    bool Init() {

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!ga) return false;



        domain_get = (t_il2cpp_domain_get)GetProcAddress(ga, "il2cpp_domain_get");

        domain_get_assemblies = (t_il2cpp_domain_get_assemblies)GetProcAddress(ga, "il2cpp_domain_get_assemblies");

        assembly_get_image = (t_il2cpp_assembly_get_image)GetProcAddress(ga, "il2cpp_assembly_get_image");

        class_from_name = (t_il2cpp_class_from_name)GetProcAddress(ga, "il2cpp_class_from_name");

        class_get_method_from_name = (t_il2cpp_class_get_method_from_name)GetProcAddress(ga, "il2cpp_class_get_method_from_name");

        runtime_invoke = (t_il2cpp_runtime_invoke)GetProcAddress(ga, "il2cpp_runtime_invoke");

        

        thread_attach = (t_il2cpp_thread_attach)GetProcAddress(ga, "il2cpp_thread_attach");

        class_get_fields = (t_il2cpp_class_get_fields)GetProcAddress(ga, "il2cpp_class_get_fields");

        field_get_name = (t_il2cpp_field_get_name)GetProcAddress(ga, "il2cpp_field_get_name");

        field_get_offset = (t_il2cpp_field_get_offset)GetProcAddress(ga, "il2cpp_field_get_offset");

        

        class_get_methods = (t_il2cpp_class_get_methods)GetProcAddress(ga, "il2cpp_class_get_methods");

        method_get_name = (t_il2cpp_method_get_name)GetProcAddress(ga, "il2cpp_method_get_name");

        gchandle_get_target = (t_il2cpp_gchandle_get_target)GetProcAddress(ga, "il2cpp_gchandle_get_target");

        

        class_get_type = (t_il2cpp_class_get_type)GetProcAddress(ga, "il2cpp_class_get_type");

        type_get_object = (t_il2cpp_type_get_object)GetProcAddress(ga, "il2cpp_type_get_object");



        method_get_param_count = (t_il2cpp_method_get_param_count)GetProcAddress(ga, "il2cpp_method_get_param_count");

        method_get_return_type = (t_il2cpp_method_get_return_type)GetProcAddress(ga, "il2cpp_method_get_return_type");

        class_from_type = (t_il2cpp_class_from_type)GetProcAddress(ga, "il2cpp_class_from_type");

        class_get_name = (t_il2cpp_class_get_name)GetProcAddress(ga, "il2cpp_class_get_name");

        resolve_icall = (t_il2cpp_resolve_icall)GetProcAddress(ga, "il2cpp_resolve_icall");

        object_new    = (t_il2cpp_object_new)GetProcAddress(ga, "il2cpp_object_new");

        string_new    = (t_il2cpp_string_new)GetProcAddress(ga, "il2cpp_string_new");

        array_new     = (t_il2cpp_array_new)GetProcAddress(ga, "il2cpp_array_new");



        if (thread_attach && domain_get) {

            thread_attach(domain_get());

        }



        if (resolve_icall) {

            g_fnGetTransform = (fn_get_transform_t)resolve_icall(

                "UnityEngine.Component::get_transform()");

            g_fnGetPosInjected = (fn_get_pos_injected_t)resolve_icall(

                "UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)");

            g_fnCameraGetMain = (fn_camera_get_main_t)resolve_icall(

                "UnityEngine.Camera::get_main()");

            g_fnGetW2CMatrixInj = (fn_get_w2c_matrix_inj_t)resolve_icall(

                "UnityEngine.Camera::get_worldToCameraMatrix_Injected(UnityEngine.Matrix4x4&)");

            g_fnGetProjMatrixInj = (fn_get_proj_matrix_inj_t)resolve_icall(

                "UnityEngine.Camera::get_projectionMatrix_Injected(UnityEngine.Matrix4x4&)");

            g_fnGetNonJitProjMatrixInj = (fn_get_nonjit_proj_inj_t)resolve_icall(

                "UnityEngine.Camera::get_nonJitteredProjectionMatrix_Injected(UnityEngine.Matrix4x4&)");

            // WorldToScreenPoint_Injected: enum nested-type spelling varies by

            // IL2CPP version, so try the common forms in order until one hits.

            const char* w2sSigs[] = {

                "UnityEngine.Camera::WorldToScreenPoint_Injected(UnityEngine.Vector3&,UnityEngine.Camera/MonoOrStereoscopicEye,UnityEngine.Vector3&)",

                "UnityEngine.Camera::WorldToScreenPoint_Injected(UnityEngine.Vector3&,UnityEngine.Camera+MonoOrStereoscopicEye,UnityEngine.Vector3&)",

                "UnityEngine.Camera::WorldToScreenPoint_Injected(UnityEngine.Vector3&,UnityEngine.Camera.MonoOrStereoscopicEye,UnityEngine.Vector3&)",

                "UnityEngine.Camera::WorldToScreenPoint_Injected(UnityEngine.Vector3&,System.Int32,UnityEngine.Vector3&)",

            };

            for (const char* sig : w2sSigs) {

                void* p = resolve_icall(sig);

                if (p) {

                    g_fnW2SInjected = (fn_w2s_inj_t)p;

                    Log("[RH][esp] WorldToScreenPoint_Injected resolved via '%s' -> %p", sig, p);

                    break;

                }

            }

        }

        Log("[RH][esp] Camera icalls: getMain=%p w2c=%p proj=%p nonJitProj=%p w2s=%p",

            (void*)g_fnCameraGetMain, (void*)g_fnGetW2CMatrixInj, (void*)g_fnGetProjMatrixInj,

            (void*)g_fnGetNonJitProjMatrixInj, (void*)g_fnW2SInjected);



        return domain_get && class_from_name && runtime_invoke;

    }



    void* FindClass(const char* namespaze, const char* className) {

        if (!domain_get || !domain_get_assemblies || !class_from_name) return nullptr;

        void* domain = domain_get();

        size_t size = 0;

        void** assemblies = domain_get_assemblies(domain, &size);

        for (size_t i = 0; i < size; i++) {

            void* img = assembly_get_image(assemblies[i]);

            void* klass = class_from_name(img, namespaze, className);

            if (klass) return klass;

        }

        return nullptr;

    }



    // Finds a method by its return class name (useful for obfuscated names!)

    void* FindMethodByReturnType(void* klass, const char* expectedReturnClass) {

        if (!klass || !class_get_methods || !method_get_param_count || !method_get_return_type || !class_from_type || !class_get_name) return nullptr;

        

        void* iter = nullptr;

        while (void* method = class_get_methods(klass, &iter)) {

            // Getter methods usually have 0 parameters

            if (method_get_param_count(method) == 0) {

                const void* retType = method_get_return_type(method);

                if (retType) {

                    void* retClass = class_from_type(retType);

                    if (retClass) {

                        const char* className = class_get_name(retClass);

                        if (className && strcmp(className, expectedReturnClass) == 0) {

                            return method;

                        }

                    }

                }

            }

        }

        return nullptr;

    }



    void DumpFields(void* klass) {

        if (!klass || !class_get_fields || !field_get_name || !field_get_offset) return;

        void* iter = nullptr;

        Log("[RH] Dumping fields for class %p:", klass);

        while (void* field = class_get_fields(klass, &iter)) {

            const char* name = field_get_name(field);

            size_t offset = field_get_offset(field);

            Log("[RH]   Field: %s at 0x%X", name, (uint32_t)offset);

        }

    }



    void DumpMethods(void* klass) {

        if (!klass || !class_get_methods || !method_get_name) return;

        void* iter = nullptr;

        Log("[RH] Dumping methods for class %p:", klass);

        while (void* method = class_get_methods(klass, &iter)) {

            const char* name = method_get_name(method);

            Log("[RH]   Method: %s", name);

        }

    }



    uintptr_t CallMethod(void* klass, const char* methodName) {

        if (!klass || !class_get_method_from_name || !runtime_invoke) return 0;

        void* method = class_get_method_from_name(klass, methodName, 0);

        if (!method) return 0;

        void* res = runtime_invoke(method, nullptr, nullptr, nullptr);

        return (uintptr_t)res;

    }



    // Advanced: Find objects using Unity's internal engine

    uintptr_t FindAllEntities(void* bnKlass) {

        if (!bnKlass || !class_get_type || !type_get_object) return 0;

        

        static void* findMethod = nullptr;

        if (!findMethod) {

            void* objKlass = FindClass("UnityEngine", "Object");

            if (objKlass) {

                // FindObjectsOfType(Type type)

                findMethod = class_get_method_from_name(objKlass, "FindObjectsOfType", 1);

            }

        }

        if (!findMethod) return 0;



        void* type = class_get_type(bnKlass);

        if (!type) return 0;

        void* typeObj = type_get_object(type);

        if (!typeObj) return 0;



        void* params[] = { typeObj };

        void* res = runtime_invoke(findMethod, nullptr, params, nullptr);

        return (uintptr_t)res; // Returns an Il2CppArray*

    }



    const char* GetClassName(void* obj) {

        HMODULE ga = GetModuleHandleA("GameAssembly.dll");

        if (!obj || !ga) return "Unknown";

        // In Il2Cpp, the first pointer of an object is to its Il2CppClass

        uintptr_t klass = 0;

        if (!SafeReadPtr((uintptr_t)obj, &klass) || !klass) return "Unknown";

        

        // Il2CppClass has a name pointer at 0x10 (usually)

        uintptr_t namePtr = 0;

        if (!SafeReadPtr(klass + 0x10, &namePtr) || !namePtr) return "Unknown";

        

        static char nameBuf[64];

        if (SafeRead(namePtr, nameBuf, 63)) {

            nameBuf[63] = 0;

            return nameBuf;

        }

        return "Unknown";

    }



    uintptr_t GetTargetFromHandle(uintptr_t handle) {

        if (!gchandle_get_target || handle == 0) return 0;

        return (uintptr_t)gchandle_get_target((uint32_t)handle);

    }

}



namespace EntCat {

    enum Kind { Unknown, Player, Vehicle, Deployable };



    struct VehMatch {

        const char* sub;

        int   subtype;

        const char* label;

        float h;

        float w;

    };



    static const VehMatch kVehicles[] = {

        { "PatrolHelicopter",         0, "PATROL HELI",  4.0f, 2.5f },

        { "BradleyAPC",               1, "BRADLEY",      3.0f, 2.0f },

        { "DeliveryDrone",            2, "DRONE",        1.0f, 1.4f },

        { "CargoShip",                3, "CARGO SHIP",   8.0f, 6.0f },

        { "Minicopter",               4, "MINI",         2.5f, 1.6f },

        { "ScrapTransportHelicopter", 5, "SCRAP HELI",   3.5f, 2.5f },

        { "AttackHelicopter",         6, "ATTACK HELI",  3.0f, 2.5f },

        { "MotorRowboat",             7, "ROWBOAT",      2.0f, 1.6f },

        { "BaseSubmarine",            8, "SUBMARINE",    2.0f, 2.0f },

        { "Tugboat",                  9, "TUGBOAT",      4.0f, 4.0f },

        { "Bike",                    10, "BIKE",         1.6f, 1.2f },

        { "Drone",                    2, "DRONE",        1.0f, 1.4f },

    };



    struct DepMatch {

        const char* sub;

        int   subtype;

        const char* label;

    };



    static const DepMatch kDeployables[] = {

        { "Recycler",         0, "RECYCLER"  },

        { "BuildingPrivlidge",1, "TC"        },

        { "StashContainer",   2, "STASH"     },

        { "SleepingBag",      3, "BAG"       },

        { "RFReceiver",       4, "RF RX"     },

        { "RFBroadcaster",    5, "RF TX"     },

        { "HBHFSensor",       6, "HBHF"      },

        { "SeismicSensor",    7, "SEISMIC"   },

        { "ElectricBattery",  8, "BATTERY"   },

        { "Workbench",        9, "WORKBENCH" },

        // Common deployables added May 2026 build pass — same subtypes 1/2/9

        // already cover most user toggles, the rest map to existing categories.

        { "BoxStorage",       2, "BOX"       },

        { "BaseOven",         9, "OVEN"      },

        { "Furnace",          9, "FURNACE"   },

        { "ResearchTable",    9, "RESEARCH"  },

        { "RepairBench",      9, "REPAIR"    },

        { "MixingTable",      9, "MIXING"    },

        { "AutoTurret",       6, "TURRET"    },

    };



    inline bool IsVehSubtypeOn(const Menu::VisualConfig::VehicleESPSettings& v, int sub) {

        switch (sub) {

            case  0: return v.PatrolHeli;

            case  1: return v.Bradley;

            case  2: return v.Drones;

            case  3: return v.CargoShip;

            case  4: return v.Minicopter;

            case  5: return v.ScrapHeli;

            case  6: return v.AttackHeli;

            case  7: return v.Rowboat;

            case  8: return v.Submarine;

            case  9: return v.TugBoat;

            case 10: return v.Bikes;

        }

        return false;

    }



    inline bool IsDepSubtypeOn(const Menu::VisualConfig::DeployableESPSettings& d, int sub) {

        switch (sub) {

            case 0: return d.Recycler;

            case 1: return d.Cupboard;

            case 2: return d.Stashes;

            case 3: return d.SleepingBag;

            case 4: return d.RFReceiver;

            case 5: return d.RFBroadcaster;

            case 6: return d.HBHFSensor;

            case 7: return d.SeismicSensor;

            case 8: return d.LargeBattery;

            case 9: return d.Workbench;

        }

        return false;

    }



    struct ClassResult {

        Kind kind = Unknown;

        int  subtype = -1;

        const char* label = nullptr;

        float h = 1.5f;

        float w = 1.5f;

    };



    inline ClassResult Classify(const char* className) {

        ClassResult r;

        if (!className) return r;

        for (const auto& v : kVehicles) {

            if (strcmp(className, v.sub) == 0) {

                r.kind = Vehicle; r.subtype = v.subtype;

                r.label = v.label; r.h = v.h; r.w = v.w;

                return r;

            }

        }

        for (const auto& d : kDeployables) {

            if (strcmp(className, d.sub) == 0) {

                r.kind = Deployable; r.subtype = d.subtype;

                r.label = d.label;

                return r;

            }

        }

        return r;

    }

}



namespace Chams {

    enum Cat { CatPlayer = 0, CatFriendly, CatNPC, CatVehicle, CatDeployable, CatCount };



    struct Color4 { float r, g, b, a; };



    struct RendBinding {

        void* renderer = nullptr;

        void* origMats = nullptr;

    };



    struct EntState {

        std::vector<RendBinding> renderers;

        void* lodGroup    = nullptr;

        bool  matApplied  = false;

        bool  cullForced  = false;

        int   activeCat   = -1;

        bool  rendersResolved = false;

        uint32_t lastSeen = 0;

    };



    static void* g_shaderClass     = nullptr;

    static void* g_materialClass   = nullptr;

    static void* g_rendererClass   = nullptr;

    static void* g_lodGroupClass   = nullptr;

    static void* g_componentClass  = nullptr;

    static void* g_transformClass  = nullptr;

    static void* g_gameObjectClass = nullptr;



    static void* g_mShaderFind         = nullptr;

    static void* g_mMaterialCtor       = nullptr;

    static void* g_mMaterialSetColor   = nullptr;

    static void* g_mMaterialSetInt     = nullptr;

    static void* g_mMaterialSetFloat   = nullptr;

    static void* g_mMaterialEnableKw   = nullptr;

    static void* g_mMaterialSetRQ      = nullptr;

    static void* g_mRendererSetMat     = nullptr;

    static void* g_mRendererSetMats    = nullptr;

    static void* g_mRendererGetMat     = nullptr;

    static void* g_mRendererGetMats    = nullptr;

    static void* g_mLodForce           = nullptr;

    static void* g_mGetCompInChildren  = nullptr;

    static void* g_mGetCompsInChildren = nullptr;

    static void* g_mCompGetGameObject  = nullptr;

    static void* g_mCompGetTransform   = nullptr;

    static void* g_mTransformChildCount = nullptr;

    static void* g_mTransformGetChild   = nullptr;

    static void* g_mGOGetComponent      = nullptr;

    static void* g_mGOGetTransform      = nullptr;



    static void* g_rendererTypeObj = nullptr;

    static void* g_lodGroupTypeObj = nullptr;



    static void*    g_basePlayerClass = nullptr;

    static int      g_playerModelOff  = 0x548;

    static int      g_playerModelOffResolved = 0;



    static void* g_matOcc[CatCount] = { nullptr };

    static void* g_matVis[CatCount] = { nullptr };

    static int   g_matStyle[CatCount] = { -1, -1, -1, -1, -1 };



    static std::unordered_map<uintptr_t, EntState> g_states;

    static uint32_t g_frameCounter = 0;



    static bool   g_initialized = false;

    static bool   g_initFailed  = false;



    static void ChamsLog(const char* fmt, ...) {

        char buf[512];

        va_list a; va_start(a, fmt);

        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, a);

        va_end(a);

        OutputDebugStringA(buf);

    }



    static void* InvokeStatic(void* method, void** args) {

        if (!Il2Cpp::runtime_invoke || !method) return nullptr;

        void* exc = nullptr;

        return Il2Cpp::runtime_invoke(method, nullptr, args, &exc);

    }



    static void* Invoke(void* method, void* obj, void** args) {

        if (!Il2Cpp::runtime_invoke || !method || !obj) return nullptr;

        void* exc = nullptr;

        void* r = nullptr;

        __try { r = Il2Cpp::runtime_invoke(method, obj, args, &exc); }

        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }

        return r;

    }



    static void* GetTypeObject(void* klass) {

        if (!klass || !Il2Cpp::class_get_type || !Il2Cpp::type_get_object) return nullptr;

        void* tp = Il2Cpp::class_get_type(klass);

        if (!tp) return nullptr;

        return Il2Cpp::type_get_object(tp);

    }



    bool Init() {

        if (g_initialized) return true;

        if (g_initFailed)  return false;



        if (!Il2Cpp::runtime_invoke || !Il2Cpp::class_get_method_from_name

            || !Il2Cpp::object_new || !Il2Cpp::string_new) {

            g_initFailed = true;

            ChamsLog("[RH][chams] missing core il2cpp exports\n");

            return false;

        }



        g_shaderClass     = Il2Cpp::FindClass("UnityEngine", "Shader");

        g_materialClass   = Il2Cpp::FindClass("UnityEngine", "Material");

        g_rendererClass   = Il2Cpp::FindClass("UnityEngine", "Renderer");

        g_lodGroupClass   = Il2Cpp::FindClass("UnityEngine", "LODGroup");

        g_componentClass  = Il2Cpp::FindClass("UnityEngine", "Component");

        g_transformClass  = Il2Cpp::FindClass("UnityEngine", "Transform");

        g_gameObjectClass = Il2Cpp::FindClass("UnityEngine", "GameObject");



        if (!g_shaderClass || !g_materialClass || !g_rendererClass

            || !g_lodGroupClass || !g_componentClass

            || !g_transformClass || !g_gameObjectClass) {

            g_initFailed = true;

            ChamsLog("[RH][chams] class lookup failed: sh=%p mat=%p rnd=%p lod=%p cmp=%p tr=%p go=%p\n",

                     g_shaderClass, g_materialClass, g_rendererClass,

                     g_lodGroupClass, g_componentClass,

                     g_transformClass, g_gameObjectClass);

            return false;

        }



        g_mShaderFind         = Il2Cpp::class_get_method_from_name(g_shaderClass,    "Find",                    1);

        g_mMaterialCtor       = Il2Cpp::class_get_method_from_name(g_materialClass,  ".ctor",                   1);

        g_mMaterialSetColor   = Il2Cpp::class_get_method_from_name(g_materialClass,  "SetColor",                2);

        g_mMaterialSetInt     = Il2Cpp::class_get_method_from_name(g_materialClass,  "SetInt",                  2);

        g_mMaterialSetFloat   = Il2Cpp::class_get_method_from_name(g_materialClass,  "SetFloat",                2);

        g_mMaterialEnableKw   = Il2Cpp::class_get_method_from_name(g_materialClass,  "EnableKeyword",           1);

        g_mMaterialSetRQ      = Il2Cpp::class_get_method_from_name(g_materialClass,  "set_renderQueue",         1);

        g_mRendererSetMat     = Il2Cpp::class_get_method_from_name(g_rendererClass,  "set_sharedMaterial",      1);

        g_mRendererSetMats    = Il2Cpp::class_get_method_from_name(g_rendererClass,  "set_sharedMaterials",     1);

        g_mRendererGetMat     = Il2Cpp::class_get_method_from_name(g_rendererClass,  "get_sharedMaterial",      0);

        g_mRendererGetMats    = Il2Cpp::class_get_method_from_name(g_rendererClass,  "get_sharedMaterials",     0);

        g_mLodForce           = Il2Cpp::class_get_method_from_name(g_lodGroupClass,  "ForceLOD",                1);

        g_mGetCompInChildren  = Il2Cpp::class_get_method_from_name(g_componentClass, "GetComponentInChildren",  2);

        g_mGetCompsInChildren = Il2Cpp::class_get_method_from_name(g_componentClass, "GetComponentsInChildren", 2);

        if (!g_mGetCompsInChildren) {

            g_mGetCompsInChildren = Il2Cpp::class_get_method_from_name(g_componentClass, "GetComponentsInChildren", 1);

        }



        g_mCompGetGameObject   = Il2Cpp::class_get_method_from_name(g_componentClass,  "get_gameObject",  0);

        g_mCompGetTransform    = Il2Cpp::class_get_method_from_name(g_componentClass,  "get_transform",   0);

        g_mTransformChildCount = Il2Cpp::class_get_method_from_name(g_transformClass,  "get_childCount",  0);

        g_mTransformGetChild   = Il2Cpp::class_get_method_from_name(g_transformClass,  "GetChild",        1);

        g_mGOGetComponent      = Il2Cpp::class_get_method_from_name(g_gameObjectClass, "GetComponent",    1);

        g_mGOGetTransform      = Il2Cpp::class_get_method_from_name(g_gameObjectClass, "get_transform",   0);



        g_rendererTypeObj = GetTypeObject(g_rendererClass);

        g_lodGroupTypeObj = GetTypeObject(g_lodGroupClass);



        g_basePlayerClass = Il2Cpp::FindClass("", "BasePlayer");

        if (g_basePlayerClass && Il2Cpp::class_get_fields

            && Il2Cpp::field_get_name && Il2Cpp::field_get_offset)

        {

            void* iter = nullptr;

            while (true) {

                void* f = nullptr;

                __try { f = Il2Cpp::class_get_fields(g_basePlayerClass, &iter); }

                __except (EXCEPTION_EXECUTE_HANDLER) { break; }

                if (!f) break;

                const char* fn = nullptr;

                __try { fn = Il2Cpp::field_get_name(f); }

                __except (EXCEPTION_EXECUTE_HANDLER) { fn = nullptr; }

                if (!fn) continue;

                if (strcmp(fn, "playerModel") == 0) {

                    int off = (int)Il2Cpp::field_get_offset(f);

                    if (off > 0x100 && off < 0x2000) {

                        g_playerModelOff = off;

                        g_playerModelOffResolved = 1;

                    }

                    break;

                }

            }

        }

        ChamsLog("[RH][chams] BasePlayer=%p playerModel.off=0x%X (resolved=%d)\n",

                 g_basePlayerClass, g_playerModelOff, g_playerModelOffResolved);



        ChamsLog("[RH][chams] methods shFind=%p matCtor=%p setCol=%p setInt=%p\n",

                 g_mShaderFind, g_mMaterialCtor, g_mMaterialSetColor, g_mMaterialSetInt);

        ChamsLog("[RH][chams] methods rndSetM=%p rndSetMs=%p rndGetM=%p rndGetMs=%p\n",

                 g_mRendererSetMat, g_mRendererSetMats, g_mRendererGetMat, g_mRendererGetMats);

        ChamsLog("[RH][chams] methods lodFrc=%p cmpChld=%p cmpsChld=%p tRnd=%p tLod=%p\n",

                 g_mLodForce, g_mGetCompInChildren, g_mGetCompsInChildren,

                 g_rendererTypeObj, g_lodGroupTypeObj);

        ChamsLog("[RH][chams] methods cGO=%p cTr=%p tCC=%p tGC=%p goGC=%p goTr=%p\n",

                 g_mCompGetGameObject, g_mCompGetTransform,

                 g_mTransformChildCount, g_mTransformGetChild,

                 g_mGOGetComponent, g_mGOGetTransform);



        if (!g_mShaderFind || !g_mMaterialCtor || !g_mMaterialSetColor

            || !g_mRendererSetMat || !g_mRendererGetMat

            || !g_mLodForce

            || !g_mGetCompsInChildren

            || !g_rendererTypeObj || !g_lodGroupTypeObj) {

            g_initFailed = true;

            ChamsLog("[RH][chams] required method missing — chams disabled\n");

            return false;

        }



        g_initialized = true;

        return true;

    }



    static const int kZTestAlways    = 8;

    static const int kZTestLessEqual = 4;



    static void* FindShader(const char* name) {

        if (!Il2Cpp::string_new) return nullptr;

        void* nstr = Il2Cpp::string_new(name);

        if (!nstr) return nullptr;

        void* args[1] = { nstr };

        return InvokeStatic(g_mShaderFind, args);

    }



    static void SetMatInt(void* mat, const char* prop, int val) {

        if (!mat || !g_mMaterialSetInt) return;

        void* nm = Il2Cpp::string_new(prop);

        int v = val;

        void* a[2] = { nm, &v };

        Invoke(g_mMaterialSetInt, mat, a);

    }



    static void SetMatFloat(void* mat, const char* prop, float val) {

        if (!mat || !g_mMaterialSetFloat) return;

        void* nm = Il2Cpp::string_new(prop);

        float v = val;

        void* a[2] = { nm, &v };

        Invoke(g_mMaterialSetFloat, mat, a);

    }



    static void SetMatColor(void* mat, const char* prop, const Color4& c) {

        if (!mat || !g_mMaterialSetColor) return;

        void* nm = Il2Cpp::string_new(prop);

        Color4 cc = c;

        void* a[2] = { nm, &cc };

        Invoke(g_mMaterialSetColor, mat, a);

    }



    static void MatEnableKeyword(void* mat, const char* kw) {

        if (!mat || !g_mMaterialEnableKw) return;

        void* s = Il2Cpp::string_new(kw);

        void* a[1] = { s };

        Invoke(g_mMaterialEnableKw, mat, a);

    }



    static void SetRenderQueue(void* mat, int queue) {

        if (!mat || !g_mMaterialSetRQ) return;

        int q = queue;

        void* a[1] = { &q };

        Invoke(g_mMaterialSetRQ, mat, a);

    }



    static void* FindShaderForStyle(int style) {

        static const char* kFlat[] = {

            "Hidden/Internal-Colored", "Sprites/Default", "UI/Default", nullptr

        };

        static const char* kLit[] = {

            "Standard", "HDRP/Lit", "Universal Render Pipeline/Lit",

            "Hidden/Internal-Colored", nullptr

        };

        const char** chain;

        switch (style) {

        case 1:

        case 2:  chain = kLit;  break;

        default: chain = kFlat; break;

        }

        for (int i = 0; chain[i]; i++) {

            void* sh = FindShader(chain[i]);

            if (sh) {

                ChamsLog("[RH][chams] shader '%s' -> %p (style=%d)\n",

                         chain[i], sh, style);

                return sh;

            }

        }

        ChamsLog("[RH][chams] all shader fallbacks failed for style=%d\n", style);

        return nullptr;

    }



    static void* CreateMaterial(int style, int ztest) {

        if (!g_initialized) return nullptr;

        void* shader = FindShaderForStyle(style);

        if (!shader) return nullptr;



        void* mat = Il2Cpp::object_new(g_materialClass);

        if (!mat) return nullptr;

        void* args[1] = { shader };

        Invoke(g_mMaterialCtor, mat, args);



        SetMatInt(mat, "_ZTest", ztest);

        SetMatInt(mat, "_ZWrite", 0);

        SetMatInt(mat, "_Cull", 0);



        switch (style) {

        case 1:

            SetMatFloat(mat, "_Glossiness", 0.0f);

            SetMatFloat(mat, "_Metallic", 0.0f);

            break;

        case 2:

            MatEnableKeyword(mat, "_EMISSION");

            SetMatFloat(mat, "_Glossiness", 0.8f);

            SetMatFloat(mat, "_Metallic", 0.0f);

            SetRenderQueue(mat, 3000);

            break;

        case 3:

            SetMatInt(mat, "_SrcBlend", 5);

            SetMatInt(mat, "_DstBlend", 1);

            SetRenderQueue(mat, 3100);

            break;

        }



        ChamsLog("[RH][chams] material %p style=%d ztest=%d\n", mat, style, ztest);

        return mat;

    }



    static void SetMaterialColor(void* mat, const Color4& c, int style) {

        if (!mat || !g_mMaterialSetColor) return;



        Color4 fc = c;

        if (style == 3) fc.a = c.a * 0.25f;



        SetMatColor(mat, "_Color", fc);

        SetMatColor(mat, "_BaseColor", fc);



        if (style == 2) {

            constexpr float kEmIntensity = 4.0f;

            Color4 em = { c.r * kEmIntensity, c.g * kEmIntensity,

                          c.b * kEmIntensity, 1.0f };

            SetMatColor(mat, "_EmissionColor", em);

        }

    }



    static int  g_diagApplyCount = 0;

    static int  g_diagEnumCount  = 0;

    static bool g_diagPrinted    = false;



    static void* SafeCallGetCompsInChildren(void* target, void* typeObj) {

        if (!target || !typeObj || !g_mGetCompsInChildren) return nullptr;

        void* r = nullptr;

        uint8_t inactive = 1;

        void* args[2] = { typeObj, &inactive };

        void* exc = nullptr;

        __try { r = Il2Cpp::runtime_invoke(g_mGetCompsInChildren, target, args, &exc); }

        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }

        return r;

    }



    static int SafeReadArrLen(void* arr) {

        int r = 0;

        __try {

            uint64_t len = *(uint64_t*)((uintptr_t)arr + 0x18);

            r = (len > 256) ? 0 : (int)len;

        } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }

        return r;

    }



    static void* SafeReadArrElem(void* arr, int idx) {

        void* r = nullptr;

        __try { r = *(void**)((uintptr_t)arr + 0x20 + (size_t)idx * 8); }

        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }

        return r;

    }



    static void* SafeReadField(uintptr_t obj, size_t off) {

        void* r = nullptr;

        __try { r = *(void**)(obj + off); }

        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }

        return r;

    }



    static bool IsBasePlayerEntity(uintptr_t entity) {

        if (!entity || !g_basePlayerClass) return false;

        uintptr_t klass = 0;

        if (!SafeRead(entity, &klass, sizeof(klass)) || !klass) return false;

        if ((void*)klass == g_basePlayerClass) return true;

        uintptr_t parent = klass;

        for (int i = 0; i < 8 && parent; i++) {

            uintptr_t parentLink = 0;

            if (!SafeRead(parent + 0x58, &parentLink, sizeof(parentLink)) || !parentLink) break;

            if ((void*)parentLink == g_basePlayerClass) return true;

            parent = parentLink;

        }

        return false;

    }



    static const char* SafeGetClassName(uintptr_t entity) {

        if (!entity) return "(null)";

        uintptr_t klass = 0;

        if (!SafeRead(entity, &klass, sizeof(klass)) || !klass) return "(noklass)";

        uintptr_t namePtr = 0;

        if (!SafeRead(klass + 0x10, &namePtr, sizeof(namePtr)) || !namePtr) return "(noname)";

        static thread_local char buf[80];

        if (!SafeRead(namePtr, buf, 79)) return "(unreadable)";

        buf[79] = 0;

        return buf;

    }



    static void GetEntityRenderers(uintptr_t entity, std::vector<void*>& out) {

        out.clear();

        if (!entity || !g_rendererTypeObj || !g_mGetCompsInChildren) return;



        void* arr = SafeCallGetCompsInChildren((void*)entity, g_rendererTypeObj);

        int len = arr ? SafeReadArrLen(arr) : 0;

        const char* sourceTag = "entity";



        if ((len <= 0 || len > 128) && IsBasePlayerEntity(entity)) {

            void* pm = SafeReadField(entity, (size_t)g_playerModelOff);

            if (pm && (uintptr_t)pm > 0x10000 && (uintptr_t)pm < 0x800000000000ULL) {

                void* pmArr = SafeCallGetCompsInChildren(pm, g_rendererTypeObj);

                int   pmLen = pmArr ? SafeReadArrLen(pmArr) : 0;

                if (pmLen > 0 && pmLen <= 128) {

                    arr = pmArr;

                    len = pmLen;

                    sourceTag = "PlayerModel";

                }

                if (g_diagEnumCount < 10) {

                    ChamsLog("[RH][chams] via PlayerModel pm=%p off=0x%X arr=%p len=%d\n",

                             pm, g_playerModelOff, pmArr, pmLen);

                }

            }

        }



        if (len <= 0 || len > 128) {

            if (g_diagEnumCount < 10) {

                g_diagEnumCount++;

                ChamsLog("[RH][chams] enum ent=%p klass='%s' renderers=0 (arr=%p len=%d)\n",

                         (void*)entity, SafeGetClassName(entity), arr, len);

            }

            return;

        }



        for (int i = 0; i < len; i++) {

            void* rend = SafeReadArrElem(arr, i);

            if (rend) out.push_back(rend);

        }



        if (g_diagEnumCount < 10) {

            g_diagEnumCount++;

            ChamsLog("[RH][chams] enum ent=%p klass='%s' renderers=%zu (via=%s arr=%p)\n",

                     (void*)entity, SafeGetClassName(entity),

                     out.size(), sourceTag, arr);

        }

    }



    static void* GetEntityLODGroup(uintptr_t entity) {

        if (!entity || !g_mGetCompInChildren || !g_lodGroupTypeObj) return nullptr;

        uint8_t inactive = 1;

        void*   args[2]  = { g_lodGroupTypeObj, &inactive };

        return Invoke(g_mGetCompInChildren, (void*)entity, args);

    }



    static void* GetSharedMaterial(void* renderer) {

        if (!renderer || !g_mRendererGetMat) return nullptr;

        return Invoke(g_mRendererGetMat, renderer, nullptr);

    }



    static void SetSharedMaterial(void* renderer, void* mat) {

        if (!renderer || !g_mRendererSetMat) return;

        void* args[1] = { mat };

        Invoke(g_mRendererSetMat, renderer, args);

    }



    static void ForceLOD(void* lodGroup, int level) {

        if (!lodGroup || !g_mLodForce) return;

        int lv = level;

        void* args[1] = { &lv };

        Invoke(g_mLodForce, lodGroup, args);

    }



    static void EnsureMaterials(int cat, int style) {

        if (cat < 0 || cat >= CatCount) return;

        if (g_matOcc[cat] && g_matVis[cat] && g_matStyle[cat] == style) return;

        g_matOcc[cat]   = CreateMaterial(style, kZTestAlways);

        g_matVis[cat]   = CreateMaterial(style, kZTestLessEqual);

        g_matStyle[cat] = style;

    }



    static void* SafeGetSharedMaterials(void* renderer) {

        void* m = nullptr;

        __try {

            if (renderer && g_mRendererGetMats)

                m = Invoke(g_mRendererGetMats, renderer, nullptr);

        } __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }

        return m;

    }



    static bool SafeSetSharedMaterials(void* renderer, void* matsArr) {

        bool ok = false;

        __try {

            if (renderer && g_mRendererSetMats && matsArr) {

                void* args[1] = { matsArr };

                Invoke(g_mRendererSetMats, renderer, args);

                ok = true;

            }

        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

        return ok;

    }



    static bool SafeSetSharedMaterial(void* renderer, void* mat) {

        bool ok = false;

        __try { SetSharedMaterial(renderer, mat); ok = true; }

        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

        return ok;

    }



    static void* BuildMatsArray(void* mat0, void* mat1) {

        if (!Il2Cpp::array_new || !g_materialClass) return nullptr;

        void* arr = nullptr;

        __try {

            arr = Il2Cpp::array_new(g_materialClass, 2);

            if (arr) {

                *(void**)((uintptr_t)arr + 0x20) = mat0;

                *(void**)((uintptr_t)arr + 0x28) = mat1;

            }

        } __except (EXCEPTION_EXECUTE_HANDLER) { arr = nullptr; }

        return arr;

    }



    void Apply(uintptr_t entity, int cat,

               const float visColor[4], const float occColor[4], int style)

    {

        if (!g_initialized) return;

        if (!entity) return;



        EntState& st = g_states[entity];

        st.lastSeen = g_frameCounter;



        if (!st.rendersResolved) {

            std::vector<void*> rs;

            GetEntityRenderers(entity, rs);

            st.renderers.clear();

            for (void* r : rs) {

                if (!r) continue;

                RendBinding b;

                b.renderer = r;

                b.origMats = SafeGetSharedMaterials(r);

                st.renderers.push_back(b);

            }

            st.rendersResolved = true;

            if (g_diagApplyCount < 10) {

                g_diagApplyCount++;

                ChamsLog("[RH][chams] resolve ent=%p cat=%d renderers=%zu\n",

                         (void*)entity, cat, st.renderers.size());

            }

        }

        if (st.renderers.empty()) return;



        EnsureMaterials(cat, style);

        void* occMat = g_matOcc[cat];

        void* visMat = g_matVis[cat];

        if (!occMat || !visMat) return;



        Color4 oc{ occColor[0], occColor[1], occColor[2], occColor[3] };

        Color4 vc{ visColor[0], visColor[1], visColor[2], visColor[3] };

        SetMaterialColor(occMat, oc, style);

        SetMaterialColor(visMat, vc, style);



        void* matsArr = BuildMatsArray(occMat, visMat);



        int swapped = 0;

        for (auto& b : st.renderers) {

            if (!b.renderer) continue;

            if (matsArr) {

                if (SafeSetSharedMaterials(b.renderer, matsArr)) swapped++;

                else b.renderer = nullptr;

            } else {

                if (SafeSetSharedMaterial(b.renderer, visMat)) swapped++;

                else b.renderer = nullptr;

            }

        }

        st.matApplied = (swapped > 0);

        st.activeCat  = cat;

        if (g_diagApplyCount < 20) {

            g_diagApplyCount++;

            ChamsLog("[RH][chams] swap ent=%p cat=%d ok=%d/%zu arr=%p\n",

                     (void*)entity, cat, swapped, st.renderers.size(), matsArr);

        }

    }



    void DisableCulling(uintptr_t entity, bool on) {

        if (!g_initialized || !entity) return;

        EntState& st = g_states[entity];

        st.lastSeen = g_frameCounter;



        if (st.lodGroup == nullptr) {

            __try {

                st.lodGroup = GetEntityLODGroup(entity);

            } __except (EXCEPTION_EXECUTE_HANDLER) {

                st.lodGroup = nullptr;

            }

        }

        if (!st.lodGroup) return;



        if (on && !st.cullForced) {

            __try { ForceLOD(st.lodGroup, 0); st.cullForced = true; }

            __except (EXCEPTION_EXECUTE_HANDLER) { st.lodGroup = nullptr; }

        } else if (!on && st.cullForced) {

            __try { ForceLOD(st.lodGroup, -1); st.cullForced = false; }

            __except (EXCEPTION_EXECUTE_HANDLER) { st.lodGroup = nullptr; }

        }

    }



    void NextFrame() {

        if (!g_initialized) return;

        g_frameCounter++;



        for (auto it = g_states.begin(); it != g_states.end(); ) {

            EntState& st = it->second;

            uint32_t age = g_frameCounter - st.lastSeen;



            if (age == 1 && st.matApplied) {

                for (auto& b : st.renderers) {

                    if (!b.renderer) continue;

                    if (b.origMats) {

                        __try {

                            void* args[1] = { b.origMats };

                            Invoke(g_mRendererSetMats, b.renderer, args);

                        } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    }

                }

                st.matApplied = false;

                st.activeCat  = -1;

            }

            if (age == 1 && st.cullForced && st.lodGroup) {

                __try { ForceLOD(st.lodGroup, -1); }

                __except (EXCEPTION_EXECUTE_HANDLER) {}

                st.cullForced = false;

            }



            if (age > 30) {

                it = g_states.erase(it);

            } else {

                ++it;

            }

        }

    }

}



namespace Bones {

    static void* g_animatorClass     = nullptr;

    static void* g_animatorTypeObj   = nullptr;

    static void* g_mGetBoneTransInt  = nullptr;

    static void* g_mGetCompInChild   = nullptr;

    static bool  g_initialized       = false;

    static bool  g_initFailed        = false;



    enum HumanBone {

        Hips = 0,

        LeftUpperLeg = 1, RightUpperLeg = 2,

        LeftLowerLeg = 3, RightLowerLeg = 4,

        LeftFoot = 5, RightFoot = 6,

        Spine = 7, Chest = 8,

        Neck = 9, Head = 10,

        LeftShoulder = 11, RightShoulder = 12,

        LeftUpperArm = 13, RightUpperArm = 14,

        LeftLowerArm = 15, RightLowerArm = 16,

        LeftHand = 17, RightHand = 18,

        BoneMax = 19

    };



    struct BonePair { int a, b; };

    static const BonePair g_pairs[] = {

        {Head, Neck}, {Neck, Chest}, {Chest, Spine}, {Spine, Hips},

        {Chest, LeftUpperArm}, {LeftUpperArm, LeftLowerArm}, {LeftLowerArm, LeftHand},

        {Chest, RightUpperArm}, {RightUpperArm, RightLowerArm}, {RightLowerArm, RightHand},

        {Hips, LeftUpperLeg}, {LeftUpperLeg, LeftLowerLeg}, {LeftLowerLeg, LeftFoot},

        {Hips, RightUpperLeg}, {RightUpperLeg, RightLowerLeg}, {RightLowerLeg, RightFoot},

    };

    static const int g_pairCount = sizeof(g_pairs) / sizeof(g_pairs[0]);



    bool Init() {

        if (g_initialized) return true;

        if (g_initFailed) return false;



        g_animatorClass = Il2Cpp::FindClass("UnityEngine", "Animator");

        if (!g_animatorClass) { g_initFailed = true; return false; }



        void* compClass = Il2Cpp::FindClass("UnityEngine", "Component");

        if (!compClass) { g_initFailed = true; return false; }



        g_mGetBoneTransInt = Il2Cpp::class_get_method_from_name(g_animatorClass, "GetBoneTransformInternal", 1);

        g_mGetCompInChild  = Il2Cpp::class_get_method_from_name(compClass, "GetComponentInChildren", 2);



        void* animType = Il2Cpp::class_get_type ? Il2Cpp::class_get_type(g_animatorClass) : nullptr;

        g_animatorTypeObj = (animType && Il2Cpp::type_get_object) ? Il2Cpp::type_get_object(animType) : nullptr;



        if (!g_mGetBoneTransInt || !g_mGetCompInChild || !g_animatorTypeObj) {

            g_initFailed = true;

            Log("[RH][bones] init failed: getBone=%p compChild=%p typeObj=%p",

                g_mGetBoneTransInt, g_mGetCompInChild, g_animatorTypeObj);

            return false;

        }



        g_initialized = true;

        Log("[RH][bones] init OK: getBone=%p compChild=%p typeObj=%p",

            g_mGetBoneTransInt, g_mGetCompInChild, g_animatorTypeObj);

        return true;

    }



    static void* SafeGetAnimator(void* playerModel) {

        if (!playerModel || !g_mGetCompInChild || !g_animatorTypeObj) return nullptr;

        void* result = nullptr;

        uint8_t inactive = 1;

        void* args[2] = { g_animatorTypeObj, &inactive };

        void* exc = nullptr;

        __try {

            result = Il2Cpp::runtime_invoke(g_mGetCompInChild, playerModel, args, &exc);

        } __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }

        return result;

    }



    static bool SafeGetBonePos(void* animator, int boneId, float outPos[3]) {

        if (!animator || !g_mGetBoneTransInt || !Il2Cpp::g_fnGetPosInjected) return false;

        void* boneTrans = nullptr;

        int id = boneId;

        void* bArgs[1] = { &id };

        void* exc = nullptr;

        __try {

            boneTrans = Il2Cpp::runtime_invoke(g_mGetBoneTransInt, animator, bArgs, &exc);

        } __except (EXCEPTION_EXECUTE_HANDLER) { boneTrans = nullptr; }

        if (!boneTrans || (uintptr_t)boneTrans < 0x10000ULL) return false;

        __try {

            Il2Cpp::g_fnGetPosInjected(boneTrans, outPos, nullptr);

        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        return true;

    }

}



namespace ESP {

    uintptr_t cachedBasePlayerClass = 0;

    uintptr_t cachedBaseNetworkableClass = 0;



    void Render() {

        static bool s_renderEntered = false;

        if (!s_renderEntered) { Log("[RH][esp] ESP::Render entered (first call)"); s_renderEntered = true; }



        CheckHotkeys();



        static bool il2cpp_inited = false;

        if (!il2cpp_inited) {

            if (Il2Cpp::Init()) {

                Log("[RH] Il2Cpp dynamic resolver initialized");

                il2cpp_inited = true;

            } else {

                static ULONGLONG lastInitFailLog = 0;

                if (GetTickCount64() - lastInitFailLog > 5000) {

                    Log("[RH][esp] Il2Cpp::Init() returning false (waiting for GameAssembly exports)");

                    lastInitFailLog = GetTickCount64();

                }

            }

        }



        static bool chams_inited = false;

        if (il2cpp_inited && !chams_inited) {

            if (Chams::Init()) {

                Log("[RH] Chams subsystem initialized");

            }

            chams_inited = true;

        }



        static bool bones_inited = false;

        if (il2cpp_inited && !bones_inited) {

            if (Bones::Init()) {

                Log("[RH] Bones subsystem initialized");

            }

            bones_inited = true;

        }



        Chams::NextFrame();



        if (!bESP) {

            static ULONGLONG lastDisabledLog = 0;

            if (GetTickCount64() - lastDisabledLog > 10000) {

                Log("[RH][esp] bESP=false (ESP toggled off in menu or via F4); skipping render");

                lastDisabledLog = GetTickCount64();

            }

            return;

        }



        static void* bnKlass = nullptr;

        if (!bnKlass) {

            bnKlass = Il2Cpp::FindClass("", "BaseNetworkable");

            if (bnKlass) Log("[RH] BaseNetworkable class found: %p", bnKlass);

            else {

                static ULONGLONG lastBnLog = 0;

                if (GetTickCount64() - lastBnLog > 5000) {

                    Log("[RH][esp] FindClass(BaseNetworkable) returned NULL");

                    lastBnLog = GetTickCount64();

                }

            }

        }



        static void* bpKlass = nullptr;

        if (!bpKlass) {

            bpKlass = Il2Cpp::FindClass("", "BasePlayer");

            if (bpKlass) {

                cachedBasePlayerClass = (uintptr_t)bpKlass;

                Log("[RH][esp] BasePlayer class found: %p", bpKlass);

            }

        }



        __try {

            ImDrawList* drawList = ImGui::GetBackgroundDrawList();

            auto& menu = Menu::Get();

            auto& vcfg = menu.VisualCfg;

            drawList->AddText(ImVec2(10, 10), IM_COL32(0, 255, 0, 255), "");



            Matrix4x4 viewMatrix;

            if (!GetViewMatrix(viewMatrix)) {

                static ULONGLONG lastVmLog = 0;

                if (GetTickCount64() - lastVmLog > 5000) {

                    Log("[RH][esp] GetViewMatrix failed (camera not ready)");

                    lastVmLog = GetTickCount64();

                }

                return;

            }



            // Aim target selection: write to LOCALS during the loop, then

            // atomically commit at the end. The previous approach of zeroing

            // Aim::currentTarget at frame start created a race window where

            // hkLaunchProjectile (on the game thread) saw target=0 during

            // the ~1ms between reset and the per-entity update, causing

            // silent-aim to no-op on most shots. x64 guarantees aligned

            // 8-byte pointer writes are atomic, so the final assignment

            // below is race-free.

            float     localBestFOV = menu.AimCfg.FOV;

            uintptr_t localTarget  = 0;

            float     localTargetX = 0.0f, localTargetY = 0.0f, localTargetZ = 0.0f;



            Vector3 localPos = {};

            const bool localPosValid = GetLocalPosition(localPos);

            // Camera position is already at eye level, no offset needed

            Aim::localEyeX = localPos.x;

            Aim::localEyeY = localPos.y;

            Aim::localEyeZ = localPos.z;

            if (!localPosValid) {

                static ULONGLONG s_lastLocalPosWarn = 0;

                if (GetTickCount64() - s_lastLocalPosWarn > 5000) {

                    Log("[RH][esp] GetLocalPosition failed — self-skip disabled this frame");

                    s_lastLocalPosWarn = GetTickCount64();

                }

            }



            uintptr_t targetEntity = 0;

            float minCrosshairDist = 99999.0f;



            int width = GetSystemMetrics(SM_CXSCREEN);

            int height = GetSystemMetrics(SM_CYSCREEN);



            // HARDCODED OFFSETS APPROACH

            // Using offsets from the user's latest dump

            

            uintptr_t ga = (uintptr_t)GetModuleHandleA("GameAssembly.dll");

            uintptr_t bnClassPtr = 0;

            // BaseNetworkable typeinfo RVA in May 2026 build (was 0xe5ca6f8 in Apr build).
            // Static-class hash: %a4a680d5d77e77f98e437fdb67505c3e44f22b9a.

            static ULONGLONG lastDebugLog = 0;

            bool doLog = (GetTickCount64() - lastDebugLog > 5000);

            if (!SafeReadPtr(ga + offsets::base_networkable::typeinfo, &bnClassPtr)) {

                if (doLog) { Log("[RH][esp] BaseNetworkable typeinfo read failed at ga+0x%llx", (unsigned long long)offsets::base_networkable::typeinfo); lastDebugLog = GetTickCount64(); }

                return;

            }

            if (bnClassPtr < 0x1000) {

                if (doLog) { Log("[RH][esp] bnClassPtr garbage/null: %p (typeinfo slot uninitialized)", (void*)bnClassPtr); lastDebugLog = GetTickCount64(); }

                return;

            }

            

            uintptr_t staticFields = 0;

            if (!SafeReadPtr(bnClassPtr + 0xB8, &staticFields)) {

                if (doLog) { Log("[RH][esp] staticFields read failed at klass+0xB8 (klass=%p)", (void*)bnClassPtr); lastDebugLog = GetTickCount64(); }

                return;

            }

            if (staticFields < 0x1000) {

                if (doLog) { Log("[RH][esp] staticFields null/garbage: %p (class not yet inited?)", (void*)staticFields); lastDebugLog = GetTickCount64(); }

                return;

            }

            static bool s_chainReached = false;

            if (!s_chainReached) { Log("[RH][esp] decrypt chain reached: klass=%p staticFields=%p", (void*)bnClassPtr, (void*)staticFields); s_chainReached = true; }



            // 1. Get clientEntities

            uintptr_t ceWrapper = 0;

            // clientEntities wrapper offset within static_fields blob.
            // 0x38 in Apr 2026 build, 0x18 in May 2026 build.

            if (!SafeReadPtr(staticFields + offsets::base_networkable::client_entities, &ceWrapper) || ceWrapper < 0x1000) {

                if (doLog) { Log("[RH] ceWrapper is invalid: %p", (void*)ceWrapper); lastDebugLog = GetTickCount64(); }

                return;

            }

            

            // Wrapper layout (May 2026 build, IDA decompile of sub_180E1B8C0):

            //   +0x10: bool initialized (1 byte) <-- check this  (was +0x14 in Apr build)

            //   +0x18: uint64 encrypted (8 bytes)

            //   +0x20: int counter      (was +0x10 in Apr build)

            uint8_t ceInitialized = 0;

            if (!SafeRead(ceWrapper + 0x10, &ceInitialized, 1) || ceInitialized == 0) {

                if (doLog) { Log("[RH] clientEntities wrapper not initialized. PLEASE JOIN A SERVER!"); lastDebugLog = GetTickCount64(); }

                return;

            }



            uint64_t encryptedCE = 0;

            if (!SafeRead(ceWrapper + 0x18, &encryptedCE, sizeof(uint64_t))) return;



            // Manual Decryption for clientEntities. Algorithm extracted from

            // May 2026 build's sub_180E1B8C0 (RVA 0xE1B8C0). Per-patch keys; do

            // not blindly reuse from previous builds.

            //   x = v + 0x2FAF9B7B

            //   x = ROL(x, 9)            ; (x << 9) | (x >> 23)

            //   x = x - 0x307C0EA5

            //   x = x ^ 0x50BB0B84

            // Applied independently to low and high 32-bit halves.

            auto decrypt_ce = [](uint64_t val) -> uint64_t {

                auto decrypt_half = [](uint32_t v) -> uint32_t {

                    uint32_t a = v + 0x2FAF9B7Bu;

                    a = (a << 9) | (a >> 23);

                    a -= 0x307C0EA5u;

                    a ^= 0x50BB0B84u;

                    return a;

                };

                uint32_t lower = decrypt_half((uint32_t)val);

                uint32_t upper = decrypt_half((uint32_t)(val >> 32));

                return ((uint64_t)upper << 32) | lower;

            };



            uint64_t clientEntitiesDecrypted = decrypt_ce(encryptedCE);

            if (clientEntitiesDecrypted == 0) {

                if (doLog) { Log("[RH] Decrypted CE handle is 0"); lastDebugLog = GetTickCount64(); }

                return;

            }

            

            // May 2026 build: the decrypted qword is always a GC handle in the

            // low 32 bits with anti-analysis garbage in the high 32 bits — see

            // sub_180E1B8C0 which discards the high dword and calls

            // il2cpp_gchandle_get_target(low32). Old `if < 0x10000000` heuristic

            // skipped resolution because the noisy high dword inflated the value.

            uint32_t ceHandle32 = (uint32_t)(clientEntitiesDecrypted & 0xFFFFFFFFull);

            uintptr_t clientEntities = Il2Cpp::GetTargetFromHandle(ceHandle32);

            if (clientEntities < 0x1000) {

                if (doLog) { Log("[RH] Failed to resolve CE GC handle: enc=%llx low=%x", (unsigned long long)clientEntitiesDecrypted, ceHandle32); lastDebugLog = GetTickCount64(); }

                return;

            }



            // 2. Get entityList

            uintptr_t elWrapper = 0;

            // entityList offset: 0x10 (unchanged)

            if (!SafeReadPtr(clientEntities + 0x10, &elWrapper) || elWrapper < 0x1000) {

                if (doLog) { Log("[RH] elWrapper is invalid: %p", (void*)elWrapper); lastDebugLog = GetTickCount64(); }

                return;

            }



            // Same wrapper layout as CE: byte initialized at +0x10, qword encrypted at +0x18.

            uint8_t elInitialized = 0;

            if (!SafeRead(elWrapper + 0x10, &elInitialized, 1) || elInitialized == 0) {

                if (doLog) { Log("[RH] entityList wrapper not initialized."); lastDebugLog = GetTickCount64(); }

                return;

            }



            uint64_t encryptedEL = 0;

            if (!SafeRead(elWrapper + 0x18, &encryptedEL, sizeof(uint64_t))) return;



            // Manual Decryption for entityList. Algorithm extracted from

            // May 2026 build's sub_180E933D0 (RVA 0xE933D0).

            //   x = v ^ 0x7FB09BA3

            //   x = ROL(x, 25)           ; (x << 25) | (x >> 7)

            //   x = x ^ 0x92589FA8

            //   x = ROL(x, 19)           ; (x << 19) | (x >> 13)

            // Applied independently to low and high 32-bit halves.

            auto decrypt_el = [](uint64_t val) -> uint64_t {

                auto decrypt_half = [](uint32_t v) -> uint32_t {

                    uint32_t a = v ^ 0x7FB09BA3u;

                    a = (a << 25) | (a >> 7);

                    a ^= 0x92589FA8u;

                    a = (a << 19) | (a >> 13);

                    return a;

                };

                uint32_t lower = decrypt_half((uint32_t)val);

                uint32_t upper = decrypt_half((uint32_t)(val >> 32));

                return ((uint64_t)upper << 32) | lower;

            };



            uint64_t entityListDecrypted = decrypt_el(encryptedEL);

            if (entityListDecrypted == 0) {

                if (doLog) { Log("[RH] Decrypted EL handle is 0"); lastDebugLog = GetTickCount64(); }

                return;

            }

            

            // Same handle-in-low-32 pattern as CE (sub_180E933D0).

            uint32_t elHandle32 = (uint32_t)(entityListDecrypted & 0xFFFFFFFFull);

            uintptr_t entityList = Il2Cpp::GetTargetFromHandle(elHandle32);

            if (entityList < 0x1000) {

                if (doLog) { Log("[RH] Failed to resolve EL GC handle: enc=%llx low=%x", (unsigned long long)entityListDecrypted, elHandle32); lastDebugLog = GetTickCount64(); }

                return;

            }



            // 3. Get vals (BufferList<BaseNetworkable>)

            // ListDictionary layout in May 2026 build (per IL2CPP dumper):

            //   +0x10: BufferList<TVal>            (values = BaseNetworkable* — what we want)

            // Layout was reshuffled vs. Apr 2026 build, where TVal lived at +0x20

            // and +0x18 was a BufferList<TKey> of ulong NetworkableIds. Always

            // read this offset from offsets::base_networkable::buffer so the

            // value tracks the dumper output for each patch.

            uintptr_t vals = 0;

            if (!SafeReadPtr(entityList + offsets::base_networkable::buffer, &vals)) {

                if (doLog) { Log("[RH][esp] vals read failed at entityList+0x%x", (unsigned)offsets::base_networkable::buffer); lastDebugLog = GetTickCount64(); }

                return;

            }

            if (vals < 0x1000) {

                if (doLog) { Log("[RH][esp] vals invalid: %p (entityList=%p +0x%x)", (void*)vals, (void*)entityList, (unsigned)offsets::base_networkable::buffer); lastDebugLog = GetTickCount64(); }

                return;

            }



            // In BufferList (or standard List<T>), the array pointer is at 0x10 and count is at 0x18

            uintptr_t arrayPtr = 0;

            if (!SafeReadPtr(vals + 0x10, &arrayPtr) || arrayPtr < 0x1000) {

                if (doLog) { Log("[RH] arrayPtr is invalid: %p (vals=%p)", (void*)arrayPtr, (void*)vals); lastDebugLog = GetTickCount64(); }

                return;

            }



            uint32_t entityCount = 0;

            if (!SafeReadU32(vals + 0x18, &entityCount)) {

                if (doLog) { Log("[RH][esp] entityCount read failed at vals+0x18"); lastDebugLog = GetTickCount64(); }

                return;

            }

            

            if (entityCount == 0 || entityCount > 150000) {

                if (doLog) { Log("[RH] Invalid entityCount: %u (entityList=%p vals=%p arr=%p)", entityCount, (void*)entityList, (void*)vals, (void*)arrayPtr); lastDebugLog = GetTickCount64(); }

                return;

            }



            if (doLog) lastDebugLog = GetTickCount64();



            uint32_t readCount = entityCount;

            if (readCount > 15000) readCount = 15000;



            // g_espEntityPtrs / g_espEntityCount are defined at file scope

            // near the top of this file; fully-qualified with :: to avoid

            // accidentally binding inside namespace ESP via ADL.

            if (!SafeRead(arrayPtr + 0x20, ::g_espEntityPtrs, readCount * sizeof(uintptr_t))) {

                if (doLog) { Log("[RH][esp] SafeRead(arr+0x20, count=%u) failed", readCount); lastDebugLog = GetTickCount64(); }

                return;

            }

            ::g_espEntityCount = readCount;

            static bool s_populatedOnce = false;

            if (!s_populatedOnce) {

                Log("[RH][esp] FIRST POPULATION OK: count=%u (arr=%p)", readCount, (void*)arrayPtr);

                s_populatedOnce = true;

            }

            // Local alias so the rest of the function body stays unchanged.

            uintptr_t* entityPtrs = ::g_espEntityPtrs;



            static uint32_t lastLoggedCount = 0;

            // Removed log to prevent packet flooding



            bool doDebugLog = false;



            for (uint32_t i = 0; i < readCount; i++) {

                uintptr_t entity = entityPtrs[i];

                if (!entity) continue;

            __try {



            uintptr_t baseObj = entity; // The elements in BufferList<BaseNetworkable> ARE the BaseNetworkable pointers!



            // Bulletproof self-skip: once localPlayer is known, exclude by pointer

            // identity. This prevents silent aim from targeting the local player

            // (which happens when the camera/feet Y-offset foils the distance-based

            // skip below).

            if (baseObj == Aim::localPlayer) continue;



            // Check class name (using cached class pointer for extreme performance)

            uintptr_t classPtr = 0;

            if (!SafeReadPtr(baseObj + 0x0, &classPtr) || !classPtr) continue;

            

            (void)doDebugLog;



            char classNameBuf[64] = {0};

            {

                uintptr_t namePtr = 0;

                if (!SafeReadPtr(classPtr + 0x10, &namePtr) || !namePtr) continue;

                if (!SafeRead(namePtr, classNameBuf, 63)) continue;

                classNameBuf[63] = 0;

            }



            const bool isCachedPlayer = (cachedBasePlayerClass != 0

                                         && classPtr == cachedBasePlayerClass);

            const bool nameLooksPlayer = (strstr(classNameBuf, "Player") != nullptr

                                          || strstr(classNameBuf, "NPC")    != nullptr);



            if (!isCachedPlayer && !nameLooksPlayer) {

                EntCat::ClassResult cr = EntCat::Classify(classNameBuf);

                if (cr.kind == EntCat::Unknown) continue;



                {

                    auto& cc = vcfg.Chams;

                    if (cr.kind == EntCat::Vehicle && cc.Vehicle.Enabled) {

                        Chams::Apply(baseObj, Chams::CatVehicle,

                                     cc.Vehicle.VisColor, cc.Vehicle.OccColor, cc.Vehicle.Style);

                    } else if (cr.kind == EntCat::Deployable && cc.Deployable.Enabled) {

                        Chams::Apply(baseObj, Chams::CatDeployable,

                                     cc.Deployable.VisColor, cc.Deployable.OccColor, cc.Deployable.Style);

                    }

                }



                bool catEnabled = (cr.kind == EntCat::Vehicle)

                    ? (vcfg.Vehicle.Enabled

                        && EntCat::IsVehSubtypeOn(vcfg.Vehicle, cr.subtype))

                    : (vcfg.Deployable.Enabled

                        && EntCat::IsDepSubtypeOn(vcfg.Deployable, cr.subtype));

                if (!catEnabled) continue;



                float maxDraw = (cr.kind == EntCat::Vehicle)

                    ? vcfg.Vehicle.DrawDistance

                    : vcfg.Deployable.DrawDistance;



                float ePos[3] = {0,0,0};

                if (!Il2Cpp::ReadEntityPosition(baseObj, ePos)) continue;

                Vector3 ePosV{ ePos[0], ePos[1], ePos[2] };

                float eDist = localPos.Distance(ePosV);

                if (eDist > maxDraw) continue;



                float entHealth = 0.0f, entMaxHealth = 0.0f;

                if (cr.kind == EntCat::Vehicle && vcfg.Vehicle.Health) {

                    SafeReadFloat(baseObj + offsets::BaseCombatEntity::health,    &entHealth);

                    SafeReadFloat(baseObj + offsets::BaseCombatEntity::maxHealth, &entMaxHealth);

                    if (entMaxHealth < 1.0f) entMaxHealth = 100.0f;

                }

                if (cr.kind == EntCat::Deployable && vcfg.Deployable.TC_HealthBar

                    && cr.subtype == 1) {

                    SafeReadFloat(baseObj + offsets::BaseCombatEntity::health,    &entHealth);

                    SafeReadFloat(baseObj + offsets::BaseCombatEntity::maxHealth, &entMaxHealth);

                    if (entMaxHealth < 1.0f) entMaxHealth = 1000.0f;

                }



                Vector2 sFoot{}, sHead{};

                Vector3 worldFoot{ ePos[0], ePos[1], ePos[2] };

                Vector3 worldHead{ ePos[0], ePos[1] + cr.h, ePos[2] };

                bool fOK = WorldToScreen(worldFoot, sFoot, viewMatrix, width, height);

                bool hOK = WorldToScreen(worldHead, sHead, viewMatrix, width, height);

                if (!fOK || !hOK) continue;



                float bH = sFoot.y - sHead.y;

                if (bH < 4.0f || std::isnan(bH) || std::isinf(bH)) continue;

                float bW = bH * (cr.w / cr.h);



                ImVec2 boxMin(sHead.x - bW * 0.5f, sHead.y);

                ImVec2 boxMax(sHead.x + bW * 0.5f, sFoot.y);



                ImU32 entColor = (cr.kind == EntCat::Vehicle)

                    ? ColorVec4ToU32(vcfg.Vehicle.Color)

                    : ColorVec4ToU32(vcfg.Deployable.Color);



                int entBoxType = (cr.kind == EntCat::Vehicle)

                    ? vcfg.Vehicle.BoxType : vcfg.Deployable.BoxType;

                bool entShowName = (cr.kind == EntCat::Vehicle)

                    ? vcfg.Vehicle.Name : vcfg.Deployable.Name;

                bool entShowDist = (cr.kind == EntCat::Vehicle)

                    ? vcfg.Vehicle.Distance : vcfg.Deployable.Distance;



                if (entBoxType == 1) {

                    drawList->AddRect(boxMin, boxMax, entColor, 0.0f, 0, 1.5f);

                } else if (entBoxType == 2) {

                    DrawCornerBox(drawList, boxMin, boxMax, entColor, 1.5f);

                } else {

                    drawList->AddCircleFilled(ImVec2(sFoot.x, sFoot.y - bH * 0.5f),

                                              4.0f, entColor, 12);

                }



                if (entHealth > 0.0f && entMaxHealth > 0.0f) {

                    float pct = entHealth / entMaxHealth;

                    if (pct > 1.0f) pct = 1.0f;

                    if (pct < 0.0f) pct = 0.0f;

                    drawList->AddRectFilled(ImVec2(boxMin.x - 6, boxMin.y),

                                            ImVec2(boxMin.x - 2, boxMax.y),

                                            IM_COL32(0,0,0,160));

                    int hr = (int)((1.0f - pct) * 255);

                    int hg = (int)(pct * 255);

                    drawList->AddRectFilled(ImVec2(boxMin.x - 6, boxMax.y - bH * pct),

                                            ImVec2(boxMin.x - 2, boxMax.y),

                                            IM_COL32(hr, hg, 0, 240));

                }



                float ty = sFoot.y + 2;

                if (entShowName && cr.label) {

                    ImVec2 ts = ImGui::CalcTextSize(cr.label);

                    drawList->AddText(ImVec2(sHead.x - ts.x * 0.5f, sHead.y - 16.0f),

                                      entColor, cr.label);



                    if (cr.kind == EntCat::Deployable && cr.subtype == 1

                        && vcfg.Deployable.TC_ShowID) {

                        char idBuf[32];

                        uint32_t netId = 0;

                        SafeReadU32(baseObj + 0x130, &netId);

                        snprintf(idBuf, sizeof(idBuf), "ID:%u", netId);

                        ImVec2 its = ImGui::CalcTextSize(idBuf);

                        drawList->AddText(ImVec2(sHead.x - its.x * 0.5f,

                                                 sHead.y - 32.0f),

                                          IM_COL32(180, 220, 255, 230), idBuf);

                    }

                }



                if (entShowDist) {

                    char distBuf[32];

                    snprintf(distBuf, sizeof(distBuf), "[%.0fm]", eDist);

                    ImVec2 ds = ImGui::CalcTextSize(distBuf);

                    drawList->AddText(ImVec2(sHead.x - ds.x * 0.5f, ty),

                                      IM_COL32(210, 210, 215, 220), distBuf);

                }



                continue;

            }



            if (cachedBasePlayerClass == 0 && nameLooksPlayer) {

                Log("[RH] Found Player-like class: %s at %p", classNameBuf, (void*)classPtr);

                cachedBasePlayerClass = classPtr;

            }



            // Health

            float health = 0.0f;

            // Try 0x27C, if it's always 100 or 0, try 0x21C (common alternative)

            if (!SafeReadFloat(baseObj + offsets::BaseCombatEntity::health, &health) || health <= 0.0f) {

                SafeReadFloat(baseObj + 0x21C, &health);

            }

            

            float maxHealth = 100.0f;

            SafeReadFloat(baseObj + offsets::BaseCombatEntity::maxHealth, &maxHealth);

            if (maxHealth < 1.0f) maxHealth = 100.0f;



            // Position via playerModel

            uintptr_t playerModel = 0;

            if (!SafeReadPtr(baseObj + offsets::BasePlayer::playerModel, &playerModel) || !playerModel) continue;



            Vector3 pos = {};

            // Smart Position Detection: read via Unity icall on the BasePlayer

            // Component first (most reliable, build-stable). The legacy

            // PlayerModel + raw-offset path is kept as a fallback for the rare

            // case where the entity's transform isn't yet attached.

            //

            // PlayerModel candidate offsets per the May 2026 dumper output:

            //   0x210 / 0x21C / 0x228 / 0x264 / 0x270  (all internal Vector3)

            //   0x234 is `velocity` — must NOT be used as position

            // Old list (0x1f8, 0x204, 0x24c, 0x258) overlapped Quaternion fields

            // and was locking `confirmedPosOffset` on garbage data.

            auto looksLikePos = [](const Vector3& v) -> bool {

                if (std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z)) return false;

                if (std::isinf(v.x) || std::isinf(v.y) || std::isinf(v.z)) return false;

                if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) return false;

                // Rust map size is 6000m; clamp generously to ±20000 X/Z, ±2000 Y.

                if (v.x < -20000.0f || v.x > 20000.0f) return false;

                if (v.z < -20000.0f || v.z > 20000.0f) return false;

                if (v.y < -2000.0f  || v.y > 2000.0f)  return false;

                return true;

            };



            float ePosICall[3] = {0,0,0};

            if (Il2Cpp::ReadEntityPosition(baseObj, ePosICall)) {

                Vector3 tmp{ ePosICall[0], ePosICall[1], ePosICall[2] };

                if (looksLikePos(tmp)) pos = tmp;

            }



            if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) {

                static uint32_t confirmedPosOffset = 0;

                if (confirmedPosOffset != 0) {

                    Vector3 tmp{};

                    if (SafeReadVec3(playerModel + confirmedPosOffset, &tmp) && looksLikePos(tmp)) {

                        pos = tmp;

                    } else {

                        confirmedPosOffset = 0;

                    }

                }

                if (confirmedPosOffset == 0) {

                    uint32_t candidates[] = { 0x210, 0x21C, 0x228, 0x264, 0x270 };

                    for (uint32_t cand : candidates) {

                        Vector3 tmp{};

                        if (SafeReadVec3(playerModel + cand, &tmp) && looksLikePos(tmp)) {

                            pos = tmp;

                            confirmedPosOffset = cand;

                            break;

                        }

                    }

                }

            }



            if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;



            bool isNPC = (strstr(Il2Cpp::GetClassName((void*)baseObj), "NPC") != nullptr);



            float dist = localPos.Distance(pos);

            // Horizontal distance: camera is at eye height (~1.6-1.8m above feet),

            // so a pure 3D distance check misses self-identification. Use XZ-only

            // distance instead — two players can never share the same XZ footprint.

            float dxLocal = localPos.x - pos.x;

            float dzLocal = localPos.z - pos.z;

            float horizDist = sqrtf(dxLocal * dxLocal + dzLocal * dzLocal);

            if (horizDist < 0.5f) { Aim::localPlayer = baseObj; continue; }

            if (dist > 1500.0f) continue;



            uint64_t entityTeam = 0;

            SafeReadU64(baseObj + offsets::BasePlayer::team, &entityTeam);

            int32_t entityLifeState = 0;

            SafeReadI32(baseObj + offsets::BaseCombatEntity::lifeState, &entityLifeState);

            uint32_t entityPlayerFlags = 0;

            SafeReadU32(baseObj + offsets::BasePlayer::playerFlags, &entityPlayerFlags);



            const bool isWounded     = (entityPlayerFlags & RustFlags::Wounded)   != 0u;

            const bool isOnline      = (entityPlayerFlags & RustFlags::Connected) != 0u;

            const bool inSafezone    = (entityPlayerFlags & RustFlags::SafeZone)  != 0u;

            const bool isPfSleeping  = (entityPlayerFlags & RustFlags::Sleeping)  != 0u;

            const bool isDead        = (entityLifeState == 1);



            // ----------------------------------------------------------

            // POSE DETECTION (crouching / sleeping) — ModelState.flags

            // ----------------------------------------------------------

            // STATICALLY VERIFIED layout via IDA Pro (May 2026 build):

            //   BasePlayer + offsets::BasePlayer::modelState (0x320) -> ModelState* (pooled object)

            //   ModelState + offsets::ModelState::flags     (0x40)  -> uint flags

            //   bits (from Il2CppDumper dump.cs, ModelState.Flag enum):

            //     Ducked=1 Jumped=2 OnGround=4 Sleeping=8 Sprinting=0x10

            //     OnLadder=0x20 Flying=0x40 Aiming=0x80 Prone=0x100

            //     Mounted=0x200 Relaxed=0x400 OnPhone=0x800 Crawling=0x1000

            //     Loading=0x2000 HeadLook=0x4000 HasParachute=0x8000

            //     Blocking=0x10000 Ragdolling=0x20000 Catching=0x40000.

            //

            // Single static offset path — no probing, no lock state.

            // Read may still SafeRead-fail on edge entities (just

            // spawned, freed mid-frame), in which case flags stays 0

            // and the player is treated as standing/awake (safe

            // default that won't skip legit targets).

            uint32_t modelStateFlags = 0;

            {

                uintptr_t msPtr = 0;

                if (SafeReadPtr(baseObj + offsets::BasePlayer::modelState, &msPtr)

                    && msPtr >= 0x10000ULL

                    && msPtr <  0x0000800000000000ULL)

                {

                    uint32_t f = 0;

                    if (SafeReadU32(msPtr + offsets::ModelState::flags, &f)) {

                        modelStateFlags = f;

                    }

                }

            }



            // Rate-limited diagnostic so flag values can be eyeballed

            // against actual game state (crouch -> Ducked, sleep ->

            // Sleeping, etc.). Throttled to ~2s to avoid log spam.

            {

                static uint64_t s_lastPoseLogTick = 0;

                uint64_t now = GetTickCount64();

                if (now - s_lastPoseLogTick > 2000ULL) {

                    s_lastPoseLogTick = now;

                    Log("[RH][esp] pose ent=%p flags=0x%X "

                        "(D=%d J=%d G=%d Sl=%d Sp=%d La=%d Fl=%d Ai=%d Mt=%d)",

                        (void*)baseObj, modelStateFlags,

                        (modelStateFlags & offsets::ModelState::Ducked)    ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Jumped)    ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::OnGround)  ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Sleeping)  ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Sprinting) ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::OnLadder)  ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Flying)    ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Aiming)    ? 1 : 0,

                        (modelStateFlags & offsets::ModelState::Mounted)   ? 1 : 0);

                }

            }



            const bool isSleeping  = ((modelStateFlags & offsets::ModelState::Sleeping) != 0u) || isPfSleeping;

            const bool isCrouching = (modelStateFlags & offsets::ModelState::Ducked) != 0u;



            // ----------------------------------------------------------

            // POSE-AWARE HITBOX Y-OFFSETS

            // ----------------------------------------------------------

            // Rust player position (PlayerModel.position) sits at FEET

            // for standing/crouching and at CHEST for prone/sleeping.

            // The bone-center offsets thus depend on pose:

            //

            //   Standing  (default): head 1.70m, neck 1.55m, chest 1.30m, pelvis 0.90m

            //   Crouching (Ducked) : head 1.10m, neck 1.00m, chest 0.85m, pelvis 0.55m

            //   Sleeping  (prone)  : the model is laid out HORIZONTALLY,

            //                        position ≈ chest, so all bones cluster

            //                        within ~0.30m vertically. We aim at

            //                        pos.y + 0.20 (chest plane) which sits

            //                        inside any of the body capsules.

            float hitboxOffset;

            if (isSleeping) {

                hitboxOffset = 0.20f;

            } else if (isCrouching) {

                int hitbox = menu.AimCfg.Hitbox;

                hitboxOffset = (hitbox == 1) ? 1.00f

                             : (hitbox == 2) ? 0.85f

                             : (hitbox == 3) ? 0.55f

                                             : 1.10f;  // head (default)

            } else {

                int hitbox = menu.AimCfg.Hitbox;

                hitboxOffset = (hitbox == 1) ? 1.55f

                             : (hitbox == 2) ? 1.30f

                             : (hitbox == 3) ? 0.90f

                                             : 1.70f;  // head (default)

            }



            // Aim Target Selection

            if (menu.AimCfg.FOV > 0) {

                if (isNPC && !menu.AimCfg.TargetNPC) goto skip_aim;

                if (isSleeping && !menu.AimCfg.TargetSleeping) goto skip_aim;



                Vector2 screenPos;

                if (WorldToScreen(pos, screenPos, viewMatrix, width, height)) {

                    float dx = screenPos.x - (width / 2.0f);

                    float dy = screenPos.y - (height / 2.0f);

                    float fov = sqrtf(dx*dx + dy*dy);

                    if (fov < localBestFOV) {

                        float candY = pos.y + hitboxOffset;

                        if (menu.AimCfg.VisibleCheck) {

                            if (!Aim::IsLineVisible(Aim::localEyeX, Aim::localEyeY, Aim::localEyeZ,

                                                     pos.x, candY, pos.z))

                                goto skip_aim;

                        }

                        localBestFOV = fov;

                        localTarget  = baseObj;

                        localTargetX = pos.x;

                        localTargetZ = pos.z;

                        localTargetY = candY;

                    }

                }

            }

skip_aim:



            // ----------------------------------------------------------

            // POSE-AWARE BOX DIMENSIONS

            // ----------------------------------------------------------

            // The on-screen box must wrap the actual rendered model, not

            // a fixed 1.8m-tall capsule. Otherwise the box for a

            // crouching player looks 50 cm too tall (visible "ghost

            // standing" outline) and a sleeping player gets a very tall

            // narrow box covering empty air above their body.

            float boxTopY    = pos.y + 1.85f;  // standing default

            float boxBottomY = pos.y;          // feet

            if (isSleeping) {

                // Prone body: ~0.50 m above ground, slightly below pos

                // (since pos sits at chest level when prone).

                boxTopY    = pos.y + 0.45f;

                boxBottomY = pos.y - 0.20f;

            } else if (isCrouching) {

                // Squashed capsule: ~1.30 m total height.

                boxTopY    = pos.y + 1.30f;

                boxBottomY = pos.y;

            }



            // W2S

            Vector2 screenFeet = {0,0}, screenHead = {0,0};

            Vector3 footPos = { pos.x, boxBottomY, pos.z };

            Vector3 headPos = { pos.x, boxTopY,    pos.z };

            const bool feetOK = WorldToScreen(footPos, screenFeet, viewMatrix, width, height);

            const bool headOK = WorldToScreen(headPos, screenHead, viewMatrix, width, height);

            const bool w2sOK  = feetOK && headOK;



            if (w2sOK) {

                float dx = screenHead.x - (width / 2.0f);

                float dy = screenHead.y - (height / 2.0f);

                float distToCenter = sqrtf(dx*dx + dy*dy);

                if (distToCenter < minCrosshairDist) {

                    minCrosshairDist = distToCenter;

                    targetEntity = baseObj;

                }

            }



            float boxH = w2sOK ? (screenFeet.y - screenHead.y) : 0.0f;

            const bool boxOK = w2sOK

                && boxH >= 4.0f

                && !std::isnan(boxH) && !std::isinf(boxH);

            float boxW = boxOK ? (boxH * 0.5f) : 0.0f;



            ImVec2 bMin = boxOK

                ? ImVec2(screenHead.x - boxW / 2, screenHead.y)

                : ImVec2(0, 0);

            ImVec2 bMax = boxOK

                ? ImVec2(screenHead.x + boxW / 2, screenFeet.y)

                : ImVec2(0, 0);



            const float fOffSlack = 4.0f;

            const bool feetOnScreen = w2sOK

                && screenFeet.x >= -fOffSlack && screenFeet.x <= (float)width  + fOffSlack

                && screenFeet.y >= -fOffSlack && screenFeet.y <= (float)height + fOffSlack;

            const bool headOnScreen = w2sOK

                && screenHead.x >= -fOffSlack && screenHead.x <= (float)width  + fOffSlack

                && screenHead.y >= -fOffSlack && screenHead.y <= (float)height + fOffSlack;

            const bool fullyOffScreen = !w2sOK || (!feetOnScreen && !headOnScreen);



            int   curBoxType    = 0;

            bool  curName       = false;

            bool  curDistance   = false;

            bool  curHealth     = false;

            bool  curWeapon     = false;

            bool  curTargetLine = false;

            bool  curTargetBelt = false;

            bool  curTeamID     = false;

            bool  curViewDir    = false;

            bool  curOOF        = false;

            bool  curOutsideMrk = false;

            bool  curSkeleton   = false;

            float curSkelThick  = 1.4f;

            float curTextSpacing = 14.0f;

            float curDrawDist    = 1500.0f;

            float curOOFRadius   = 220.0f;

            ImU32 curColor       = IM_COL32(255, 50, 50, 230);

            ImU32 curOOFColor    = IM_COL32(255, 80, 220, 230);



            uint64_t localTeam = 0;

            if (Aim::localPlayer != 0) {

                SafeReadU64(Aim::localPlayer + offsets::BasePlayer::team, &localTeam);

            }

            const bool isTeammate = (localTeam != 0 && entityTeam == localTeam);



            {

                auto& cc = vcfg.Chams;

                if (isNPC) {

                    if (cc.NPC.Enabled)

                        Chams::Apply(baseObj, Chams::CatNPC,

                                     cc.NPC.VisColor, cc.NPC.OccColor, cc.NPC.Style);

                } else if (isTeammate) {

                    if (cc.Friendly.Enabled)

                        Chams::Apply(baseObj, Chams::CatFriendly,

                                     cc.Friendly.VisColor, cc.Friendly.OccColor, cc.Friendly.Style);

                } else {

                    if (cc.Player.Enabled)

                        Chams::Apply(baseObj, Chams::CatPlayer,

                                     cc.Player.VisColor, cc.Player.OccColor, cc.Player.Style);

                }

                if (!isNPC && cc.DisablePlayerCulling) {

                    Chams::DisableCulling(baseObj, true);

                }

            }



            if (isNPC) {

                auto& npc = vcfg.NPC;

                if (!npc.Enabled) continue;

                if (dist > npc.DrawDistance) continue;

                curBoxType    = npc.BoxType;

                curName       = npc.Name;

                curDistance   = npc.Distance;

                curHealth     = npc.Health;

                curWeapon     = npc.Weapon;

                curTargetLine = npc.TargetLine;

                curSkeleton   = npc.Skeleton;

                curSkelThick  = npc.SkeletonThickness;

                curTextSpacing = npc.TextSpacing;

                curDrawDist    = npc.DrawDistance;

                curColor       = ColorVec4ToU32(npc.Color);

            } else {

                auto& pl = vcfg.Player;

                if (!pl.Enabled) continue;

                if (dist > pl.DrawDistance) continue;



                if (isTeammate && !pl.DrawTeam)        continue;

                if (!isTeammate && !pl.DrawEnemies)    continue;

                if (isWounded && !pl.DrawWounded)      continue;

                if (isDead    && !pl.DrawDead)         continue;

                if (isSleeping && !pl.DrawSleeping)    continue;

                if (isSleeping && pl.OnlineSleeperOnly && !isOnline) continue;

                if (inSafezone && !pl.DrawSafezone)    continue;



                bool aimVisible = (baseObj == Aim::currentTarget) && Aim::currentTargetVisible;

                if (pl.UseVisibleColor && aimVisible) {

                    curColor = ColorVec4ToU32(pl.ColorVisible);

                } else if (isDead) {

                    curColor = ColorVec4ToU32(pl.ColorDead);

                } else if (isWounded) {

                    curColor = ColorVec4ToU32(pl.ColorWounded);

                } else if (isSleeping) {

                    curColor = ColorVec4ToU32(pl.ColorSleeping);

                } else if (isTeammate) {

                    curColor = ColorVec4ToU32(pl.ColorTeam);

                } else {

                    curColor = ColorVec4ToU32(pl.ColorEnemy);

                }



                curBoxType    = pl.BoxType;

                curName       = pl.Name;

                curDistance   = pl.Distance;

                curHealth     = pl.Health;

                curWeapon     = pl.Weapon;

                curTargetLine = pl.TargetLine;

                curTargetBelt = pl.TargetBelt;

                curTeamID     = pl.TeamID;

                curViewDir    = pl.ViewDirection;

                curOOF        = pl.OOFIndicator;

                curOutsideMrk = pl.OutsideMark;

                curSkeleton   = pl.Skeleton;

                curSkelThick  = pl.SkeletonThickness;

                curTextSpacing = pl.TextSpacing;

                curDrawDist    = pl.DrawDistance;

                curOOFRadius   = pl.OOFRadius;

                curOOFColor    = ColorVec4ToU32(pl.ColorOOF);

            }



            if (fullyOffScreen) {

                if (curOOF) {

                    Vector3 anchor = { pos.x, pos.y + 1.0f, pos.z };

                    float w = viewMatrix.m[0][3] * anchor.x + viewMatrix.m[1][3] * anchor.y

                            + viewMatrix.m[2][3] * anchor.z + viewMatrix.m[3][3];

                    float xx = viewMatrix.m[0][0] * anchor.x + viewMatrix.m[1][0] * anchor.y

                            + viewMatrix.m[2][0] * anchor.z + viewMatrix.m[3][0];

                    float yy = viewMatrix.m[0][1] * anchor.x + viewMatrix.m[1][1] * anchor.y

                            + viewMatrix.m[2][1] * anchor.z + viewMatrix.m[3][1];

                    bool behind = (w < 0.0f);

                    if (behind) { xx = -xx; yy = -yy; w = -w; }

                    if (w > 0.001f) {

                        float invw = 1.0f / w;

                        float sx = (width / 2.0f) * (1.0f + xx * invw);

                        float sy = (height / 2.0f) * (1.0f - yy * invw);

                        float odx = sx - width  / 2.0f;

                        float ody = sy - height / 2.0f;

                        if (behind) { odx = -odx; ody = -ody; }

                        float len = sqrtf(odx * odx + ody * ody);

                        if (len > 1.0f) {

                            float ax = width  / 2.0f + (odx / len) * curOOFRadius;

                            float ay = height / 2.0f + (ody / len) * curOOFRadius;

                            float angle = atan2f(ody, odx);

                            float cs = cosf(angle), sn = sinf(angle);

                            const float s = 11.0f;

                            ImVec2 tip(ax + cs * s,            ay + sn * s);

                            ImVec2 lf (ax + cs * -s * 0.55f - sn * s * 0.65f,

                                       ay + sn * -s * 0.55f + cs * s * 0.65f);

                            ImVec2 rt (ax + cs * -s * 0.55f + sn * s * 0.65f,

                                       ay + sn * -s * 0.55f - cs * s * 0.65f);

                            drawList->AddTriangleFilled(tip, lf, rt, curOOFColor);

                            drawList->AddTriangle(tip, lf, rt, IM_COL32(0, 0, 0, 220), 1.2f);

                        }

                    }

                }

                continue;

            }



            if (!boxOK) continue;



            int rA = (curColor >> IM_COL32_A_SHIFT) & 0xFF;

            int rR = (curColor >> IM_COL32_R_SHIFT) & 0xFF;

            int rG = (curColor >> IM_COL32_G_SHIFT) & 0xFF;

            int rB = (curColor >> IM_COL32_B_SHIFT) & 0xFF;



            if (curTargetLine) {

                drawList->AddLine(ImVec2(width / 2.0f, (float)height),

                                  ImVec2(screenFeet.x, screenFeet.y),

                                  IM_COL32(rR, rG, rB, (rA * 150) / 255), 1.0f);

            }



            if (curBoxType == 1) {

                drawList->AddRect(bMin, bMax, curColor, 0.0f, 0, 1.5f);

            } else if (curBoxType == 2) {

                DrawCornerBox(drawList, bMin, bMax, curColor, 1.5f);

            }



            if (curSkeleton && !isSleeping && Bones::g_initialized && playerModel) {

                void* animator = Bones::SafeGetAnimator((void*)playerModel);

                if (animator) {

                    float bp[Bones::BoneMax][3] = {};

                    bool  bOk[Bones::BoneMax] = {};

                    for (int bi = 0; bi < Bones::BoneMax; bi++)

                        bOk[bi] = Bones::SafeGetBonePos(animator, bi, bp[bi]);



                    Vector2 sp[Bones::BoneMax] = {};

                    bool    sOk[Bones::BoneMax] = {};

                    for (int bi = 0; bi < Bones::BoneMax; bi++) {

                        if (!bOk[bi]) continue;

                        Vector3 w{ bp[bi][0], bp[bi][1], bp[bi][2] };

                        sOk[bi] = WorldToScreen(w, sp[bi], viewMatrix, width, height);

                    }



                    for (int pi = 0; pi < Bones::g_pairCount; pi++) {

                        int a = Bones::g_pairs[pi].a;

                        int b = Bones::g_pairs[pi].b;

                        if (!sOk[a] || !sOk[b]) continue;

                        drawList->AddLine(ImVec2(sp[a].x, sp[a].y),

                                          ImVec2(sp[b].x, sp[b].y),

                                          curColor, curSkelThick);

                    }



                    if (sOk[Bones::Head] && sOk[Bones::Neck]) {

                        float headRadius = fabsf(sp[Bones::Neck].y - sp[Bones::Head].y) * 0.45f;

                        if (headRadius < 2.0f) headRadius = 2.0f;

                        drawList->AddCircle(ImVec2(sp[Bones::Head].x, sp[Bones::Head].y - headRadius * 0.5f),

                                            headRadius, curColor, 12, curSkelThick);

                    }

                }

            }



            if (curHealth && maxHealth > 0.0f) {

                float pct = fminf(health / maxHealth, 1.0f);

                if (pct < 0.0f) pct = 0.0f;

                drawList->AddRectFilled(ImVec2(screenHead.x - boxW / 2 - 6, screenHead.y),

                                        ImVec2(screenHead.x - boxW / 2 - 2, screenHead.y + boxH),

                                        IM_COL32(0, 0, 0, 160));

                int hr = (int)((1.0f - pct) * 255);

                int hg = (int)(pct * 255);

                float hBar = boxH * pct;

                drawList->AddRectFilled(ImVec2(screenHead.x - boxW / 2 - 6, screenFeet.y - hBar),

                                        ImVec2(screenHead.x - boxW / 2 - 2, screenFeet.y),

                                        IM_COL32(hr, hg, 0, 240));

            }



            if (curViewDir) {

                Vector3 vel = {};

                bool gotVel = SafeReadVec3(playerModel + offsets::PlayerModel::velocity, &vel);

                float vmag = gotVel ? sqrtf(vel.x*vel.x + vel.z*vel.z) : 0.0f;

                if (gotVel && vmag > 0.4f) {

                    float ux = vel.x / vmag, uz = vel.z / vmag;

                    Vector3 a = { pos.x, pos.y + 1.4f, pos.z };

                    Vector3 b = { pos.x + ux * 1.6f, pos.y + 1.4f, pos.z + uz * 1.6f };

                    Vector2 sa, sb;

                    if (WorldToScreen(a, sa, viewMatrix, width, height)

                        && WorldToScreen(b, sb, viewMatrix, width, height)) {

                        drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), curColor, 1.6f);

                    }

                }

            }



            float textY = screenFeet.y + 2;

            const float textStep = curTextSpacing;



            if (curName) {

                uintptr_t nameObj = 0;

                if (SafeReadPtr(baseObj + offsets::BasePlayer::username, &nameObj) && nameObj) {

                    int len = 0;

                    if (SafeRead(nameObj + 0x10, &len, sizeof(int)) && len > 0 && len < 64) {

                        wchar_t wbuf[64] = {0};

                        if (SafeRead(nameObj + 0x14, wbuf, len * sizeof(wchar_t))) {

                            char buf[128];

                            int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, buf, 127, NULL, NULL);

                            if (n > 0) {

                                buf[n] = 0;

                                ImVec2 textSize = ImGui::CalcTextSize(buf);

                                drawList->AddText(ImVec2(screenHead.x - textSize.x / 2,

                                                         screenHead.y - textStep - 2.0f),

                                                  IM_COL32(255, 255, 255, 240), buf);

                            }

                        }

                    }

                }

            }



            if (curTeamID && entityTeam != 0) {

                char tbuf[40];

                snprintf(tbuf, sizeof(tbuf), "T:%llu", (unsigned long long)entityTeam);

                ImVec2 ts = ImGui::CalcTextSize(tbuf);

                drawList->AddText(ImVec2(screenHead.x - ts.x / 2,

                                         screenHead.y - textStep * 2.0f - 2.0f),

                                  IM_COL32(180, 220, 255, 230), tbuf);

            }



            if (curWeapon) {

                uintptr_t activeItem = 0;

                if (SafeReadPtr(baseObj + offsets::BasePlayer::clActiveItem, &activeItem) && activeItem) {

                    uintptr_t itemDef = 0;

                    if (SafeReadPtr(activeItem + offsets::Item::itemDefinition, &itemDef) && itemDef) {

                        uintptr_t nameObj = 0;

                        if (SafeReadPtr(itemDef + offsets::ItemDefinition::itemDisplayName, &nameObj) && nameObj) {

                            SafeReadPtr(itemDef + offsets::ItemDefinition::shortName, &nameObj);

                            int len = 0;

                            if (SafeRead(nameObj + 0x10, &len, sizeof(int)) && len > 0 && len < 64) {

                                wchar_t wbuf[64] = {0};

                                if (SafeRead(nameObj + 0x14, wbuf, len * sizeof(wchar_t))) {

                                    char buf[128];

                                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, buf, 127, NULL, NULL);

                                    if (n > 0) {

                                        buf[n] = 0;

                                        ImVec2 ts2 = ImGui::CalcTextSize(buf);

                                        drawList->AddText(ImVec2(screenHead.x - ts2.x / 2, textY),

                                                          IM_COL32(255, 230, 110, 255), buf);

                                        textY += textStep;

                                    }

                                }

                            }

                        }

                    }

                }

            }



            if (curTargetBelt && baseObj == targetEntity && dist < 150.0f) {

                ImGui::SetNextWindowPos(ImVec2(width - 220.0f, height / 2.0f - 100.0f), ImGuiCond_Always);

                ImGui::SetNextWindowSize(ImVec2(200, 0));

                if (ImGui::Begin("TargetHotbar", nullptr,

                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |

                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |

                    ImGuiWindowFlags_NoSavedSettings)) {

                    ImGui::TextColored(ImVec4(1, 1, 0, 1), "TARGET HOTBAR");

                    ImGui::Separator();



                    uintptr_t inv = 0;

                    if (SafeReadPtr(baseObj + offsets::BasePlayer::playerInventory, &inv) && inv) {

                        uintptr_t belt = 0;

                        if (SafeReadPtr(inv + offsets::PlayerInventory::container2, &belt) && belt) {

                            uintptr_t itemList = 0;

                            if (SafeReadPtr(belt + offsets::ItemContainer::list, &itemList) && itemList) {

                                uintptr_t itemsArray = 0;

                                if (SafeReadPtr(itemList + 0x10, &itemsArray) && itemsArray) {

                                    uint32_t count = 0;

                                    SafeReadU32(itemList + 0x18, &count);

                                    if (count > 6) count = 6;

                                    for (uint32_t j = 0; j < count; j++) {

                                        uintptr_t item = 0;

                                        SafeReadPtr(itemsArray + 0x20 + (j * 8), &item);

                                        if (item) {

                                            uintptr_t itemDef = 0;

                                            SafeReadPtr(item + offsets::Item::itemDefinition, &itemDef);

                                            if (itemDef) {

                                                uintptr_t nameObj = 0;

                                                SafeReadPtr(itemDef + offsets::ItemDefinition::shortName, &nameObj);

                                                int len = 0;

                                                if (SafeRead(nameObj + 0x10, &len, sizeof(int)) && len > 0 && len < 64) {

                                                    wchar_t wbuf[64] = {0};

                                                    SafeRead(nameObj + 0x14, wbuf, len * sizeof(wchar_t));

                                                    char buf[128];

                                                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, buf, 127, NULL, NULL);

                                                    if (n > 0) {

                                                        buf[n] = 0;

                                                        ImGui::TextColored(ImVec4(1, 1, 1, 1), "[%d] %s", j+1, buf);

                                                    }

                                                }

                                            }

                                        }

                                    }

                                }

                            }

                        }

                    }

                    ImGui::End();

                }

            }



            if (curDistance) {

                char distBuf[40];

                if (curOutsideMrk) {

                    snprintf(distBuf, sizeof(distBuf), "[%.0fm]%s", dist, isOnline ? " *" : "");

                } else {

                    snprintf(distBuf, sizeof(distBuf), "[%.0fm]", dist);

                }

                ImVec2 textSize = ImGui::CalcTextSize(distBuf);

                drawList->AddText(ImVec2(screenHead.x - textSize.x / 2, textY),

                                  IM_COL32(210, 210, 215, 220), distBuf);

                textY += textStep;

            }



            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        }



        // Atomic commit of aim target.

        // Sticky targeting: if we found a target this frame, commit it and

        // stamp the tick. If we did NOT but we saw one very recently (<500ms),

        // KEEP the previous target/position alive so the shot hook (running

        // on the game-logic thread) doesn't see a transient target=0 due to

        // a render-frame skip or momentary occlusion filter trip.

        const unsigned long long kStickyMs = 500;

        unsigned long long now = GetTickCount64();

        // Final safety net: never target the local player itself.

        if (localTarget != 0 && localTarget == Aim::localPlayer) {

            localTarget = 0;

        }

        if (localTarget != 0) {

            // ---- Target world-velocity tracking ----------------------------

            // Sample: instantaneous v = (pos - lastPos) / dt. Feed through an

            // EMA filter to damp noise (tick jitter, network interpolation),

            // then clamp to a human-plausible magnitude so a target teleport,

            // a re-acquire on a different player, or a hitbox switch can't

            // produce a huge lead vector that sends the bullet into orbit.

            //

            // Rust movement caps (approx): walk ~3 m/s, run ~5.5 m/s, sprint

            // ~7 m/s, horse/vehicle up to ~15 m/s. We clamp component-wise

            // at 20 m/s which tolerates vehicles but still rejects teleports.

            static uintptr_t s_lastVelTarget = 0;

            static float s_lastPX = 0, s_lastPY = 0, s_lastPZ = 0;

            static unsigned long long s_lastVelTick = 0;



            if (localTarget == s_lastVelTarget && s_lastVelTick != 0) {

                float dtMs = (float)(now - s_lastVelTick);

                if (dtMs >= 1.0f && dtMs <= 300.0f) {

                    float dt = dtMs * 0.001f;

                    float vx = (localTargetX - s_lastPX) / dt;

                    float vy = (localTargetY - s_lastPY) / dt;

                    float vz = (localTargetZ - s_lastPZ) / dt;

                    // Clamp per-component to reject teleport / re-acquire spikes.

                    auto clampv = [](float v) {

                        const float kMax = 20.0f;

                        if (v >  kMax) return  kMax;

                        if (v < -kMax) return -kMax;

                        return v;

                    };

                    vx = clampv(vx);

                    vy = clampv(vy);

                    vz = clampv(vz);

                    // Exponential moving average. Alpha ~ 0.35 gives a ~3-frame

                    // effective window at 60 FPS — responsive enough for a

                    // strafing target but well-damped against tick jitter.

                    const float alpha = 0.35f;

                    Aim::targetVelX = Aim::targetVelX * (1.0f - alpha) + vx * alpha;

                    Aim::targetVelY = Aim::targetVelY * (1.0f - alpha) + vy * alpha;

                    Aim::targetVelZ = Aim::targetVelZ * (1.0f - alpha) + vz * alpha;

                }

            } else {

                // New target (or first frame after acquire) — zero the filter.

                Aim::targetVelX = 0.0f;

                Aim::targetVelY = 0.0f;

                Aim::targetVelZ = 0.0f;

            }

            s_lastVelTarget = localTarget;

            s_lastPX = localTargetX;

            s_lastPY = localTargetY;

            s_lastPZ = localTargetZ;

            s_lastVelTick = now;



            Aim::targetPosX    = localTargetX;

            Aim::targetPosY    = localTargetY;

            Aim::targetPosZ    = localTargetZ;

            Aim::bestTargetFOV = localBestFOV;

            Aim::currentTarget = localTarget;   // commit new target

            Aim::lastTargetTick = now;

        } else if ((now - Aim::lastTargetTick) >= kStickyMs) {

            // Sticky window expired — actually clear.

            Aim::currentTarget = 0;

            Aim::targetVelX = Aim::targetVelY = Aim::targetVelZ = 0.0f;

        }

        // else: keep previous currentTarget/position inside sticky window.



        if (!menu.AimCfg.VisibleCheck) {

            Aim::currentTargetVisible = true;

        } else if (Aim::currentTarget == 0) {

            Aim::currentTargetVisible = false;

        } else if (localTarget != 0) {

            Aim::currentTargetVisible = true;

        }



        if (menu.WorldCfg.BulletTracer) {

            unsigned long long now = GetTickCount64();

            float lifeMs = menu.WorldCfg.TracerLife * 1000.0f;

            const float gravity = 9.81f * 0.15f;

            const float simDt = 0.025f;

            const int   kSteps = 40;

            const float kSkipDist = 3.0f;



            for (int i = 0; i < Aim::kMaxTracers; i++) {

                auto& t = Aim::g_tracers[i];

                if (t.spawnTick == 0) continue;

                float age = (float)(now - t.spawnTick);

                if (age > lifeMs) { t.spawnTick = 0; continue; }



                float alpha = 1.0f - (age / lifeMs);

                if (alpha < 0.01f) continue;



                float speed = sqrtf(t.velX * t.velX + t.velY * t.velY + t.velZ * t.velZ);

                if (speed < 1.0f) continue;



                ImU32 col;

                if (menu.WorldCfg.TracerRainbow) {

                    float hue = fmodf((float)t.spawnTick * 0.001f + age * 0.002f, 1.0f);

                    float r2, g2, b2;

                    ImGui::ColorConvertHSVtoRGB(hue, 0.9f, 1.0f, r2, g2, b2);

                    col = IM_COL32((int)(r2 * 255), (int)(g2 * 255), (int)(b2 * 255), (int)(alpha * 255));

                } else {

                    auto& c = menu.WorldCfg.TracerColor;

                    col = IM_COL32((int)(c[0]*255), (int)(c[1]*255), (int)(c[2]*255), (int)(c[3]*alpha*255));

                }



                float px = t.startX, py = t.startY, pz = t.startZ;

                float vx = t.velX,   vy = t.velY,   vz = t.velZ;



                float skipT = kSkipDist / speed;

                px += vx * skipT;

                py += vy * skipT - 0.5f * gravity * skipT * skipT;

                pz += vz * skipT;

                vy -= gravity * skipT;



                Vector2 prev{};

                bool prevOk = false;



                for (int s = 0; s <= kSteps; s++) {

                    Vector3 wp{ px, py, pz };

                    Vector2 sp{};

                    bool ok = WorldToScreen(wp, sp, viewMatrix, width, height);

                    if (ok && prevOk) {

                        drawList->AddLine(ImVec2(prev.x, prev.y), ImVec2(sp.x, sp.y),

                                          col, menu.WorldCfg.TracerThickness);

                    }

                    if (ok) { prev = sp; prevOk = true; }



                    px += vx * simDt;

                    py += vy * simDt;

                    pz += vz * simDt;

                    vy -= gravity * simDt;

                }

            }

        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        Log("[RH] Exception caught in ESP::Render!");

    }

}

} // namespace ESP



