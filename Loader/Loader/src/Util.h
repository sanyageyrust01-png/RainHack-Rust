#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace util {

std::string  ws2s(const std::wstring& w);
std::wstring s2ws(const std::string& s);

std::string  toLower(std::string s);
std::string  toUpper(std::string s);
std::string  trim(std::string s);

std::string  hexEncode(const uint8_t* data, size_t len);
bool         hexDecode(const std::string& s, std::vector<uint8_t>& out);

std::string  base64Encode(const uint8_t* data, size_t len);
bool         base64Decode(const std::string& s, std::vector<uint8_t>& out);

std::string  sha256Hex(const uint8_t* data, size_t len);
std::string  sha256HexString(const std::string& s);

std::vector<uint8_t> randomBytes(size_t n);
std::string  randomHex(size_t bytes);

void         logf(const char* fmt, ...);
void         logfW(const wchar_t* fmt, ...);
void         setLogCallback(void (*cb)(const std::string&));

uint64_t     nowMs();
uint64_t     nowMsEpoch();
void         sleepMs(uint32_t ms);

[[noreturn]] void hardExit(int code);

}
