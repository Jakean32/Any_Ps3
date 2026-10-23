#pragma once
// Minimal JSON value model + parser + writer used for library.json,
// cache manifests and app config. Strictly scoped: objects, arrays,
// strings (with \uXXXX escapes for our needs), numbers, bools, null.
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace anyps3 {

class JVal;
using JObj = std::map<std::string, JVal>;
using JArr = std::vector<JVal>;

class JVal {
public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::shared_ptr<JArr> arr;
    std::shared_ptr<JObj> obj;

    JVal() = default;
    static JVal make_obj() { JVal v; v.type = Obj; v.obj = std::make_shared<JObj>(); return v; }
    static JVal make_arr() { JVal v; v.type = Arr; v.arr = std::make_shared<JArr>(); return v; }
    static JVal s(const std::string& x) { JVal v; v.type = Str; v.str = x; return v; }
    static JVal n(double x) { JVal v; v.type = Num; v.num = x; return v; }
    static JVal bl(bool x) { JVal v; v.type = Bool; v.b = x; return v; }

    bool is_obj() const { return type == Obj; }
    bool is_arr() const { return type == Arr; }
    bool is_str() const { return type == Str; }
    bool is_num() const { return type == Num; }
    bool is_bool() const { return type == Bool; }

    // Object access with defaults (never throws).
    const JVal& at(const std::string& key) const;           // Null val if missing
    std::string get_str(const std::string& key, const std::string& def) const;
    double get_num(const std::string& key, double def) const;
    int64_t get_int(const std::string& key, int64_t def) const;
    bool get_bool(const std::string& key, bool def) const;
    void set(const std::string& key, const JVal& v);        // requires is_obj
    void push(const JVal& v);                               // requires is_arr
};

// Parse text; returns false and fills `err` on malformed input.
bool json_parse(const std::string& text, JVal* out, std::string* err);
// Serialize (compact).
std::string json_dump(const JVal& v);
// Escape a string as a JSON string literal (with quotes).
std::string json_quote(const std::string& s);

}  // namespace anyps3
