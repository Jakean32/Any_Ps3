#include "anyps3/loader/import.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "anyps3/fs/iso9660.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/sfo/sfo.h"

namespace anyps3 {

static bool IsDir(const std::string& path) { return platform::IsDir(path); }
static bool IsFile(const std::string& path) { return platform::IsFile(path); }
static bool ReadHostFile(const std::string& path, std::vector<u8>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(sz));
    size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return got == out->size();
}

// ---------------------------------------------------------------------------
// Metadata fallbacks (never fail an import for missing metadata)
// ---------------------------------------------------------------------------

static std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return name;
}

static std::string TitleFromSourceName(const std::string& path_or_name) {
    std::string name = BaseName(path_or_name);
    size_t dot = name.find_last_of('.');
    std::string base = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    if (base.empty()) base = "Untitled game";
    return base;
}

// Scan a buffer for a PS3 title id: four letters, optional '-' or ' ', five digits
// (e.g. "BLUS-30493", "NPEB00866" also matches without the separator).
static std::string ScanTitleId(const u8* data, size_t len) {
    for (size_t i = 0; i + 8 <= len; i++) {
        if (std::isupper(data[i]) && std::isupper(data[i + 1]) && std::isupper(data[i + 2]) &&
            std::isupper(data[i + 3])) {
            size_t j = i + 4;
            char sep = static_cast<char>(data[j]);
            if (sep == '-' || sep == ' ') j++;
            if (j + 5 <= len) {
                bool all_digit = true;
                for (int k = 0; k < 5; k++) {
                    if (!std::isdigit(data[j + static_cast<size_t>(k)])) all_digit = false;
                }
                if (all_digit) {
                    std::string id(data + i, data + i + 4);
                    if (sep == '-' || sep == ' ') id += '-';
                    id.append(data + j, data + j + 5);
                    return id;
                }
            }
        }
    }
    return "";
}

// PS3_DISC.SFB: little disc-header file at the ISO root containing the real
// title id as ASCII. Public format: magic "\0SFB" then fields; the id is
// found by pattern scan (robust against layout drift).
static std::string TitleIdFromSfb(const std::vector<u8>& sfb) {
    if (sfb.size() < 4) return "";
    if (!(sfb[0] == 0x00 && sfb[1] == 'S' && sfb[2] == 'F' && sfb[3] == 'B')) {
        AP_WARN("import: PS3_DISC.SFB has unexpected magic");
    }
    return ScanTitleId(sfb.data(), sfb.size());
}

static std::string HexFirstBytes(const std::vector<u8>& data, size_t n) {
    std::string s;
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n && i < data.size(); i++) {
        s.push_back(d[data[i] >> 4]);
        s.push_back(d[data[i] & 15]);
        if (i + 1 < n && i + 1 < data.size()) s.push_back(' ');
    }
    return s;
}

// ---------------------------------------------------------------------------
// Unified source view (folder or ISO) used by the inspect/read paths.
// ---------------------------------------------------------------------------

struct SourceView {
    bool is_folder = false;
    std::string folder_root;  // absolute game dir (content root already applied)
    std::unique_ptr<IsoReader> iso;
};

static bool OpenSourceView(const std::string& source_path, const std::string* key_text,
                           SourceView* v, std::string* err) {
    v->iso.reset();
    v->folder_root.clear();
    if (IsDir(source_path)) {
        v->is_folder = true;
        return true;
    }
    if (IsFile(source_path)) {
        v->is_folder = false;
        v->iso = std::make_unique<IsoReader>();
        if (!v->iso->Open(source_path)) {
            *err = v->iso->open_error();
            return false;  // encryption() stays available to the caller
        }
        if (key_text && !key_text->empty() && v->iso->encryption().any_encrypted) {
            // An explicitly supplied key is the user's deliberate choice: it
            // always wins over sibling/embedded keys. A failed unlock surfaces
            // honestly through the callers (import refuses, boot reports).
            if (!v->iso->SetDiscKeyText(*key_text)) {
                AP_WARN("import: the supplied disc key did not unlock the image: ",
                        v->iso->open_error().c_str());
            }
        }
        return true;
    }
    *err = "path does not exist: " + source_path;
    return false;
}

// Reads `rel` where rel is relative to the game's content root.
// NOTE for folders: `folder_root` already points INTO the content dir
// (DetectContentRoot appends /PS3_GAME), so content_root is ignored there.
static bool SourceRead(SourceView& v, const std::string& content_root, const std::string& rel,
                       std::vector<u8>* out) {
    if (v.is_folder) {
        std::string p = v.folder_root + "/" + rel;
        return ReadHostFile(p, out);
    }
    std::string rel_full = content_root.empty() ? rel : content_root + "/" + rel;
    return v.iso->ReadFile(rel_full, out);
}

static bool SourceExists(SourceView& v, const std::string& content_root, const std::string& rel) {
    std::vector<u8> tmp;
    return SourceRead(v, content_root, rel, &tmp);
}

static std::string DetectContentRoot(SourceView& v, std::string* err) {
    if (v.is_folder) {
        // folder_root is the game dir itself; content root is "".
        if (IsFile(v.folder_root + "/PS3_GAME/PARAM.SFO") ||
            IsFile(v.folder_root + "/PS3_GAME/EBOOT.BIN")) {
            // caller passed the parent; adjust
            v.folder_root += "/PS3_GAME";
            return "PS3_GAME";
        }
        return "";
    }
    if (v.iso->Find("PS3_GAME/PARAM.SFO") || v.iso->Find("PS3_GAME/EBOOT.BIN")) {
        return "PS3_GAME";
    }
    if (v.iso->Find("PARAM.SFO") || v.iso->Find("EBOOT.BIN")) return "";
    *err = "ISO does not contain PS3_GAME/PARAM.SFO, PS3_GAME/EBOOT.BIN, PARAM.SFO or EBOOT.BIN";
    return "";
}

// ---------------------------------------------------------------------------
// Inspect
// ---------------------------------------------------------------------------

// Shared metadata extraction. Returns true when the game is importable
// (which is almost always — missing metadata degrades, not fails).
static bool FillMetadata(SourceView& v, const std::string& content_root,
                         const std::string& display_name, GameInfo* out, std::string* err) {
    const bool encrypted_layer = !v.is_folder && v.iso->encryption().any_encrypted;
    const bool key_active = !v.is_folder && v.iso->HasKey();

    std::string diag;

    // ---- PARAM.SFO ----
    std::vector<u8> sfo_data;
    bool have_sfo = SourceRead(v, content_root, "PARAM.SFO", &sfo_data);
    AP_INFO("import: PS3_GAME/PARAM.SFO ", have_sfo ? "found" : "NOT found",
            have_sfo ? ", size " + std::to_string(sfo_data.size()) : "",
            have_sfo ? ", first bytes: " + HexFirstBytes(sfo_data, 16) : "");
    if (have_sfo) {
        bool magic_ok = sfo_data.size() >= 4 && sfo_data[0] == 0x00 && sfo_data[1] == 'P' &&
                        sfo_data[2] == 'S' && sfo_data[3] == 'F';
        AP_INFO("import: PARAM.SFO starts with \\0PSF: ",
                magic_ok ? "yes" : "NO (content is unreadable or wrong)");
        ParamSfo sfo;
        if (magic_ok && sfo.Parse(sfo_data)) {
            out->title = sfo.GetString("TITLE").value_or("");
            out->title_id = sfo.GetString("TITLE_ID").value_or("");
            out->category = sfo.GetString("CATEGORY").value_or("");
            out->app_ver = sfo.GetString("APP_VER").value_or("");
            out->system_ver = sfo.GetString("PS3_SYSTEM_VER").value_or("");
            out->meta_source = "PARAM.SFO";
            diag += "SFO ok";
            AP_DEBUG("import: SFO fields: TITLE='", out->title.c_str(),
                     "' TITLE_ID='", out->title_id.c_str(), "' CATEGORY='",
                     out->category.c_str(), "' APP_VER='", out->app_ver.c_str(),
                     "' PS3_SYSTEM_VER='", out->system_ver.c_str(), "'");
        } else {
            diag += "SFO unreadable";
            if (encrypted_layer && !key_active) {
                diag += " (encrypted region without key)";
            } else if (encrypted_layer && key_active) {
                diag += " (decryption produced garbage — wrong disc key?)";
            }
        }
    } else {
        diag += "no PARAM.SFO";
    }

    // ---- encrypted-image bookkeeping ----
    if (encrypted_layer) {
        AP_DEBUG("import: encryption layer present: ", v.iso->encryption().Summary().c_str(),
                 " key_active=", key_active ? "yes" : "no");
        out->encrypted = key_active ? EncState::KeyInUse : EncState::NeedsKey;
        if (v.iso->key_source() == IsoReader::KeySource::Embedded3k3y) {
            out->dkey_path = "3k3y-embedded";
        } else if (v.iso->key_source() == IsoReader::KeySource::SiblingFile) {
            out->dkey_path = v.iso->sibling_key_path();
        } else if (key_active) {
            out->dkey_path = "user-supplied";
        }
    }

    // ---- fallbacks for missing title id / title ----
    if (out->title_id.empty()) {
        // 1) PS3_DISC.SFB at the disc root (plain even on encrypted dumps)
        std::vector<u8> sfb;
        std::string sfb_rel = content_root.empty() ? "" : "";
        // The SFB lives at the ISO ROOT (not inside PS3_GAME) on disc dumps.
        if (!v.is_folder && v.iso->ReadFile("PS3_DISC.SFB", &sfb)) {
            std::string id = TitleIdFromSfb(sfb);
            AP_INFO("import: PS3_DISC.SFB found (", sfb.size(), " bytes), title id scan: ",
                    id.empty() ? "not found" : id);
            if (!id.empty()) {
                out->title_id = id;
                if (out->meta_source.empty()) out->meta_source = "PS3_DISC.SFB";
                diag += " +SFB id";
            }
        } else if (v.is_folder) {
            std::vector<std::string> roots = {v.folder_root, v.folder_root + "/.."};
            for (const auto& root : roots) {
                if (ReadHostFile(root + "/PS3_DISC.SFB", &sfb)) {
                    std::string id = TitleIdFromSfb(sfb);
                    if (!id.empty()) {
                        out->title_id = id;
                        if (out->meta_source.empty()) out->meta_source = "PS3_DISC.SFB";
                        diag += " +SFB id";
                    }
                    break;
                }
            }
        }
        // 2) filename pattern (e.g. "BLUS-30493 - Game.iso")
        if (out->title_id.empty()) {
            std::string id = ScanTitleId(
                reinterpret_cast<const u8*>(display_name.data()), display_name.size());
            if (!id.empty()) {
                out->title_id = id;
                out->meta_source = "filename";
                diag += " +filename id";
            }
        }
    }
    if (out->title.empty()) {
        out->title = TitleFromSourceName(display_name);
        if (out->meta_source.empty()) out->meta_source = "filename";
        diag += " +filename title";
    }

    // ---- honest refusal point: we must at least know WHO this game is ----
    if (out->title_id.empty()) {
        if (encrypted_layer && !key_active) {
            // Encrypted without key: import anyway with a placeholder id so the
            // user can attach a disc key in the settings. This is the state
            // "Encrypted - disc key needed".
            out->title_id = "UNKNOWN";
            diag += " +placeholder id (encrypted, no key)";
        } else if (have_sfo) {
            *err = "PARAM.SFO parsed but has no TITLE_ID and no PS3_DISC.SFB/filename "
                   "fallback matched — cannot identify this game";
            return false;
        } else {
            *err = "not a PS3 game dump: no PARAM.SFO, no PS3_DISC.SFB";
            return false;
        }
    }

    if (encrypted_layer && !key_active) {
        AP_WARN("import: image has encrypted regions but no disc key — importing with "
                "state 'Encrypted - disc key needed'. Provide a .dkey/.key file next to "
                "the ISO or select one in the game settings.");
    }

    // ---- ICON0.PNG (optional) ----
    std::vector<u8> icon;
    if (SourceRead(v, content_root, "ICON0.PNG", &icon)) {
        out->icon_png = std::move(icon);
    }

    out->diag = diag;
    return true;
}

bool InspectGame(const std::string& source_path, GameInfo* out, std::string* err,
                 const std::string& key_text) {
    *out = GameInfo{};
    out->source_path = source_path;

    if (IsDir(source_path)) {
        SourceView v;
        v.is_folder = true;
        v.folder_root = source_path;
        std::string dummy;
        std::string content_root = DetectContentRoot(v, &dummy);
        // For folder dumps the SFO check decides whether this is a PS3 game at all.
        if (!IsFile(v.folder_root + "/PARAM.SFO") && !IsFile(v.folder_root + "/EBOOT.BIN")) {
            *err = "folder does not contain PARAM.SFO or EBOOT.BIN — not a PS3 game dump";
            return false;
        }
        std::string cerr;
        if (!FillMetadata(v, content_root, source_path, out, &cerr)) {
            *err = cerr;
            return false;
        }
        out->content_root = content_root;
        return true;
    }

    if (IsFile(source_path)) {
        SourceView v;
        std::string open_err;
        bool opened = OpenSourceView(source_path,
                                     key_text.empty() ? nullptr : &key_text, &v, &open_err);
        if (!opened) {
            // With an explicit disc key, a locked image must actually unlock —
            // otherwise report the failure honestly instead of importing a
            // placeholder (the caller keeps its previous library entry).
            if (!key_text.empty() && v.iso && v.iso->encryption().any_encrypted) {
                if (v.iso->SetDiscKeyText(key_text)) {
                    opened = true;  // the key re-walked the filesystem
                } else {
                    AP_WARN("import: the supplied disc key did not unlock the image: ",
                            v.iso->open_error().c_str());
                    *err = "the supplied disc key did not unlock this image: " +
                           v.iso->open_error();
                    return false;
                }
            }
        }
        if (!opened) {
            // Open failed without a key — if an encryption layer was detected,
            // import anyway (the filesystem itself may sit in encrypted regions).
            if (v.iso && v.iso->encryption().any_encrypted && !v.iso->HasKey()) {
                AP_WARN("import: ISO9660 unreadable (encrypted regions, no disc key) — ",
                        v.iso->encryption().Summary().c_str());
                out->title = TitleFromSourceName(source_path);
                out->title_id = "UNKNOWN";
                out->encrypted = EncState::NeedsKey;
                out->meta_source = "filename";
                out->diag = "FS unreadable without key";
                *err = "";
                return true;
            }
            *err = "not a readable PS3 ISO: " + open_err;
            return false;
        }
        std::string detect_err;
        std::string content_root = DetectContentRoot(v, &detect_err);
        if (content_root.empty() && !detect_err.empty()) {
            if (v.iso->encryption().any_encrypted && !v.iso->HasKey()) {
                // FS structures may be in encrypted regions; import flagged.
                out->title = TitleFromSourceName(source_path);
                out->title_id = "UNKNOWN";
                out->encrypted = EncState::NeedsKey;
                out->meta_source = "filename";
                out->diag = "FS entry points unreadable without key";
                return true;
            }
            if (!key_text.empty()) {
                *err = "the supplied disc key did not unlock this image "
                       "(no PS3_GAME/PARAM.SFO visible through the decrypted view)";
                return false;
            }
            *err = detect_err;
            return false;
        }
        std::string cerr;
        if (!FillMetadata(v, content_root, source_path, out, &cerr)) {
            if (!key_text.empty() && v.iso->encryption().any_encrypted) {
                // Explicit key present but the metadata stayed unreadable:
                // name the likely cause instead of a generic identification error.
                *err = "the supplied disc key did not unlock this image "
                       "(metadata stayed unreadable): " + cerr;
            } else {
                *err = cerr;
            }
            return false;
        }
        out->content_root = content_root;
        return true;
    }

    *err = "path does not exist: " + source_path;
    return false;
}

bool InspectGameFd(int fd, const std::string& display_name, GameInfo* out,
                   std::string* err, const std::string& key_text) {
    *out = GameInfo{};
    out->source_path = display_name;  // informational

    SourceView v;
    v.is_folder = false;
    v.iso = std::make_unique<IsoReader>();
    bool opened = v.iso->OpenFd(fd, display_name);
    if (!key_text.empty() && v.iso->encryption().any_encrypted) {
        // Explicit key (per-game settings "attach key and re-read"): it must
        // actually unlock the image, or the inspect fails honestly.
        if (v.iso->SetDiscKeyText(key_text)) {
            opened = true;  // key attached + filesystem re-walked
        } else {
            AP_WARN("import: the supplied disc key did not unlock the image: ",
                    v.iso->open_error().c_str());
            *err = "the supplied disc key did not unlock this image: " +
                   v.iso->open_error();
            return false;
        }
    }
    if (!opened) {
        if (v.iso->encryption().any_encrypted && !v.iso->HasKey()) {
            out->title = TitleFromSourceName(display_name);
            out->title_id = "UNKNOWN";
            out->encrypted = EncState::NeedsKey;
            out->meta_source = "filename";
            out->diag = "FS unreadable without key";
            return true;
        }
        *err = "cannot open ISO from file descriptor: " + v.iso->open_error();
        return false;
    }
    std::string detect_err;
    std::string content_root = DetectContentRoot(v, &detect_err);
    if (content_root.empty() && !detect_err.empty()) {
        if (v.iso->encryption().any_encrypted && !v.iso->HasKey()) {
            out->title = TitleFromSourceName(display_name);
            out->title_id = "UNKNOWN";
            out->encrypted = EncState::NeedsKey;
            out->meta_source = "filename";
            out->diag = "FS entry points unreadable without key";
            return true;
        }
        if (!key_text.empty()) {
            *err = "the supplied disc key did not unlock this image "
                   "(no PS3_GAME/PARAM.SFO visible through the decrypted view)";
            return false;
        }
        *err = detect_err;
        return false;
    }
    std::string cerr;
    if (!FillMetadata(v, content_root, display_name, out, &cerr)) {
        if (!key_text.empty() && v.iso->encryption().any_encrypted) {
            *err = "the supplied disc key did not unlock this image "
                   "(metadata stayed unreadable): " + cerr;
        } else {
            *err = cerr;
        }
        return false;
    }
    out->content_root = content_root;
    return true;
}

// ---------------------------------------------------------------------------
// Game-file reads (boot pipeline)
// ---------------------------------------------------------------------------

bool ReadGameFile(const std::string& source_path, const std::string& rel_path,
                  std::vector<u8>* out, std::string* err,
                  const std::string* key_text) {
    std::string path = rel_path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (IsDir(source_path)) {
        if (ReadHostFile(source_path + "/" + path, out)) return true;
        *err = "file not found in game folder: " + path;
        return false;
    }
    if (IsFile(source_path)) {
        SourceView v;
        std::string open_err;
        if (!OpenSourceView(source_path, key_text, &v, &open_err)) {
            *err = "cannot open ISO image (" + open_err + "): " + source_path;
            return false;
        }
        if (v.iso->ReadFile(path, out)) return true;
        *err = "file not found in ISO: " + path;
        return false;
    }
    *err = "source is neither file nor folder: " + source_path;
    return false;
}

bool ReadGameFileFd(int fd, const std::string& rel_path, std::vector<u8>* out,
                    std::string* err, const std::string* key_text) {
    std::string path = rel_path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    IsoReader iso;
    if (!iso.OpenFd(fd, "SAF fd")) {
        *err = "cannot open ISO from file descriptor: " + iso.open_error();
        return false;
    }
    if (key_text && !key_text->empty() && iso.NeedsKey()) {
        if (!iso.SetDiscKeyText(*key_text)) {
            AP_WARN("boot: disc key did not decrypt the image");
        }
    }
    if (iso.ReadFile(path, out)) return true;
    *err = "file not found in ISO: " + path;
    return false;
}

// ---------------------------------------------------------------------------
// Disc-key helpers (user-supplied keys only — nothing is ever bundled)
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Hex32OfKey(const uint8_t key[16]) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; i++) {
        out += hex[key[i] >> 4];
        out += hex[key[i] & 0xF];
    }
    return out;
}

std::string ReadDiscKeyFileAsText(const std::string& path, std::string* err) {
    if (err) err->clear();
    std::vector<u8> data;
    if (!ReadHostFile(path, &data) || data.empty()) {
        if (err) *err = "cannot read disc-key file: " + path;
        return "";
    }
    if (data.size() > 4096) {
        if (err) *err = "disc-key file too large (expect 16 raw bytes or 32 hex chars): " + path;
        return "";
    }
    uint8_t key[16];
    std::string perr;
    // 1) the file content as-is (raw 16-byte keys, clean hex text)
    std::string raw(data.begin(), data.end());
    if (any_ps3::ParseDiscKey(raw, key, &perr)) {
        AP_DEBUG("disc key file parsed (raw form): ", path.c_str());
        return Hex32OfKey(key);
    }
    // 2) exactly 16 bytes = a raw binary key (even when it contains bytes the
    //    text parser treats as whitespace/control characters)
    if (data.size() == 16) {
        std::memcpy(key, data.data(), 16);
        AP_DEBUG("disc key file parsed (16 raw bytes): ", path.c_str());
        return Hex32OfKey(key);
    }
    // 3) hex/colon text with surrounding whitespace or a trailing newline
    std::string trimmed = Trim(raw);
    if (any_ps3::ParseDiscKey(trimmed, key, &perr)) {
        AP_DEBUG("disc key file parsed (text form): ", path.c_str());
        return Hex32OfKey(key);
    }
    if (err) *err = "not a usable disc key (" + perr + "): " + path;
    return "";
}

std::string EffectiveDiscKeyText(const GameSettings& s, std::string* err) {
    if (err) err->clear();
    // A typed 32-hex key wins over the key-file path.
    if (!s.dkey_hex.empty()) {
        uint8_t key[16];
        std::string perr;
        if (!any_ps3::ParseDiscKey(s.dkey_hex, key, &perr)) {
            if (err) *err = "stored disc key is malformed: " + perr;
            return "";
        }
        return Hex32OfKey(key);
    }
    if (!s.dkey_path.empty() && s.dkey_path != "3k3y-embedded" &&
        s.dkey_path != "user-supplied") {
        return ReadDiscKeyFileAsText(s.dkey_path, err);
    }
    if (err) *err = "no disc key attached";
    return "";
}

// ---------------------------------------------------------------------------
// Library index: <base>/games/<TITLE_ID>/{game.txt, icon.png}
// game.txt is a tiny key=value manifest (no external deps, easy to edit by hand).
// ---------------------------------------------------------------------------

static void WriteManifest(const std::string& path, const GameInfo& g) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "title=%s\n", g.title.c_str());
    std::fprintf(f, "title_id=%s\n", g.title_id.c_str());
    std::fprintf(f, "category=%s\n", g.category.c_str());
    std::fprintf(f, "app_ver=%s\n", g.app_ver.c_str());
    std::fprintf(f, "system_ver=%s\n", g.system_ver.c_str());
    std::fprintf(f, "source_path=%s\n", g.source_path.c_str());
    std::fprintf(f, "source_uri=%s\n", g.source_uri.c_str());
    std::fprintf(f, "content_root=%s\n", g.content_root.c_str());
    std::fprintf(f, "encrypted=%d\n", static_cast<int>(g.encrypted));
    std::fprintf(f, "dkey_path=%s\n", g.dkey_path.c_str());
    std::fprintf(f, "meta_source=%s\n", g.meta_source.c_str());
    std::fprintf(f, "diag=%s\n", g.diag.c_str());
    std::fclose(f);
}

static bool ReadManifest(const std::string& path, GameInfo* g) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::map<std::string, std::string> kv;
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = Trim(line);
        if (l.empty()) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        kv[Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    std::fclose(f);
    g->title = kv["title"];
    g->title_id = kv["title_id"];
    g->category = kv["category"];
    g->app_ver = kv["app_ver"];
    g->system_ver = kv["system_ver"];
    g->source_path = kv["source_path"];
    g->source_uri = kv["source_uri"];
    g->content_root = kv["content_root"];
    g->encrypted = static_cast<EncState>(std::atoi(kv["encrypted"].c_str()));
    g->dkey_path = kv["dkey_path"];
    g->meta_source = kv["meta_source"];
    g->diag = kv["diag"];
    return !g->title_id.empty();
}

GameLibrary::GameLibrary(std::string base_dir) : dir_(std::move(base_dir) + "/games") {
    platform::MakeDirs(dir_);
}

bool GameLibrary::Import(const std::string& source_path, GameInfo* out, std::string* err) {
    if (!InspectGame(source_path, out, err)) return false;
    return ImportInfo(*out, err);
}

bool GameLibrary::ImportInfo(const GameInfo& info, std::string* err) {
    std::string id = info.title_id;
    // Unique directory: UNKNOWN entries get numbered suffixes.
    std::string gdir = dir_ + "/" + id;
    if (id == "UNKNOWN") {
        int n = 2;
        GameInfo existing;
        while (platform::IsFile(gdir + "/game.txt")) {
            if (ReadManifest(gdir + "/game.txt", &existing) &&
                existing.source_path == info.source_path) {
                break;  // same source: reuse the slot
            }
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%d", n++);
            gdir = dir_ + "/UNKNOWN" + suffix;
        }
        id = BaseName(gdir);
    }
    platform::MakeDirs(gdir);
    GameInfo stored = info;
    stored.title_id = id;
    WriteManifest(gdir + "/game.txt", stored);
    if (!stored.icon_png.empty()) {
        FILE* f = std::fopen((gdir + "/icon.png").c_str(), "wb");
        if (f) {
            std::fwrite(stored.icon_png.data(), 1, stored.icon_png.size(), f);
            std::fclose(f);
        }
    }
    return true;
}

bool GameLibrary::List(std::vector<LibraryEntry>* out) const {
    out->clear();
    std::vector<std::string> names;
    if (!platform::ListDir(dir_, &names)) return false;
    for (const auto& name : names) {
        std::string manifest = dir_ + "/" + name + "/game.txt";
        GameInfo g;
        if (!ReadManifest(manifest, &g)) continue;
        LibraryEntry entry;
        entry.info = g;
        entry.dir = dir_ + "/" + name;
        std::vector<u8> icon;
        if (ReadHostFile(entry.dir + "/icon.png", &icon)) entry.info.icon_png = icon;
        out->push_back(std::move(entry));
    }
    return true;
}

bool GameLibrary::Remove(const std::string& title_id, std::string* err) {
    std::string gdir = dir_ + "/" + title_id;
    std::string manifest = gdir + "/game.txt";
    if (!IsFile(manifest)) {
        *err = "game not in library: " + title_id;
        return false;
    }
    std::remove((gdir + "/game.txt").c_str());
    std::remove((gdir + "/icon.png").c_str());
    std::remove((gdir + "/aot").c_str());
    // Per-game settings and the user's stored disc key belong to the game and
    // must not survive its removal (previously these files blocked the rmdir
    // and silently kept the key on disk).
    std::remove((gdir + "/settings.ini").c_str());
    std::remove((gdir + "/dkey.bin").c_str());
    platform::RemoveDir(gdir);
    return true;
}

bool ReplaceLibraryEntry(const AppConfig& app, GameLibrary* lib, const GameInfo& info,
                         const std::string& old_id, std::string* err) {
    if (err) err->clear();
    if (!lib->ImportInfo(info, err)) return false;
    if (!old_id.empty() && old_id != info.title_id) {
        // The id changed (e.g. UNKNOWN -> the real TITLE_ID once the disc key
        // unlocked PARAM.SFO). Move the per-game settings and the stored key
        // file to the new slot, then drop the old library entry.
        GameSettings s = GameSettings::Load(app, old_id);
        s.Save(app, info.title_id);
        std::string old_dir = app.config_dir + "/games/" + old_id;
        std::string new_dir = app.config_dir + "/games/" + info.title_id;
        platform::MakeDirs(new_dir);
        std::rename((old_dir + "/dkey.bin").c_str(), (new_dir + "/dkey.bin").c_str());
        std::string rerr;
        lib->Remove(old_id, &rerr);  // best-effort; the fresh entry is in place
        AP_INFO("library: entry re-keyed, id ", old_id.c_str(), " -> ",
                info.title_id.c_str(), " (settings + key moved)");
    }
    return true;
}

}  // namespace anyps3
