#pragma once

#include <string>

namespace ban {

bool isBanned();

[[noreturn]] void triggerBan(const std::string& reason);

}
