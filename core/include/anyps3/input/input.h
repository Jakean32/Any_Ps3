// Any_Ps3 — input abstraction: guest pad state + per-game mapping.
#pragma once

#include <map>
#include <string>

#include "anyps3/config.h"
#include "anyps3/types.h"

namespace anyps3 {

// Guest pad buttons (PS3 pad layout subset).
enum PadButton : u32 {
    PAD_NONE = 0,
    PAD_SELECT = 1 << 0,
    PAD_L3 = 1 << 1,
    PAD_R3 = 1 << 2,
    PAD_START = 1 << 3,
    PAD_UP = 1 << 4,
    PAD_RIGHT = 1 << 5,
    PAD_DOWN = 1 << 6,
    PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8,
    PAD_R2 = 1 << 9,
    PAD_L1 = 1 << 10,
    PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12,
    PAD_CIRCLE = 1 << 13,
    PAD_CROSS = 1 << 14,
    PAD_SQUARE = 1 << 15,
};

struct PadState {
    u32 buttons = 0;         // currently held (bitmask of PadButton)
    float left_x = 0, left_y = 0;
    float right_x = 0, right_y = 0;

    bool Held(PadButton b) const { return (buttons & b) != 0; }
    void Press(PadButton b) { buttons |= b; }
    void Release(PadButton b) { buttons &= ~b; }
};

// Map named GameSettings entries ("pad_cross" etc.) to PadButton values.
class PadMapping {
public:
    void Load(const GameSettings& settings) {
        map_.clear();
        for (const auto& [name, host_code] : settings.controller) {
            u32 guest = ButtonFromName(name);
            if (guest != PAD_NONE) map_[host_code] = guest;
        }
    }
    // Translate a host button event (keycode / touch id) to the guest button.
    u32 Translate(int host_code, bool pressed, PadState* pad) const {
        auto it = map_.find(host_code);
        if (it == map_.end()) return PAD_NONE;
        u32 guest = it->second;
        if (pressed) pad->Press(static_cast<PadButton>(guest));
        else pad->Release(static_cast<PadButton>(guest));
        return guest;
    }
    size_t size() const { return map_.size(); }

    static u32 ButtonFromName(const std::string& name);

private:
    std::map<int, u32> map_;  // host code -> guest button
};

}  // namespace anyps3
