// Any_Ps3 — minimal JSON (integers only, no floats — by design).
// Used for cache manifests, Windows settings and the Windows game database.
#pragma once

#include "types.h"
#include <map>

namespace any_ps3 {

class JsonValue;
using JsonArray = std::vector<JsonValue>;

class JsonValue {
public:
    enum class Kind { Null, Bool, Int, Str, Arr, Obj };

    Kind kind = Kind::Null;
    bool b = false;
    long long i = 0;
    std::string s;
    JsonArray arr;
    std::vector<std::pair<std::string, JsonValue>> obj; // insertion order preserved

    JsonValue() = default;
    static JsonValue make_bool(bool v);
    static JsonValue make_int(long long v);
    static JsonValue make_str(std::string v);
    static JsonValue make_arr();
    static JsonValue make_obj();

    // Object access
    JsonValue* find(const std::string& key);
    const JsonValue* find(const std::string& key) const;
    JsonValue& set(const std::string& key, JsonValue v);
    std::string get_str(const std::string& key, const std::string& def = "") const;
    long long get_int(const std::string& key, long long def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;

    std::string dump() const;

    // Parse; on failure returns false and fills err. Depth capped (64).
    static bool parse(const std::string& text, JsonValue& out, std::string& err);
};

std::string json_escape(const std::string& in);

} // namespace any_ps3
