#pragma once

#include <string>

namespace antitamper {

struct Result {
    bool detected = false;
    std::string method;
    std::string detail;
};

Result scanProcesses();
Result scanWindows();
Result scanModules();
Result selfIntegrityCheck();
Result fullScan();

}
