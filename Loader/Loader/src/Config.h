#pragma once

#define RAINHACK_LOADER_VERSION         "1.0.0"
#define RAINHACK_LOADER_VERSION_W       L"1.0.0"
#define RAINHACK_LOADER_NAME            "RainHack"
#define RAINHACK_LOADER_NAME_W          L"RainHack"
#define RAINHACK_USER_AGENT             "RainHackLoader/1.0"
#define RAINHACK_USER_AGENT_W           L"RainHackLoader/1.0"

#define RAINHACK_TARGET_PROCESS         L"RustClient.exe"
#define RAINHACK_TARGET_FALLBACK        nullptr
#define RAINHACK_TARGET_WAIT_SECONDS    (300)
#define RAINHACK_INJECT_DELAY_SECONDS   (90)
#define RAINHACK_PROC_ALIVE_RECHECK_MS  (200)

#define RAINHACK_INTEGRITY_INTERVAL_MS  (4 * 1000)

#define RAINHACK_ENABLE_VMPROTECT       1
#define RAINHACK_DEV_MODE               0
