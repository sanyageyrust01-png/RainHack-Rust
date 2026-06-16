# RainHack Loader

C++17 native loader for Windows x64. Authenticates the user against the RainHack server, downloads the encrypted cheat DLL into memory, and manual-maps it into the target process. Runs an in-process watchdog that constantly performs anti-debug / anti-VM / anti-tamper checks and a server heartbeat. **Any tamper detection bans the key + hardware on the server and terminates the loader immediately.**

## Build

Requires **Visual Studio 2022** with the *Desktop development with C++* workload (any edition, including Build Tools).

### From the IDE

```
Loader\Loader.sln  →  Release | x64  →  Build
```

### From the command line

```powershell
cd c:\Users\valera\Documents\RainHack\Loader
.\build.bat Release
```

Output:

```
Loader\build\Release\RainHackLoader.exe
```

## Configure for your server

Edit `Loader\src\Config.h`:

| Macro | Meaning |
| --- | --- |
| `RAINHACK_SERVER_HOST` | Server hostname (`L"your.server.com"` for production) |
| `RAINHACK_SERVER_PORT` | Server port |
| `RAINHACK_USE_HTTPS`   | `1` for TLS, `0` for plain HTTP (only for local dev) |
| `RAINHACK_REQUIRE_TLS` | `1` to refuse to run unless HTTPS is configured |
| `RAINHACK_LOADER_PSK`  | 32-byte AES-256-GCM pre-shared key. **Must match the server's `LOADER_PSK` in `.env`** |
| `RAINHACK_TARGET_PROCESS` | Game process to inject into (default: `RustClient.exe`) |

The PSK is the foundation of all loader↔server traffic confidentiality. Get the value the server printed on `npm run init` and replace the bytes in `RAINHACK_LOADER_PSK` (16 hex pairs of two = 32 bytes).

> A wrong PSK = every request returns `dec-fail` and the loader bails out at handshake.

## VMProtect

The whole binary is designed to be wrapped by VMProtect after build. Flip `RAINHACK_ENABLE_VMPROTECT` to `1` in `Config.h` and add the real `VMProtectSDK_real.h` + the static library to the project. The stubs in `VMProtectSDK.h` already mark the critical regions:

- `earlyChecks` — pre-login self-protection
- `login` — auth code path
- `payload` — payload fetch + manual map

After VMProtect mutates these regions and turns on its own anti-debug, the SDK runtime checks (`VMProtectIsDebuggerPresent`, `VMProtectIsVirtualMachinePresent`, `VMProtectIsValidImageCRC`) become real and trigger the same tamper-report → ban → terminate flow.

## Hardware fingerprint

`Hardware.cpp` collects:

- **HWID** = SHA-256(`SMBIOS UUID | SMBIOS Serial | MachineGUID | CPUID | MAC | Disk Serial`)
- **GPUID** = SHA-256(`gpuName | vendorId | deviceId`) (DXGI)
- **MachineGUID** = `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`

The IP and region/country are taken **server-side** from the connection — the loader never asks the user for them.

## What it detects

- **AntiDebug**: `IsDebuggerPresent`, `CheckRemoteDebuggerPresent`, PEB `BeingDebugged`/`NtGlobalFlag`/`HeapFlags`, `NtQueryInformationProcess` (DebugPort/Object/Flags), kernel debugger info, hardware breakpoints (Dr0–Dr3), `NtClose` invalid-handle exception, `OutputDebugString` last-error trick, RDTSC timing, foreign desktop.
- **AntiVM**: CPUID hypervisor bit + brand string, SMBIOS strings (VMware/VBox/QEMU/Xen/Parallels/KVM/Hyper-V), MAC OUI vendor (00:05:69, 00:0C:29, 00:1C:14, 00:50:56, 08:00:27, 00:16:3E, 00:1C:42, 00:15:5D), guest driver files, guest tools registry keys, loaded modules.
- **AntiTamper**: process scan (x64dbg, IDA, Cheat Engine, Scylla, ReClass, Fiddler, Wireshark, Process Hacker, dnSpy, …), window class scan, suspicious modules (ScyllaHide, TitanHide, …), self-section integrity (writable executable section = injected code).

The watchdog re-runs all of these every `RAINHACK_INTEGRITY_INTERVAL_MS` (default 4s).

## What happens on detection

1. The loader builds an evidence JSON (`{ method, detail, loader_version }`).
2. POSTs to `/api/v1/loader/report-tamper` (encrypted envelope).
3. Server bans:
   - the **key** (status = banned, all sessions killed)
   - the **HWID**
   - the **GPUID**
   - the **MachineGUID**
   - the **IP**
4. Loader calls `TerminateProcess` on itself.

After that, **any future login attempt** from the same hardware fails with `banned` no matter what key is used.

## Run

The loader requires admin rights (manifest enforces `requireAdministrator` because of manual-map + optional kdmapper).

```
RainHackLoader.exe
```

UI shows: motd from server, license key field, status line, log. Enter your key, click *Sign in*. After successful auth the window hides and the loader stays alive in the background to keep the heartbeat running. **Killing the loader = killing the cheat session** (heartbeat stops, watchdog stops, but injected DLL keeps running until game process closes).

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Login failed: dec-fail` (server log) | PSK mismatch between `Config.h` and server `.env` |
| `Login failed: hwid-mismatch`         | The key is bound to another machine. Reset HWID from admin panel. |
| `Inject failed: OpenProcess-failed`   | Anti-cheat blocked the loader. Need driver mode (`Allow driver` toggle on server). |
| `Payload error: payload-missing`      | `server/data/cheat.dll` not present on the server. |
| Loader closes immediately after start | Tamper trigger before login. Check `OutputDebugString` log via DebugView. |
