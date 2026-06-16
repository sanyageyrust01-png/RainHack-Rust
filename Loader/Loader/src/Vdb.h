#pragma once

#include <string>

namespace vdb {

struct Status {
    bool blocklistEnabled = false;
    bool hvciEnabled      = false;
    bool dseEnabled       = false;
    std::string detail;
};

__declspec(noinline) Status check();

__declspec(noinline) bool disable(std::string& err);

bool scheduleReboot(unsigned seconds, std::string& err);

}
