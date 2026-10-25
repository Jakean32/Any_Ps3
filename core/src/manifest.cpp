#include "any_ps3/manifest.h"
#include "any_ps3/bytes.h"

#include <ctime>
#include <cstdio>

namespace any_ps3 {

std::string utc_now_iso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

ManifestStage* Manifest::stage(const char* id) {
    for (auto& s : stages)
        if (s.id == id) return &s;
    return nullptr;
}

void Manifest::set_stage(const char* id, const char* state, std::string detail) {
    ManifestStage* s = stage(id);
    if (s) {
        s->state = state;
        s->detail = std::move(detail);
    } else {
        stages.push_back({id, state, std::move(detail)});
    }
}

std::string Manifest::to_json() const {
    JsonValue j = JsonValue::make_obj();
    j.set("format", JsonValue::make_int(format));
    j.set("game_id", JsonValue::make_str(game_id));
    j.set("title", JsonValue::make_str(title));
    j.set("source_kind", JsonValue::make_str(source_kind));
    j.set("source_hash", JsonValue::make_str(source_hash));
    j.set("source_size", JsonValue::make_int((long long)source_size));
    j.set("engine_version", JsonValue::make_str(engine_version));
    j.set("created_utc", JsonValue::make_str(created_utc));
    j.set("updated_utc", JsonValue::make_str(updated_utc));
    j.set("status", JsonValue::make_str(status));
    JsonValue arr = JsonValue::make_arr();
    for (const auto& s : stages) {
        JsonValue o = JsonValue::make_obj();
        o.set("id", JsonValue::make_str(s.id));
        o.set("state", JsonValue::make_str(s.state));
        o.set("detail", JsonValue::make_str(s.detail));
        arr.arr.push_back(std::move(o));
    }
    j.set("stages", std::move(arr));
    return j.dump();
}

bool Manifest::from_json(const std::string& text, Manifest& out, std::string& err) {
    JsonValue j;
    if (!JsonValue::parse(text, j, err)) return false;
    if (j.kind != JsonValue::Kind::Obj) { err = "manifest root must be an object"; return false; }
    long long fmt = j.get_int("format", 0);
    if (fmt < 1 || fmt > 999) { err = "unsupported manifest format"; return false; }
    out.format = (int)fmt;
    out.game_id = j.get_str("game_id");
    out.title = j.get_str("title");
    out.source_kind = j.get_str("source_kind");
    out.source_hash = j.get_str("source_hash");
    out.source_size = (uint64_t)j.get_int("source_size", 0);
    out.engine_version = j.get_str("engine_version");
    out.created_utc = j.get_str("created_utc");
    out.updated_utc = j.get_str("updated_utc");
    out.status = j.get_str("status");
    out.stages.clear();
    JsonValue* arr = j.find("stages");
    if (arr && arr->kind == JsonValue::Kind::Arr) {
        for (auto& e : arr->arr) {
            if (e.kind != JsonValue::Kind::Obj) continue;
            ManifestStage s;
            s.id = e.get_str("id");
            s.state = e.get_str("state");
            s.detail = e.get_str("detail");
            out.stages.push_back(std::move(s));
        }
    }
    if (out.game_id.empty()) { err = "manifest missing game_id"; return false; }
    if (out.status.empty()) out.status = "not_compiled";
    return true;
}

bool manifest_is_outdated(const Manifest& m, const std::string& current_source_hash) {
    if (m.source_hash.empty()) return false;
    // A manifest only becomes "outdated" once there is something compiled.
    if (m.status != "ready" && m.status != "analyzed" && m.status != "not_compiled") return false;
    return m.source_hash != current_source_hash;
}

bool manifest_write_fd(int fd, Manifest& m, std::string& err) {
    m.updated_utc = utc_now_iso8601();
    if (m.created_utc.empty()) m.created_utc = m.updated_utc;
    std::string text = m.to_json();
    FdWriter w(fd);
    if (!w.write_str(text)) { err = "write failed"; return false; }
    return true;
}

bool manifest_read_fd(int fd, Manifest& out, std::string& err) {
    FdSource src(fd);
    uint64_t sz = src.size();
    if (sz > 1024 * 1024) { err = "manifest implausibly large"; return false; }
    std::string text;
    text.resize((size_t)sz);
    if (sz && !src.read_exact(0, text.data(), text.size())) { err = "read failed"; return false; }
    return Manifest::from_json(text, out, err);
}

bool manifest_write_path(const std::string& path, Manifest& m, std::string& err) {
    m.updated_utc = utc_now_iso8601();
    if (m.created_utc.empty()) m.created_utc = m.updated_utc;
    std::string text = m.to_json();
    if (!write_file_path(path, text.data(), text.size())) { err = "cannot write " + path; return false; }
    return true;
}

bool manifest_read_path(const std::string& path, Manifest& out, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file_path(path, data, 1024 * 1024)) { err = "cannot read " + path; return false; }
    std::string text((const char*)data.data(), data.size());
    return Manifest::from_json(text, out, err);
}

} // namespace any_ps3
