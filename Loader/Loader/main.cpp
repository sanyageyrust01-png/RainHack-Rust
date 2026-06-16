#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#pragma comment(lib, "shell32.lib")

#include "src/Config.h"
#include "src/Util.h"
#include "src/VMProtectSDK.h"
#include "src/AntiDebug.h"
#include "src/AntiVM.h"
#include "src/AntiTamper.h"
#include "src/AntiPatch.h"
#include "src/Injector.h"
#include "src/Vdb.h"
#include "src/GUI.h"
#include "src/Resources.h"
#include "src/Ban.h"
#include "src/AntiDbg.h"

static std::atomic<bool> g_loginInFlight{ false };
static std::atomic<bool> g_loggedIn{ false };
static std::atomic<bool> g_tamperRun{ false };

__declspec(noinline) static bool earlyChecks() {
    VMP_BEGIN_MUTATION("main.earlyChecks");
    antidebug::hideThreadFromDebugger();
    antidbg::TrySoftHardening();

#if !RAINHACK_DEV_MODE
    if (!antidbg::VerifyHard()) {
        ban::triggerBan("antidbg.hard");
    }
#else
    (void)antidbg::VerifyHard();
#endif

    auto d = antidebug::check();
    if (d.detected) {
#if RAINHACK_DEV_MODE
        util::logf("[boot] DEV_MODE: ignored antidebug %s/%s", d.method.c_str(), d.detail.c_str());
#else
        ban::triggerBan(std::string("antidebug:") + d.method);
#endif
    }
    auto t = antitamper::fullScan();
    if (t.detected) {
#if RAINHACK_DEV_MODE
        util::logf("[boot] DEV_MODE: ignored antitamper %s/%s", t.method.c_str(), t.detail.c_str());
#else
        ban::triggerBan(std::string("antitamper:") + t.method);
#endif
    }
    auto v = antivm::check();
    if (v.detected) {
#if RAINHACK_DEV_MODE
        util::logf("[boot] DEV_MODE: ignored antivm %s/%s", v.method.c_str(), v.detail.c_str());
#else
        ban::triggerBan(std::string("antivm:") + v.method);
#endif
    }

    if (VMProtectIsDebuggerPresent(true)) {
#if !RAINHACK_DEV_MODE
        ban::triggerBan("vmp.debugger");
#endif
    }
    if (VMProtectIsVirtualMachinePresent()) {
#if !RAINHACK_DEV_MODE
        ban::triggerBan("vmp.vm");
#endif
    }
    if (!VMProtectIsValidImageCRC()) {
#if !RAINHACK_DEV_MODE
        ban::triggerBan("vmp.crc");
#endif
    }

    if (!antipatch::init()) {
#if RAINHACK_DEV_MODE
        util::logf("[boot] DEV_MODE: ignored antipatch.init: %s", antipatch::lastFault().c_str());
#else
        ban::triggerBan(std::string("antipatch.init:") + antipatch::lastFault());
#endif
    }

    return true;
}

static void backgroundIntegrity() {
    g_tamperRun.store(true);
    while (g_tamperRun.load()) {
        for (int i = 0; i < 16 && g_tamperRun.load(); ++i) util::sleepMs(250);
        if (!g_tamperRun.load()) break;

        auto d = antidebug::check();
        if (d.detected) ban::triggerBan(std::string("antidebug:") + d.method);

        auto t = antitamper::fullScan();
        if (t.detected) ban::triggerBan(std::string("antitamper:") + t.method);

        auto v = antivm::check();
        if (v.detected) ban::triggerBan(std::string("antivm:") + v.method);

        if (!antipatch::verify()) ban::triggerBan(std::string("antipatch:") + antipatch::lastFault());
    }
}

__declspec(noinline) static void launchPayloadAsync() {
    std::thread([]() {
        VMP_BEGIN_MUTATION("main.payload");

        std::vector<uint8_t> encryptedDll;
        if (!resources::loadCheatBlob(encryptedDll)) {
            util::logf("payload: cheat blob load failed");
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }
        util::logf("cheat blob loaded (%zu bytes)", encryptedDll.size());

        std::vector<uint8_t> sys;
        if (!resources::loadDriverSys(sys)) {
            util::logf("payload: driver blob load/decrypt failed");
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }
        util::logf("driver blob decrypted (%zu bytes)", sys.size());

        if (!injector::driverLoaded()) {
            auto vs = vdb::check();
            util::logf("vdb: %s", vs.detail.c_str());

            if (vs.blocklistEnabled || vs.hvciEnabled) {
                std::wstring msg =
                    L"Kernel driver mapping is blocked by Windows.\n\n"
                    L"Detected:\n";
                if (vs.blocklistEnabled) msg += L"  \u2022 Vulnerable Driver Blocklist  ON\n";
                if (vs.hvciEnabled)      msg += L"  \u2022 Memory Integrity (HVCI)      ON\n";
                msg +=
                    L"\nYes  \u2014 Disable automatically and REBOOT in 10 s.\n"
                    L"No   \u2014 Show manual instructions, then exit.\n"
                    L"Cancel \u2014 Abort.";

                int r = MessageBoxW(gui::window(), msg.c_str(),
                    L"RainHack: kernel protection enabled",
                    MB_YESNOCANCEL | MB_ICONWARNING | MB_TOPMOST);

                if (r == IDYES) {
                    std::string ve;
                    if (!vdb::disable(ve)) {
                        MessageBoxW(gui::window(),
                            (L"Auto-disable failed: " + util::s2ws(ve)).c_str(),
                            L"RainHack", MB_OK | MB_ICONERROR);
                        gui::setStatus("Login failed", 2);
                        gui::setBusy(false);
                        g_loginInFlight = false;
                        return;
                    }
                    std::string re;
                    if (!vdb::scheduleReboot(10, re)) {
                        MessageBoxW(gui::window(),
                            (L"Reboot scheduling failed: " + util::s2ws(re) +
                             L"\n\nReboot manually to apply.").c_str(),
                            L"RainHack", MB_OK | MB_ICONERROR);
                    }
                    gui::setStatus("Rebooting...", 3);
                    util::sleepMs(1500);
                    util::hardExit(0);
                } else if (r == IDNO) {
                    MessageBoxW(gui::window(),
                        L"To disable manually:\n\n"
                        L"1. Settings \u2192 Privacy & security \u2192 Windows Security\n"
                        L"   \u2192 Device security \u2192 Core isolation details\n"
                        L"   \u2192 Memory integrity: OFF\n\n"
                        L"2. Settings \u2192 Privacy & security \u2192 Windows Security\n"
                        L"   \u2192 App & browser control \u2192 Reputation-based protection\n"
                        L"   \u2192 Block potentially unwanted apps: OFF\n\n"
                        L"3. Reboot, then run RainHack Loader again.",
                        L"RainHack: manual steps", MB_OK | MB_ICONINFORMATION);
                    gui::setStatus("Login failed", 2);
                    gui::setBusy(false);
                    g_loginInFlight = false;
                    return;
                } else {
                    gui::setStatus("Login failed", 2);
                    gui::setBusy(false);
                    g_loginInFlight = false;
                    return;
                }
            }

            gui::setStatus("Mapping driver...", 3);
            std::string drvErr;
            if (!injector::runEmbeddedKdmapper(sys, drvErr)) {
                util::logf("driver mapping failed: %s", drvErr.c_str());
                bool defenderIssue =
                    drvErr.find("Defender") != std::string::npos ||
                    drvErr.find("vanished") != std::string::npos ||
                    drvErr.find("quarantine") != std::string::npos;
                if (defenderIssue) {
                    int r = MessageBoxW(gui::window(),
                        L"Windows Defender is blocking the kernel driver mapper.\n\n"
                        L"Disable both:\n"
                        L"  1. Tamper Protection\n"
                        L"  2. Real-time protection\n\n"
                        L"Open Virus & threat protection settings now?\n"
                        L"(After disabling, run RainHack Loader again.)",
                        L"RainHack: Defender blocked kdmapper",
                        MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
                    if (r == IDYES) {
                        ShellExecuteW(nullptr, L"open",
                            L"windowsdefender://Threat",
                            nullptr, nullptr, SW_SHOW);
                    }
                }
                gui::setStatus("Login failed", 2);
                gui::setBusy(false);
                g_loginInFlight = false;
                return;
            }
            util::logf("kdmapper finished");

            for (int i = 0; i < 30; ++i) {
                if (injector::driverLoaded()) break;
                util::sleepMs(100);
            }
            if (!injector::driverLoaded()) {
                util::logf("driver mapped but device not reachable");
                gui::setStatus("Login failed", 2);
                gui::setBusy(false);
                g_loginInFlight = false;
                return;
            }
            util::logf("driver online");
        } else {
            util::logf("driver already loaded, reusing");
        }

        SecureZeroMemory(sys.data(), sys.size());
        std::vector<uint8_t>().swap(sys);

        gui::setStatus("Waiting for Rust...", 3);
        DWORD pid = injector::waitForProcess(RAINHACK_TARGET_PROCESS, RAINHACK_TARGET_FALLBACK, RAINHACK_TARGET_WAIT_SECONDS);
        if (!pid) {
            util::logf("target process timeout");
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }
        util::logf("found Rust PID %lu, waiting %d s for menu", pid, RAINHACK_INJECT_DELAY_SECONDS);

        gui::showSkipButton(true);
        bool skipped = false;
        for (int remaining = RAINHACK_INJECT_DELAY_SECONDS; remaining > 0 && !skipped; --remaining) {
            char buf[64];
            _snprintf_s(buf, _TRUNCATE, "Loading Rust... %d:%02d", remaining / 60, remaining % 60);
            gui::setStatus(buf, 3);
            for (int i = 0; i < 10; ++i) {
                if (gui::consumeSkipPress()) {
                    skipped = true;
                    util::logf("user pressed skip with %d s remaining", remaining);
                    break;
                }
                util::sleepMs(100);
            }
        }
        gui::showSkipButton(false);

        if (!injector::driverLoaded()) {
            util::logf("driver gone before inject");
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }

        DWORD livePid = 0;
        gui::setStatus("Verifying target...", 3);
        if (!injector::ensureTargetAlive(RAINHACK_TARGET_PROCESS, pid, livePid, 60 * 1000)) {
            util::logf("target %ls is not alive after delay (was PID %lu)",
                       RAINHACK_TARGET_PROCESS, pid);
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }
        if (livePid != pid) {
            util::logf("target PID changed during delay: %lu -> %lu", pid, livePid);
            pid = livePid;
        }

        std::string modErr;
        if (!injector::validateProcessModules(pid, modErr)) {
            util::logf("process module validation failed: %s", modErr.c_str());
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }

        if (!injector::isProcessAlive(pid)) {
            util::logf("target PID %lu died between validate and inject", pid);
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }

        util::logf("injecting into PID %lu via kernel-stream", pid);
        gui::setStatus("Injecting...", 3);

        std::vector<uint8_t> sessionKey(resources::cheatSessionKey(),
                                        resources::cheatSessionKey() + resources::cheatSessionKeyLen());

        std::string ierr;
        bool injected = injector::kernelStreamMap(pid, encryptedDll, sessionKey, ierr);

        if (!encryptedDll.empty()) {
            SecureZeroMemory(encryptedDll.data(), encryptedDll.size());
            std::vector<uint8_t>().swap(encryptedDll);
        }
        if (!sessionKey.empty()) {
            SecureZeroMemory(sessionKey.data(), sessionKey.size());
            std::vector<uint8_t>().swap(sessionKey);
        }

        if (!injected) {
            util::logf("inject failed: %s", ierr.c_str());
            gui::setStatus("Login failed", 2);
            gui::setBusy(false);
            g_loginInFlight = false;
            return;
        }
        util::logf("cheat injected via kernel-stream APC");

        std::thread(backgroundIntegrity).detach();

        g_loggedIn = true;
        gui::setStatus("Injected", 1);
        gui::setBusy(false);
        util::sleepMs(800);
        gui::hide();
        g_loginInFlight = false;
    }).detach();
}

__declspec(noinline) static void onLoginClicked() {
    if (g_loginInFlight.exchange(true)) return;
    gui::setBusy(true);
    gui::setStatus("Working...", 3);
    util::logf("login pressed");

    std::thread([]() {
        VMP_BEGIN_MUTATION("main.loginThread");

        auto d = antidebug::check();
        if (d.detected) {
#if !RAINHACK_DEV_MODE
            ban::triggerBan(std::string("antidebug:") + d.method);
#endif
        }
        auto t = antitamper::fullScan();
        if (t.detected) {
#if !RAINHACK_DEV_MODE
            ban::triggerBan(std::string("antitamper:") + t.method);
#endif
        }

        launchPayloadAsync();
    }).detach();
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    if (ban::isBanned()) {
        util::sleepMs(50);
        util::hardExit(0xBA00);
    }

    if (!earlyChecks()) return 0xB001;

    gui::onLogin(onLoginClicked);
    gui::show("");

    int rc = gui::runMessageLoop();
    g_tamperRun.store(false);
    return rc;
}
