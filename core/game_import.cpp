#include "game_import.h"

#include "minijson.h"
#include "thumbs.h"
#include "util.h"

namespace anyps3 {

static const char* src_type_name(SourceType t) {
    switch (t) {
        case SourceType::Folder: return "folder";
        case SourceType::IsoFile: return "iso";
        case SourceType::SafUri: return "saf_uri";
    }
    return "?";
}
static SourceType src_type_from(const std::string& s) {
    if (s == "iso") return SourceType::IsoFile;
    if (s == "saf_uri") return SourceType::SafUri;
    return SourceType::Folder;
}

bool looks_like_folder_game(const std::string& dir) {
    std::vector<uint8_t> sfo;
    std::string sub;
    return read_folder_sfo(dir, &sfo, &sub);
}

bool read_folder_sfo(const std::string& dir, std::vector<uint8_t>* out, std::string* ps3_game_dir) {
    // layout A: <dir>/PS3_GAME/PARAM.SFO ; layout B: <dir>/PARAM.SFO
    static const char* kLayouts[] = {"PS3_GAME/PARAM.SFO", "PARAM.SFO"};
    for (const char* rel : kLayouts) {
        std::string p = path_join(dir, rel);
        if (read_file(p, out)) {
            if (ps3_game_dir) *ps3_game_dir = path_dirname(p);
            return true;
        }
    }
    return false;
}

GameImport::GameImport(std::string meta_dir)
    : meta_dir_(std::move(meta_dir)),
      lib_path_(path_join(meta_dir_, "library.json")),
      thumbs_dir_(path_join(meta_dir_, "thumbs")) {
    make_dirs(meta_dir_);
    make_dirs(thumbs_dir_);
}

GameRecord* GameImport::find(const std::string& title_id) {
    for (auto& g : games_)
        if (g.title_id == title_id) return &g;
    return nullptr;
}
const GameRecord* GameImport::find(const std::string& title_id) const {
    for (const auto& g : games_)
        if (g.title_id == title_id) return &g;
    return nullptr;
}

bool GameImport::import_sfo_based(const std::string& ref, SourceType type, ByteSource* src,
                                  const Sfo& sfo, const std::string& icon_rel,
                                  const std::string& pic1_rel, Iso9660* iso,
                                  const std::string& folder_dir, std::string* err) {
    std::string id = sfo.title_id();
    std::string title = sfo.title();
    if (id.empty() || title.empty()) {
        if (err) *err = "PARAM.SFO lacks TITLE_ID/TITLE";
        return false;
    }
    GameRecord rec;
    GameRecord* existing = find(id);
    if (existing) rec = *existing;
    rec.title = title;
    rec.title_id = id;
    rec.app_ver = sfo.app_ver();
    rec.category = sfo.category();
    rec.src_type = type;
    rec.src_ref = ref;
    rec.missing = false;
    rec.last_seen_ms = now_ms();
    if (rec.added_ms == 0) rec.added_ms = rec.last_seen_ms;

    // --- extract covers into the thumbnail cache (no game data copied) ---
    auto extract = [&](const std::string& rel_in, std::string* out_path, int max_w, bool jpeg) {
        std::vector<uint8_t> bytes;
        bool got = false;
        if (iso) {
            got = iso->read_file(rel_in, &bytes, 8 * 1024 * 1024);
        } else if (!folder_dir.empty()) {
            got = read_file(path_join(folder_dir, path_filename(rel_in)), &bytes);
        } else if (src) {
            // direct open via source assumed to be the icon; not used here
            got = false;
        }
        if (!got) return false;
        std::string suffix = jpeg ? ".jpg" : ".png";
        std::string outp = path_join(thumbs_dir_, id + "_" + path_filename(rel_in));
        // normalize extension
        std::string stem = outp.substr(0, outp.size() - path_extension(outp).size());
        outp = stem + suffix;
        ThumbInfo ti = make_thumbnail(outp, bytes.data(), bytes.size(), max_w, jpeg);
        if (ti.ok) {
            *out_path = ti.path;
            return true;
        }
        return false;
    };

    std::string icon_rel_l = icon_rel;
    std::string pic1_rel_l = pic1_rel;
    std::string ip, pp;
    if (extract(icon_rel_l, &ip, 320, false)) rec.icon_path = ip;
    if (extract(pic1_rel_l, &pp, 480, true)) rec.pic1_path = pp;

    if (existing) {
        *existing = rec;
        return true;
    }
    games_.push_back(std::move(rec));
    return true;
}

bool GameImport::import_iso(const std::string& ref, ByteSource* src, std::string* err) {
    if (!src) {
        if (err) *err = "null source for ISO import";
        return false;
    }
    std::string ierr;
    auto iso = Iso9660::open(std::shared_ptr<ByteSource>(src, [](ByteSource*) {}), &ierr);
    if (!iso) {
        if (err) *err = "open ISO failed: " + ierr;
        return false;
    }
    std::vector<uint8_t> sfo_data;
    if (!iso->read_file("PS3_GAME/PARAM.SFO", &sfo_data, 1 * 1024 * 1024)) {
        if (err) *err = "no PS3_GAME/PARAM.SFO in image";
        return false;
    }
    Sfo sfo;
    if (!Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, err)) return false;
    return import_sfo_based(ref, SourceType::IsoFile, src, sfo,
                            "PS3_GAME/ICON0.PNG", "PS3_GAME/PIC1.PNG", iso.get(), "", err);
}

bool GameImport::import_folder_game(const std::string& dir, std::string* err) {
    std::vector<uint8_t> sfo_data;
    std::string ps3_dir;
    if (!read_folder_sfo(dir, &sfo_data, &ps3_dir)) {
        if (err) *err = "no PARAM.SFO under " + dir;
        return false;
    }
    Sfo sfo;
    if (!Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, err)) return false;
    return import_sfo_based(dir, SourceType::Folder, nullptr, sfo,
                            "ICON0.PNG", "PIC1.PNG", nullptr, ps3_dir, err);
}

bool GameImport::import_from_parts(const std::string& src_ref, SourceType type,
                                   const std::vector<uint8_t>& sfo_data,
                                   const std::vector<uint8_t>* icon,
                                   const std::vector<uint8_t>* pic1, std::string* err) {
    Sfo sfo;
    if (!Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, err)) return false;
    std::string id = sfo.title_id();
    std::string title = sfo.title();
    if (id.empty() || title.empty()) {
        if (err) *err = "PARAM.SFO lacks TITLE_ID/TITLE";
        return false;
    }
    GameRecord* existing = find(id);
    GameRecord rec;
    if (existing) rec = *existing;
    rec.title = title;
    rec.title_id = id;
    rec.app_ver = sfo.app_ver();
    rec.category = sfo.category();
    rec.src_type = type;
    rec.src_ref = src_ref;
    rec.missing = false;
    rec.last_seen_ms = now_ms();
    if (rec.added_ms == 0) rec.added_ms = rec.last_seen_ms;

    auto write_thumb = [&](const std::vector<uint8_t>* bytes, const char* stem,
                           int max_w, bool jpeg, std::string* out_path) {
        if (!bytes || bytes->empty()) return;
        std::string suffix = jpeg ? ".jpg" : ".png";
        std::string outp = path_join(thumbs_dir_, id + "_" + stem + suffix);
        ThumbInfo ti = make_thumbnail(outp, bytes->data(), bytes->size(), max_w, jpeg);
        if (ti.ok) *out_path = ti.path;
    };
    if (!rec.icon_path.empty() && file_exists(rec.icon_path)) {
        // keep existing thumbnail
    } else {
        std::string ip;
        write_thumb(icon, "icon0", 320, false, &ip);
        if (!ip.empty()) rec.icon_path = ip;
    }
    std::string pp;
    write_thumb(pic1, "pic1", 480, true, &pp);
    if (!pp.empty()) rec.pic1_path = pp;

    if (existing) {
        *existing = rec;
    } else {
        games_.push_back(std::move(rec));
    }
    save(err);
    return true;
}

bool GameImport::set_saf_present(const std::string& uri, bool present, uint64_t size) {
    bool changed = false;
    for (auto& g : games_) {
        if (g.src_type == SourceType::SafUri && g.src_ref == uri) {
            g.missing = !present;
            if (present) {
                g.src_size = size;
                g.last_seen_ms = now_ms();
            }
            changed = true;
        }
    }
    if (changed) save(nullptr);
    return changed;
}

ScanStats GameImport::scan_folder(const std::string& path, bool recursive, std::string* err) {
    ScanStats st;
    last_scan_log_.clear();
    if (!dir_exists(path)) {
        if (err) *err = "not a folder: " + path;
        return st;
    }
    std::vector<std::string> names;
    list_dir(path, &names);
    for (const std::string& n : names) {
        std::string full = path_join(path, n);
        if (dir_exists(full)) {
            if (looks_like_folder_game(full)) {
                st.found++;
                std::string tid_before;
                {
                    std::vector<uint8_t> b;
                    std::string d;
                    if (read_folder_sfo(full, &b, &d)) {
                        Sfo s;
                        if (Sfo::parse(b.data(), b.size(), &s)) tid_before = s.title_id();
                    }
                }
                bool was_new = find(tid_before) == nullptr;
                std::string ierr;
                if (import_folder_game(full, &ierr)) {
                    if (was_new) st.imported++; else st.updated++;
                    last_scan_log_.push_back("imported folder game: " + full);
                } else {
                    st.skipped++;
                    last_scan_log_.push_back("failed folder game: " + full + " (" + ierr + ")");
                }
            } else if (recursive) {
                ScanStats sub = scan_folder(full, false, err);
                st.found += sub.found;
                st.imported += sub.imported;
                st.updated += sub.updated;
                st.skipped += sub.skipped;
                for (auto& l : sub.log) last_scan_log_.push_back(l);
            }
        } else if (to_lower(path_extension(full)) == ".iso") {
            st.found++;
            std::string tid_before;
            {
                auto fs = FileSource::open(full, nullptr);
                if (fs) {
                    auto iso = Iso9660::open(std::shared_ptr<ByteSource>(fs.get(), [](ByteSource*) {}), nullptr);
                    if (iso) {
                        std::vector<uint8_t> b;
                        if (iso->read_file("PS3_GAME/PARAM.SFO", &b)) {
                            Sfo s;
                            if (Sfo::parse(b.data(), b.size(), &s)) tid_before = s.title_id();
                        }
                    }
                }
            }
            bool was_new = find(tid_before) == nullptr;
            std::string ierr;
            auto fs = FileSource::open(full, nullptr);
            if (fs && import_iso(full, fs.get(), &ierr)) {
                if (was_new) st.imported++; else st.updated++;
                last_scan_log_.push_back("imported ISO: " + full);
            } else {
                st.skipped++;
                last_scan_log_.push_back("failed ISO: " + full + " (" + ierr + ")");
            }
        }
    }
    save(err);
    st.log = last_scan_log_;
    return st;
}

bool GameImport::refresh(GameRecord* rec, std::string* err) {
    if (!rec) return false;
    rec->missing = false;
    bool present = false;
    std::vector<uint8_t> sfo_data;
    Sfo sfo;
    if (rec->src_type == SourceType::Folder) {
        present = read_folder_sfo(rec->src_ref, &sfo_data, nullptr) &&
                  Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, nullptr);
    } else if (rec->src_type == SourceType::IsoFile) {
        auto fs = FileSource::open(rec->src_ref, nullptr);
        if (fs) {
            auto iso = Iso9660::open(std::shared_ptr<ByteSource>(fs.get(), [](ByteSource*) {}), nullptr);
            if (iso) {
                present = iso->read_file("PS3_GAME/PARAM.SFO", &sfo_data) &&
                          Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, nullptr);
            }
        }
    } else {  // SafUri — presence is checked by the platform layer via a
              // freshly opened fd; core just re-extracts thumbs if requested
        present = rec->icon_path.empty() == false || !rec->src_ref.empty();
    }
    if (!present) {
        rec->missing = true;
        return true;  // note: not an error; caller shows "file missing" state
    }
    rec->last_seen_ms = now_ms();
    // re-extract thumbnails if cache was wiped
    if (rec->icon_path.empty() || !file_exists(rec->icon_path)) {
        if (rec->src_type == SourceType::IsoFile) import_iso(rec->src_ref, FileSource::open(rec->src_ref, err ? nullptr : nullptr).get(), err);
        if (rec->src_type == SourceType::Folder) import_folder_game(rec->src_ref, err);
    }
    (void)err;
    return true;
}

int GameImport::refresh_all(std::string* err) {
    int present = 0;
    for (auto& g : games_) {
        refresh(&g, err);
        if (!g.missing) present++;
    }
    save(err);
    return present;
}

bool GameImport::remove_record(const std::string& title_id, std::string* err) {
    for (auto it = games_.begin(); it != games_.end(); ++it) {
        if (it->title_id == title_id) {
            games_.erase(it);
            return save(err);
        }
    }
    if (err) *err = "no such game: " + title_id;
    return false;
}

// ---------------- persistence ----------------
static JVal record_to_json(const GameRecord& g) {
    JVal v = JVal::make_obj();
    v.set("title", JVal::s(g.title));
    v.set("title_id", JVal::s(g.title_id));
    v.set("app_ver", JVal::s(g.app_ver));
    v.set("category", JVal::s(g.category));
    v.set("src_type", JVal::s(src_type_name(g.src_type)));
    v.set("src_ref", JVal::s(g.src_ref));
    v.set("src_size", JVal::n((double)g.src_size));
    v.set("added_ms", JVal::n((double)g.added_ms));
    v.set("last_seen_ms", JVal::n((double)g.last_seen_ms));
    v.set("missing", JVal::bl(g.missing));
    v.set("icon_path", JVal::s(g.icon_path));
    v.set("pic1_path", JVal::s(g.pic1_path));
    return v;
}

bool GameImport::save(std::string* err) const {
    JVal root = JVal::make_obj();
    root.set("format", JVal::n(1));
    root.set("anyps3_version", JVal::s(kVersion));
    JVal arr = JVal::make_arr();
    for (const auto& g : games_) arr.push(record_to_json(g));
    root.set("games", arr);
    std::string text = json_dump(root);
    if (!write_file(lib_path_, text.data(), text.size())) {
        if (err) *err = "cannot write " + lib_path_;
        return false;
    }
    return true;
}

bool GameImport::load(std::string* err) {
    games_.clear();
    std::string text;
    std::vector<uint8_t> raw;
    if (!read_file(lib_path_, &raw)) {
        if (file_exists(lib_path_)) {
            if (err) *err = "cannot read " + lib_path_;
            return false;
        }
        return true;  // fresh library
    }
    JVal root;
    if (!json_parse(std::string(raw.begin(), raw.end()), &root, err)) return false;
    const JVal& arr = root.at("games");
    if (!arr.is_arr() || !arr.arr) return true;
    for (const auto& item : *arr.arr) {
        GameRecord g;
        g.title = item.get_str("title", "");
        g.title_id = item.get_str("title_id", "");
        g.app_ver = item.get_str("app_ver", "");
        g.category = item.get_str("category", "");
        g.src_type = src_type_from(item.get_str("src_type", "folder"));
        g.src_ref = item.get_str("src_ref", "");
        g.src_size = (uint64_t)item.get_int("src_size", 0);
        g.added_ms = item.get_int("added_ms", 0);
        g.last_seen_ms = item.get_int("last_seen_ms", 0);
        g.missing = item.get_bool("missing", false);
        g.icon_path = item.get_str("icon_path", "");
        g.pic1_path = item.get_str("pic1_path", "");
        if (!g.title_id.empty()) games_.push_back(std::move(g));
    }
    return true;
}

}  // namespace anyps3
