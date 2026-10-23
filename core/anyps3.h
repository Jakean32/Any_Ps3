// Any_Ps3 — AOT-recompilation-based PS3 launcher
// Core public header. Platform-neutral C++17, no external dependencies.
//
// Legal note: Any_Ps3 contains no Sony firmware, keys, or game content.
// It only reads user-provided game images the user already owns.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace anyps3 {

inline constexpr const char* kVersion = "0.3.0";
inline constexpr const char* kCodename = "App Shell";
inline constexpr int kManifestFormatVersion = 1;
inline constexpr const char* kLegalNotice =
    "Any_Ps3 contains no Sony firmware, decryption keys, or game content. "
    "You may only import and compile games you legally own. "
    "AOT recompilation is an original, clean-room engineering effort.";

// Central registry of honest capability status reported by the UI.
enum class StageStatus {
    Ok,               // implemented and succeeded
    NotImplemented,   // stage exists in design, not implemented yet
    Blocked,          // implemented but blocked (e.g. encrypted EBOOT.BIN)
    Failed,           // implemented but failed
    Pending,          // not started
};

inline const char* stage_status_name(StageStatus s) {
    switch (s) {
        case StageStatus::Ok: return "ok";
        case StageStatus::NotImplemented: return "not_implemented";
        case StageStatus::Blocked: return "blocked";
        case StageStatus::Failed: return "failed";
        case StageStatus::Pending: return "pending";
    }
    return "?";
}

}  // namespace anyps3
