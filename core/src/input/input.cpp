#include "anyps3/input/input.h"

namespace anyps3 {

u32 PadMapping::ButtonFromName(const std::string& name) {
    struct N {
        const char* s;
        u32 b;
    };
    static const N table[] = {
        {"cross", PAD_CROSS},   {"circle", PAD_CIRCLE},   {"square", PAD_SQUARE},
        {"triangle", PAD_TRIANGLE}, {"start", PAD_START}, {"select", PAD_SELECT},
        {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2},
        {"l3", PAD_L3}, {"r3", PAD_R3},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
    };
    for (const auto& e : table) {
        if (name == e.s) return e.b;
    }
    return PAD_NONE;
}

}  // namespace anyps3
