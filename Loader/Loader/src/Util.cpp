#include "Util.h"

#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <thread>
#include <mutex>
#include <wincrypt.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace util {

static std::mutex g_logMx;
static void (*g_logCb)(const std::string&) = nullptr;

std::string ws2s(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring s2ws(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string toLower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string toUpper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

std::string hexEncode(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out(len * 2, 0);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2 + 0] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0xF];
    }
    return out;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2 != 0) return false;
    out.clear(); out.reserve(s.size() / 2);
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int h = hv(s[i]); int l = hv(s[i + 1]);
        if (h < 0 || l < 0) return false;
        out.push_back((uint8_t)((h << 4) | l));
    }
    return true;
}

std::string base64Encode(const uint8_t* data, size_t len) {
    DWORD outLen = 0;
    if (!CryptBinaryToStringA(data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen)) return {};
    std::string out(outLen, 0);
    if (!CryptBinaryToStringA(data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &outLen)) return {};
    out.resize(outLen);
    return out;
}

bool base64Decode(const std::string& s, std::vector<uint8_t>& out) {
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(s.data(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &outLen, nullptr, nullptr)) return false;
    out.resize(outLen);
    if (!CryptStringToBinaryA(s.data(), (DWORD)s.size(), CRYPT_STRING_BASE64, out.data(), &outLen, nullptr, nullptr)) return false;
    out.resize(outLen);
    return true;
}

std::string sha256Hex(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); return {}; }
    BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0);
    std::vector<uint8_t> out(hashLen);
    BCryptFinishHash(h, out.data(), hashLen, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hexEncode(out.data(), out.size());
}

std::string sha256HexString(const std::string& s) {
    return sha256Hex((const uint8_t*)s.data(), s.size());
}

std::vector<uint8_t> randomBytes(size_t n) {
    std::vector<uint8_t> out(n);
    if (BCryptGenRandom(nullptr, out.data(), (ULONG)out.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (auto& b : out) b = (uint8_t)(GetTickCount64() & 0xff);
    }
    return out;
}

std::string randomHex(size_t bytes) {
    auto b = randomBytes(bytes);
    return hexEncode(b.data(), b.size());
}

void setLogCallback(void (*cb)(const std::string&)) {
    std::lock_guard<std::mutex> _(g_logMx);
    g_logCb = cb;
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::lock_guard<std::mutex> _(g_logMx);
    OutputDebugStringA("[RainHack] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (g_logCb) g_logCb(buf);
}

void logfW(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    OutputDebugStringW(L"[RainHack] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    auto s = ws2s(buf);
    std::lock_guard<std::mutex> _(g_logMx);
    if (g_logCb) g_logCb(s);
}

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t nowMsEpoch() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void sleepMs(uint32_t ms) { Sleep(ms); }

[[noreturn]] void hardExit(int code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    ExitProcess((UINT)code);
}

}
