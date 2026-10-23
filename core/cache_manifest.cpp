#include "cache_manifest.h"

#include "minijson.h"
#include "util.h"

namespace anyps3 {

CacheStore::CacheStore(std::string root) : root_(std::move(root)) {
    make_dirs(root_);
}

std::string CacheStore::game_dir(const std::string& id) const { return path_join(root_, id); }
std::string CacheStore::manifest_path(const std::string& id) const {
    return path_join(game_dir(id), "manifest.json");
}
std::string CacheStore::logs_dir(const std::string& id) const { return path_join(game_dir(id), "logs"); }
std::string CacheStore::code_dir(const std::string& id) const { return path_join(game_dir(id), "code"); }

bool CacheStore::exists(const std::string& game_id) const {
    return file_exists(manifest_path(game_id));
}

std::string CacheStore::source_hash_of(const GameRecord& rec, const std::string& sfo_hash) {
    Sha256 h;
    std::string meta = rec.src_ref + "|" + std::to_string(rec.src_size);
    h.update(meta.data(), meta.size());
    h.update(sfo_hash.data(), sfo_hash.size());
    return h.hex();
}

bool CacheStore::is_stale(const CacheManifest& m, const GameRecord& rec,
                          const std::string& sfo_hash) const {
    return m.source_hash != source_hash_of(rec, sfo_hash);
}

bool CacheStore::compatible(const CacheManifest& m, std::string* why_not) {
    if (m.format_version > kManifestFormatVersion) {
        if (why_not) *why_not = "manifest format " + std::to_string(m.format_version) +
                                " is newer than supported " + std::to_string(kManifestFormatVersion);
        return false;
    }
    return true;
}

bool CacheStore::ensure(const GameRecord& rec, CacheManifest* out, std::string* err) {
    make_dirs(game_dir(rec.title_id));
    make_dirs(logs_dir(rec.title_id));
    make_dirs(code_dir(rec.title_id));
    if (load(rec.title_id, out, nullptr)) {
        std::string why;
        if (!compatible(*out, &why)) {
            if (err) *err = why + " — cache ignored (delete and recompile)";
            return false;
        }
        return true;  // existing manifest
    }
    CacheManifest m;
    m.anyps3_version = kVersion;
    m.game_id = rec.title_id;
    m.source_ref = rec.src_ref;
    m.source_size = rec.src_size;
    m.source_hash = source_hash_of(rec, "");
    m.status = CacheStatus::NotCompiled;
    m.created_ms = now_ms();
    m.updated_ms = m.created_ms;
    m.note = "created by library import; no compile run yet";
    if (!save(m, err)) return false;
    *out = m;
    return true;
}

static JVal manifest_to_json(const CacheManifest& m) {
    JVal v = JVal::make_obj();
    v.set("format_version", JVal::n(m.format_version));
    v.set("anyps3_version", JVal::s(m.anyps3_version));
    v.set("game_id", JVal::s(m.game_id));
    v.set("source_hash", JVal::s(m.source_hash));
    v.set("source_ref", JVal::s(m.source_ref));
    v.set("source_size", JVal::n((double)m.source_size));
    v.set("status", JVal::s(cache_status_name(m.status)));
    v.set("created_ms", JVal::n((double)m.created_ms));
    v.set("updated_ms", JVal::n((double)m.updated_ms));
    v.set("note", JVal::s(m.note));
    JVal st = JVal::make_arr();
    for (const auto& s : m.stages) {
        JVal o = JVal::make_obj();
        o.set("name", JVal::s(s.name));
        o.set("status", JVal::s(s.status));
        o.set("note", JVal::s(s.note));
        st.push(o);
    }
    v.set("stages", st);
    return v;
}

bool CacheStore::save(const CacheManifest& m, std::string* err) const {
    if (m.game_id.empty()) {
        if (err) *err = "manifest has empty game_id";
        return false;
    }
    make_dirs(game_dir(m.game_id));
    std::string text = json_dump(manifest_to_json(m));
    return write_file(manifest_path(m.game_id), text.data(), text.size());
}

bool CacheStore::load(const std::string& game_id, CacheManifest* out, std::string* err) const {
    std::vector<uint8_t> raw;
    if (!read_file(manifest_path(game_id), &raw)) {
        if (err) *err = "no manifest for " + game_id;
        return false;
    }
    JVal v;
    if (!json_parse(std::string(raw.begin(), raw.end()), &v, err)) return false;
    CacheManifest m;
    m.format_version = (int)v.get_int("format_version", 0);
    m.anyps3_version = v.get_str("anyps3_version", "");
    m.game_id = v.get_str("game_id", game_id);
    m.source_hash = v.get_str("source_hash", "");
    m.source_ref = v.get_str("source_ref", "");
    m.source_size = (uint64_t)v.get_int("source_size", 0);
    m.status = cache_status_from(v.get_str("status", "not_compiled"));
    m.created_ms = v.get_int("created_ms", 0);
    m.updated_ms = v.get_int("updated_ms", 0);
    m.note = v.get_str("note", "");
    const JVal& st = v.at("stages");
    if (st.is_arr() && st.arr) {
        for (const auto& s : *st.arr) {
            StageInfo si;
            si.name = s.get_str("name", "");
            si.status = s.get_str("status", "pending");
            si.note = s.get_str("note", "");
            m.stages.push_back(std::move(si));
        }
    }
    *out = m;
    return true;
}

bool CacheStore::delete_cache(const std::string& game_id, std::string* err) {
    std::string dir = game_dir(game_id);
    if (!dir_exists(dir)) {
        if (err) *err = "no cache for " + game_id;
        return false;
    }
    if (!remove_tree(dir)) {
        if (err) *err = "failed to remove " + dir;
        return false;
    }
    return true;
}

}  // namespace anyps3
