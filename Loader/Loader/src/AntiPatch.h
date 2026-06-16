#pragma once

#include <cstdint>
#include <string>

namespace antipatch {

bool init();

bool verify();

const std::string& lastFault();

}
