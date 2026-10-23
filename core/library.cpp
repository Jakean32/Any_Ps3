#include "library.h"

#include "minijson.h"
#include "util.h"

namespace anyps3 {

// ---------------- AppConfig ----------------
bool AppConfig::load(const std::string& dir) {
    meta_dir = dir;
    make_dirs(dir);
    cfg_path_ = path_join(dir, "config.json");
    std::vector<uint8_t> raw;
    if (!read_file(cfg_path_, &raw)) return true;  // fresh config
    JVal v;
    std::string err;
    if (!json_parse(std::string(raw.begin(), raw.end()), &v, &err)) return false;
    cache_root = v.get_str("cache_root", "");
    wizard_done = v.get_bool("wizard_done", false);
    legal_version_accepted = v.get_str("legal_version_accepted", "");
    const JVal& ex = v.at("extra");
    if (ex.is_obj() && ex.obj) {
        for (const auto& kv : *ex.obj) extra[kv.first] = kv.second.str;
    }
    return true;
}

bool AppConfig::save() const {
    // allow save() before any load(): derive the path from meta_dir
    std::string p = cfg_path_.empty() ? path_join(meta_dir, "config.json") : cfg_path_;
    JVal v = JVal::make_obj();
    v.set("cache_root", JVal::s(cache_root));
    v.set("wizard_done", JVal::bl(wizard_done));
    v.set("legal_version_accepted", JVal::s(legal_version_accepted));
    JVal ex = JVal::make_obj();
    for (const auto& kv : extra) ex.set(kv.first, JVal::s(kv.second));
    v.set("extra", ex);
    std::string text = json_dump(v);
    return write_file(p, text.data(), text.size());
}

std::string AppConfig::get(const std::string& key, const std::string& def) const {
    if (key == "cache_root") return cache_root;
    if (key == "wizard_done") return wizard_done ? "true" : "false";
    if (key == "legal_version_accepted") return legal_version_accepted;
    auto it = extra.find(key);
    return it == extra.end() ? def : it->second;
}

bool AppConfig::set(const std::string& key, const std::string& value) {
    if (key == "cache_root") { cache_root = value; return save(); }
    if (key == "wizard_done") { wizard_done = value == "true"; return save(); }
    if (key == "legal_version_accepted") { legal_version_accepted = value; return save(); }
    extra[key] = value;
    return save();
}

// ---------------- Library ----------------
Library::Library(AppConfig cfg) : cfg_(std::move(cfg)), import_(cfg_.meta_dir),
                                  cache_(cfg_.cache_root) {
    import_.load(nullptr);
}

std::vector<LibraryEntry> Library::entries(std::string* err) {
    std::vector<LibraryEntry> out;
    (void)err;
    for (const auto& g : import_.games()) {
        LibraryEntry e;
        e.rec = g;
        CacheManifest m;
        if (cache_.load(g.title_id, &m, nullptr)) {
            e.has_manifest = true;
            e.manifest = m;
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool Library::refresh(std::vector<std::string>* log_lines, std::string* err) {
    for (auto& g : *const_cast<std::vector<GameRecord>*>(&import_.games())) {
        import_.refresh(&g, err);
        if (log_lines) {
            log_lines->push_back((g.missing ? "[missing] " : "[ok] ") + g.title_id + " — " +
                                 g.title);
        }
        // staleness check when a manifest exists
        CacheManifest m;
        if (cache_.load(g.title_id, &m, nullptr) && !g.missing) {
            std::string sfo_hash;
            resolve_sfo_hash(g, &sfo_hash);
            if (cache_.is_stale(m, g, sfo_hash) && m.status == CacheStatus::Ready) {
                m.status = CacheStatus::Stale;
                m.note = "source changed since compile (hash mismatch)";
                m.updated_ms = now_ms();
                cache_.save(m, nullptr);
                if (log_lines)
                    log_lines->push_back("[stale] " + g.title_id + " — source changed, recompile recommended");
            }
        }
    }
    import_.save(nullptr);
    return true;
}

bool Library::resolve_sfo_hash(const GameRecord& rec, std::string* sfo_hash) {
    std::vector<uint8_t> sfo_data;
    bool got = false;
    if (rec.src_type == SourceType::IsoFile) {
        auto fs = FileSource::open(rec.src_ref, nullptr);
        if (fs) {
            auto iso = Iso9660::open(std::shared_ptr<ByteSource>(fs.get(), [](ByteSource*) {}), nullptr);
            if (iso) got = iso->read_file("PS3_GAME/PARAM.SFO", &sfo_data);
        }
    } else if (rec.src_type == SourceType::Folder) {
        read_folder_sfo(rec.src_ref, &sfo_data, nullptr);
        got = !sfo_data.empty();
    }
    if (!got) {
        *sfo_hash = "";
        return false;
    }
    *sfo_hash = Sha256::hex_of(sfo_data.data(), sfo_data.size());
    return true;
}

bool Library::import_iso_path(const std::string& path, std::string* err) {
    auto fs = FileSource::open(path, err);
    if (!fs) return false;
    if (!import_.import_iso(path, fs.get(), err)) return false;
    // ensure cache manifest exists (NotCompiled)
    GameRecord* r = import_.find([&] {
        std::vector<uint8_t> b;
        auto f2 = FileSource::open(path, nullptr);
        if (!f2) return std::string();
        auto iso = Iso9660::open(std::shared_ptr<ByteSource>(f2.get(), [](ByteSource*) {}), nullptr);
        if (!iso || !iso->read_file("PS3_GAME/PARAM.SFO", &b)) return std::string();
        Sfo s;
        if (!Sfo::parse(b.data(), b.size(), &s, nullptr)) return std::string();
        return s.title_id();
    }());
    if (r) {
        CacheManifest m;
        std::string sfo_hash;
        resolve_sfo_hash(*r, &sfo_hash);
        cache_.ensure(*r, &m, nullptr);
        CacheManifest m2;
        cache_.load(r->title_id, &m2, nullptr);
        m2.source_hash = CacheStore::source_hash_of(*r, sfo_hash);
        cache_.save(m2, nullptr);
        log_line("imported: " + r->title_id + " — " + r->title);
    }
    return import_.save(err);
}

bool Library::import_folder(const std::string& dir, std::string* err) {
    if (!import_.import_folder_game(dir, err)) return false;
    std::vector<uint8_t> b;
    std::string ps3_dir;
    std::string tid;
    if (read_folder_sfo(dir, &b, &ps3_dir)) {
        Sfo s;
        if (Sfo::parse(b.data(), b.size(), &s, nullptr)) tid = s.title_id();
    }
    GameRecord* r = import_.find(tid);
    if (r) {
        CacheManifest m;
        std::string sfo_hash;
        resolve_sfo_hash(*r, &sfo_hash);
        cache_.ensure(*r, &m, nullptr);
        CacheManifest m2;
        cache_.load(r->title_id, &m2, nullptr);
        m2.source_hash = CacheStore::source_hash_of(*r, sfo_hash);
        cache_.save(m2, nullptr);
        log_line("imported: " + r->title_id + " — " + r->title);
    }
    return import_.save(err);
}

bool Library::import_saf_uri(const std::string& uri, int fd, bool owns_fd, std::string* err) {
    auto src = FdSource::attach(fd, owns_fd, err);
    if (!src) return false;
    // import via the ISO path over the fd
    if (!import_.import_iso(uri, src.get(), err)) {
        // not an ISO — folder URIs are scanned through platform layer
        if (err && err->find("PS3_GAME/PARAM.SFO") != std::string::npos) {
            *err = "SAF source has no PS3_GAME/PARAM.SFO";
        }
        return false;
    }
    // find record by URI
    GameRecord* r = nullptr;
    for (auto& g : *const_cast<std::vector<GameRecord>*>(&import_.games()))
        if (g.src_ref == uri) r = &g;
    if (r) {
        r->src_type = SourceType::SafUri;
        r->src_size = src->size();
        CacheManifest m;
        std::string sfo_hash;
        resolve_sfo_hash(*r, &sfo_hash);  // ISO case handled below
        cache_.ensure(*r, &m, nullptr);
        CacheManifest m2;
        cache_.load(r->title_id, &m2, nullptr);
        m2.source_ref = uri;
        m2.source_size = r->src_size;
        m2.source_hash = CacheStore::source_hash_of(*r, sfo_hash);
        cache_.save(m2, nullptr);
        log_line("imported (SAF): " + r->title_id + " — " + r->title);
    }
    return import_.save(err);
}

bool Library::check_saf_uri_present(const std::string& uri, int fd, std::string* err) {
    auto src = FdSource::attach(fd, true, err);
    if (!src) return false;
    // fd stays open only for this check; random reads verified
    uint8_t probe[16];
    bool readable = src->read(0, sizeof probe, probe);
    if (!readable && err) *err = "fd not readable at offset 0";
    return readable;
}

bool Library::import_saf_folder(const std::string& game_dir_uri, const std::string& sfo_uri,
                                int sfo_fd, int icon_fd, int pic1_fd, std::string* err) {
    std::vector<uint8_t> sfo_data, icon_data, pic1_data;
    auto sfo_src = FdSource::attach(sfo_fd, true, err);
    if (!sfo_src) return false;
    sfo_data.resize((size_t)sfo_src->size());
    if (!sfo_data.empty() && !sfo_src->read(0, (uint32_t)sfo_data.size(), sfo_data.data())) {
        if (err) *err = "cannot read PARAM.SFO from fd";
        return false;
    }
    auto read_all = [&](int fd, std::vector<uint8_t>* out) {
        if (fd < 0) return;
        auto src = FdSource::attach(fd, true, nullptr);
        if (!src) return;
        out->resize((size_t)src->size());
        if (!out->empty()) src->read(0, (uint32_t)out->size(), out->data());
    };
    read_all(icon_fd, &icon_data);
    read_all(pic1_fd, &pic1_data);

    if (!import_.import_from_parts(game_dir_uri, SourceType::SafUri, sfo_data,
                                   icon_data.empty() ? nullptr : &icon_data,
                                   pic1_data.empty() ? nullptr : &pic1_data, err))
        return false;
    // ensure manifest + refresh UI log
    std::string tid;
    {
        Sfo s;
        if (Sfo::parse(sfo_data.data(), sfo_data.size(), &s, nullptr)) tid = s.title_id();
    }
    GameRecord* r = import_.find(tid);
    if (r) {
        CacheManifest m;
        cache_.ensure(*r, &m, nullptr);
        log_line("imported (SAF folder): " + r->title_id + " — " + r->title);
    }
    return import_.save(err);
}

std::string Library::status_ui_name(CacheStatus s) {
    switch (s) {
        case CacheStatus::NotCompiled: return "Not compiled";
        case CacheStatus::Compiling: return "Compiling…";
        case CacheStatus::Ready: return "Ready";
        case CacheStatus::Stale: return "Out of date";
        case CacheStatus::Failed: return "Failed";
    }
    return "?";
}

CacheStatus Library::status_of(const std::string& title_id, std::string* note) {
    const GameRecord* r = import_.find(title_id);
    if (!r) return CacheStatus::NotCompiled;
    CacheManifest m;
    if (!cache_.load(title_id, &m, nullptr)) return CacheStatus::NotCompiled;
    if (note) *note = m.note;
    return m.status;
}

bool Library::compile(const std::string& title_id, const ProgressFn& progress, std::string* err) {
    GameRecord* r = import_.find(title_id);
    if (!r) {
        if (err) *err = "unknown game: " + title_id;
        return false;
    }
    if (r->missing) {
        if (err) *err = "source missing — reselect the file/folder";
        return false;
    }
    CacheStore* cs = &cache_;
    CompilePipeline pipe(cs, cache_.logs_dir(title_id));
    CacheManifest m;
    ProgressFn wrapped = [this, progress](const CompileProgress& p) {
        std::string frac = p.fraction > 0 ? std::to_string((int)(p.fraction * 100)) + "%" : "—";
        log_line("[" + p.stage + "] " + frac + " " + p.note);
        if (progress) progress(p);
    };
    CacheStatus st = pipe.run(*r, wrapped, &m, err);
    return st != CacheStatus::Failed;
}

bool Library::recompile(const std::string& title_id, const ProgressFn& progress, std::string* err) {
    // recompile = delete existing manifest result and run again
    GameRecord* r = import_.find(title_id);
    if (!r) {
        if (err) *err = "unknown game: " + title_id;
        return false;
    }
    CacheManifest m;
    if (cache_.load(title_id, &m, nullptr)) {
        m.status = CacheStatus::NotCompiled;
        m.note = "recompile requested";
        cache_.save(m, nullptr);
    }
    return compile(title_id, progress, err);
}

bool Library::delete_cache(const std::string& title_id, std::string* err) {
    return cache_.delete_cache(title_id, err);
}

bool Library::remove_game(const std::string& title_id, bool also_delete_cache, std::string* err) {
    if (also_delete_cache) cache_.delete_cache(title_id, nullptr);
    bool ok = import_.remove_record(title_id, err);
    if (ok) log_line("removed from library: " + title_id);
    return ok;
}

}  // namespace anyps3
