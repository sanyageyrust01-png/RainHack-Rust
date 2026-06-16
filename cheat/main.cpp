#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <MinHook.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

HMODULE g_hModule = NULL;

#include "features/esp/esp.h"
#include "features/Menu/Menu.h"
#include "features/aim/aim.h"
#include "features/il2cpp/il2cpp.h"

static bool   g_showMenu     = true;
static WNDPROC oWndProc      = NULL;
static HWND    g_gameHwnd    = NULL;   // saved during ImGui init for re-hook checks

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_showMenu = !g_showMenu;
        Menu::SetVisible(g_showMenu);
        OutputDebugStringA(g_showMenu ? "[RH][menu] VK_INSERT -> g_showMenu=1\n"
                                      : "[RH][menu] VK_INSERT -> g_showMenu=0\n");
        return 0;
    }

    if (g_showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// Some Unity-based games (Rust included) reinstall their own WndProc partway
// through startup — typically right after "[Renderer] Initializing D3D11
// graphics device" — which silently overwrites our SetWindowLongPtr hook.
// After that point INSERT no longer reaches hkWndProc and the menu can't be
// toggled. Defence: every Present, verify the active WndProc is still ours;
// if not, capture whatever Unity installed as the new oWndProc and re-attach
// hkWndProc on top. Cheap to call (one GetWindowLongPtr) and only writes when
// it actually drifted.
static void EnsureWndProcHooked() {
    if (!g_gameHwnd) return;
    WNDPROC current = (WNDPROC)GetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC);
    if (!current || current == hkWndProc) return; // still good

    // Unity (or someone) replaced it. Chain through the new one.
    oWndProc = current;
    SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    OutputDebugStringA("[RH][menu] WndProc was overwritten externally — re-hooked\n");
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ---- Hook typedefs ----
typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn oPresent = NULL;
static ResizeBuffersFn oResizeBuffers = NULL;
static long g_frameCount = 0;

static bool g_imGuiInitialized = false;
static ID3D11Device* g_pd3dDevice = NULL;
static ID3D11DeviceContext* g_pd3dDeviceContext = NULL;

static CRITICAL_SECTION g_renderCS;

// ---- Present Hook ----
HRESULT WINAPI hkPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags) {
    EnterCriticalSection(&g_renderCS);
    __try {
        // Detect device change (game recreated D3D11)
        ID3D11Device* pDev = NULL;
        if (SUCCEEDED(pSC->GetDevice(__uuidof(ID3D11Device), (void**)&pDev))) {
            if (pDev != g_pd3dDevice) {
                // Device changed — full teardown
                if (g_imGuiInitialized) {
                    Log("[RH] Device changed — tearing down ImGui");
                    ImGui_ImplDX11_Shutdown();
                    ImGui::DestroyContext();
                    g_imGuiInitialized = false;
                }
                if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
                if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }

                g_pd3dDevice = pDev;
                g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            } else {
                pDev->Release();
            }
        } else {
            // Can't get device — skip this frame entirely
            goto call_original;
        }

        // Init ImGui if needed
        if (!g_imGuiInitialized && g_pd3dDevice) {
            Log("[RH] Initializing ImGui...");
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = NULL;
            
            // Get game window
            DXGI_SWAP_CHAIN_DESC sd;
            pSC->GetDesc(&sd);
            HWND hWnd = sd.OutputWindow;
            
            ImGui_ImplWin32_Init(hWnd);

            Menu::Initialize();

            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

            g_gameHwnd = hWnd; // remember for the per-frame re-hook check
            oWndProc = (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
            
            g_imGuiInitialized = true;
            Log("[RH] ImGui ready");
        }

        // Defend against Unity overwriting our WndProc after Renderer init —
        // re-attach if it drifted. Runs every frame; near-zero cost when intact.
        EnsureWndProcHooked();

        // Render
        if (g_imGuiInitialized) {
            ID3D11Texture2D* pBackBuffer = NULL;
            HRESULT hr = pSC->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            if (SUCCEEDED(hr) && pBackBuffer) {
                ID3D11RenderTargetView* pRTV = NULL;
                hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &pRTV);
                pBackBuffer->Release();

                if (SUCCEEDED(hr) && pRTV) {
                    DXGI_SWAP_CHAIN_DESC scd;
                    pSC->GetDesc(&scd);
                    ImGuiIO& io = ImGui::GetIO();
                    io.DisplaySize = ImVec2((float)scd.BufferDesc.Width, (float)scd.BufferDesc.Height);

                    static LARGE_INTEGER freq = {}, lastTime = {};
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    if (lastTime.QuadPart == 0) { QueryPerformanceFrequency(&freq); lastTime = now; }
                    io.DeltaTime = (float)(now.QuadPart - lastTime.QuadPart) / (float)freq.QuadPart;
                    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
                    lastTime = now;

                    ImGui_ImplDX11_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();
                    
                    Menu::SetVisible(g_showMenu);
                    Menu::Render();

                    Menu::RenderFreeWatermark();

                    ESP::Render();
                    ImGui::Render();

                    // Save the old RenderTarget and DepthStencil
                    ID3D11RenderTargetView* pOldRTV = NULL;
                    ID3D11DepthStencilView* pOldDSV = NULL;
                    g_pd3dDeviceContext->OMGetRenderTargets(1, &pOldRTV, &pOldDSV);

                    g_pd3dDeviceContext->OMSetRenderTargets(1, &pRTV, NULL);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                    
                    // Restore the old RenderTarget and DepthStencil
                    g_pd3dDeviceContext->OMSetRenderTargets(1, &pOldRTV, pOldDSV);

                    if (pOldRTV) pOldRTV->Release();
                    if (pOldDSV) pOldDSV->Release();
                    pRTV->Release();
                }
            }
        }

call_original:
        g_frameCount++;
        if (g_frameCount == 1) Log("[RH] Frame 1 OK");
        if (g_frameCount % 1000 == 0) Log("[RH] Frame %ld OK", g_frameCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[RH] Exception caught in hkPresent rendering loop!");
    }

    LeaveCriticalSection(&g_renderCS);
    return oPresent(pSC, SyncInterval, Flags);
}

// ---- ResizeBuffers Hook ----
HRESULT WINAPI hkResizeBuffers(IDXGISwapChain* pSC, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Log("[RH] ResizeBuffers %ux%u", w, h);
    EnterCriticalSection(&g_renderCS);

    // Full teardown before resize — don't try to invalidate objects on a potentially dead device
    if (g_imGuiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_imGuiInitialized = false;
    }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }

    LeaveCriticalSection(&g_renderCS);
    
    HRESULT hr = oResizeBuffers(pSC, bc, w, h, fmt, flags);
    
    Log("[RH] ResizeBuffers done hr=0x%X", hr);
    // ImGui will be re-initialized on next hkPresent call
    return hr;
}

// ---- Init ----
static void CALLBACK InitThread(PTP_CALLBACK_INSTANCE inst, PVOID param) {
    Log("[RH] InitThread started");
    Sleep(5000);
    Log("[RH] Sleep(5000) finished");

    WNDCLASSEXA wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0, 0,
                       GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "RH_D2D", NULL };
    if (!RegisterClassExA(&wc)) {
        Log("[RH] RegisterClassExA failed (error %lu)", GetLastError());
        // If it's already registered, we continue
    }

    HWND hDummy = CreateWindowA("RH_D2D", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!hDummy) {
        Log("[RH] CreateWindowA failed (error %lu)", GetLastError());
        return;
    }
    Log("[RH] Dummy window created: %p", hDummy);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hDummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* pDev = NULL;
    ID3D11DeviceContext* pCtx = NULL;
    IDXGISwapChain* pSC = NULL;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    typedef HRESULT(WINAPI* PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11) {
        hD3D11 = LoadLibraryA("d3d11.dll");
    }

    if (!hD3D11) {
        Log("[RH] d3d11.dll not found/loaded (error %lu)", GetLastError());
        DestroyWindow(hDummy);
        return;
    }
    Log("[RH] d3d11.dll handle: %p", hD3D11);

    auto pD3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
    if (!pD3D11CreateDeviceAndSwapChain) {
        Log("[RH] GetProcAddress(D3D11CreateDeviceAndSwapChain) failed (error %lu)", GetLastError());
        DestroyWindow(hDummy);
        return;
    }

    Log("[RH] Calling D3D11CreateDeviceAndSwapChain...");
    HRESULT hr = pD3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        fls, 2, D3D11_SDK_VERSION, &sd, &pSC, &pDev, &fl, &pCtx);

    if (FAILED(hr)) {
        Log("[RH] D3D11CreateDeviceAndSwapChain failed: 0x%X", hr);
        DestroyWindow(hDummy);
        return;
    }
    Log("[RH] D3D11 device and swapchain created successfully");

    void** pVMT = *(void***)pSC;
    void* pPresentAddr = pVMT[8];
    void* pResizeAddr = pVMT[13];
    Log("[RH] PresentAddr: %p, ResizeAddr: %p", pPresentAddr, pResizeAddr);

    pSC->Release();
    pCtx->Release();
    pDev->Release();
    DestroyWindow(hDummy);
    UnregisterClassA("RH_D2D", wc.hInstance);

    InitializeCriticalSection(&g_renderCS);

    Log("[RH] Initializing MinHook...");
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Log("[RH] MH_Initialize failed: %d", status);
        return;
    }

    MH_STATUS mhPresent = MH_CreateHook(pPresentAddr, hkPresent, (void**)&oPresent);
    MH_STATUS mhResize  = MH_CreateHook(pResizeAddr, hkResizeBuffers, (void**)&oResizeBuffers);
    Log("[RH] MH_CreateHook Present=%d Resize=%d", (int)mhPresent, (int)mhResize);

    // Enable D3D hooks immediately so the UI always comes up even if IL2CPP hooks fail.
    if (mhPresent == MH_OK) {
        MH_STATUS s = MH_EnableHook(pPresentAddr);
        Log("[RH] MH_EnableHook Present: %d", (int)s);
    }
    if (mhResize == MH_OK) {
        MH_STATUS s = MH_EnableHook(pResizeAddr);
        Log("[RH] MH_EnableHook Resize: %d", (int)s);
    }

    Log("[RH] Initializing il2cpp (ESP only build)...");
    if (il2cpp.Init()) {
        __try {
            Aim::InitIL2CPPOffsets();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[RH] InitIL2CPPOffsets CRASHED");
        }
    } else {
        Log("[RH] il2cpp.Init failed!");
    }

    Log("[RH] Hooks installed. Thread exiting.");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        OutputDebugStringA("[RH] DllMain: ATTACH\n");
        TrySubmitThreadpoolCallback(InitThread, hModule, nullptr);
    }
    return TRUE;
}
