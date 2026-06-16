#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace injector {

DWORD findProcessId(const wchar_t* exeName);
DWORD waitForProcess(const wchar_t* primary, const wchar_t* fallback, unsigned timeoutSeconds);

bool isProcessAlive(DWORD pid);
bool ensureTargetAlive(const wchar_t* exeName, DWORD knownPid, DWORD& outPid, unsigned rescanTimeoutMs);

bool validateProcessModules(DWORD pid, std::string& reason);

bool driverLoaded();
bool unloadDriver();

__declspec(noinline) bool manualMapDll(DWORD targetPid,
                  const std::vector<uint8_t>& dllImage,
                  std::string& errorOut);

__declspec(noinline) bool kernelStreamMap(DWORD targetPid,
                  const std::vector<uint8_t>& encryptedBlob,
                  const std::vector<uint8_t>& sessionKey,
                  std::string& errorOut);

bool runKdmapper(const std::wstring& kdmapperExe,
                 const std::vector<uint8_t>& sysImage,
                 std::string& errorOut);

__declspec(noinline) bool runEmbeddedKdmapper(const std::vector<uint8_t>& sysImage,
                         std::string& errorOut);

}
