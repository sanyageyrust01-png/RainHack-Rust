#pragma once

#include <cstdint>
#include <vector>

namespace resources {

bool loadCheatBlob(std::vector<uint8_t>& outEncryptedBlob);

bool loadDriverSys(std::vector<uint8_t>& outDecrypted);

bool loadKdmapperExe(std::vector<uint8_t>& outDecrypted);

const uint8_t* cheatSessionKey();
size_t         cheatSessionKeyLen();

}
