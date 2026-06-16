#pragma once
#include <cstddef>
#include <cstdint>

namespace ESP {
    extern bool bESP;
    extern bool bName;
    extern bool bBox;
    extern bool bHealth;
    extern bool bDistance;

    void Render();

    // Returns a pointer to the last-populated entity list snapshot and its
    // count. The snapshot is updated inside Render(); callers get last
    // frame's data. Returns 0 count before the first successful Render.
    size_t GetEntityList(const uintptr_t** outPtr);
}
