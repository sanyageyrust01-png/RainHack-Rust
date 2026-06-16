#pragma once

#include <string>

namespace antivm {

struct Result {
    bool detected = false;
    std::string method;
    std::string detail;
};

Result check();

}
