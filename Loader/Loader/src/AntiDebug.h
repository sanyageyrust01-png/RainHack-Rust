#pragma once

#include <string>

namespace antidebug {

struct Result {
    bool detected = false;
    std::string method;
    std::string detail;
};

Result check();

bool isDebuggerPresentBasic();
void hideThreadFromDebugger();

}
