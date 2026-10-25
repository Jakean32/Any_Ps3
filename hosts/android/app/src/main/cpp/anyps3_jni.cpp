// Any_Ps3 Android JNI bridge — connects the Kotlin/Compose UI to the Any_Ps3 core.
// Java class: com.anyps3.app.NativeBridge
//
// Import contract (no-copy): ISOs are NEVER copied into app storage. The UI
// takes a persistable URI, opens it through SAF (ParcelFileDescriptor) and
// hands the raw fd to the core, which reads with random access and closes
// nothing (the caller owns the fd). Only tiny metadata (icon thumbnail,
// manifest) is stored. Encrypted images: the user's own .dkey/.key file is
// copied (16..64 bytes) into the app-private games/<id>/ directory when picked.
#include <jni.h>
#include <android/log.h>

#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "anyps3/config.h"
#include "anyps3/demo/hello.h"
#include "anyps3/loader/import.h"
#include "anyps3/loader/elf.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/runtime/emulator.h"

#include "any_ps3/disc.h"  // ParseDiscKey (validation of user-supplied keys)

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnyPs3", __VA_ARGS__)

namespace {

struct AndroidRuntime {
    anyps3::AppConfig app{"."};
    anyps3::Emulator emu{app};
    std::unique_ptr<anyps3::GameLibrary> library;
    std::thread emu_thread;
    JavaVM* vm = nullptr;
    jobject callback_ref = nullptr;
    jmethodID cb_log = nullptr;
    jmethodID cb_progress = nullptr;
    jmethodID cb_state = nullptr;
};

AndroidRuntime g_rt;

// Marshal a log line up to Kotlin (com.anyps3.app.NativeBridge.RuntimeCallbacks).
void PostToKotlin(const std::string& line) {
    JNIEnv* env = nullptr;
    if (g_rt.vm == nullptr || g_rt.callback_ref == nullptr) return;
    int attached = 0;
    if (g_rt.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        g_rt.vm->AttachCurrentThread(&env, nullptr);
        attached = 1;
    }
    if (env && g_rt.cb_log) {
        jstring s = env->NewStringUTF(line.c_str());
        env->CallVoidMethod(g_rt.callback_ref, g_rt.cb_log, s);
        env->DeleteLocalRef(s);
    }
    if (attached) g_rt.vm->DetachCurrentThread();
}

void PostProgress(int pct, const std::string& note) {
    JNIEnv* env = nullptr;
    if (g_rt.vm == nullptr || g_rt.callback_ref == nullptr || g_rt.cb_progress == nullptr)
        return;
    int attached = 0;
    if (g_rt.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        g_rt.vm->AttachCurrentThread(&env, nullptr);
        attached = 1;
    }
    if (env) {
        jstring s = env->NewStringUTF(note.c_str());
        env->CallVoidMethod(g_rt.callback_ref, g_rt.cb_progress, pct, s);
        env->DeleteLocalRef(s);
    }
    if (attached) g_rt.vm->DetachCurrentThread();
}

std::string JStringToStd(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    env->ReleaseStringUTFChars(s, p);
    return out;
}

// Look one game up in the library; returns false + err when absent.
bool FindGame(const std::string& id, anyps3::GameInfo* out, std::string* err) {
    std::vector<anyps3::LibraryEntry> games;
    g_rt.library->List(&games);
    for (const auto& e : games) {
        if (e.info.title_id == id) {
            *out = e.info;
            return true;
        }
    }
    if (err) *err = "game not found: " + id;
    return false;
}

// Store (or replace) a game whose metadata came in through a SAF fd.
// Keeps per-game settings + stored disc key alive across re-imports; moves
// them to the new slot when the id changes (UNKNOWN -> real TITLE_ID).
std::string ImportInfoReplacing(const anyps3::GameInfo& info, const std::string& old_id) {
    std::string err;
    if (!anyps3::ReplaceLibraryEntry(g_rt.app, &*g_rt.library, info, old_id, &err))
        return "ERR " + err;
    return "OK " + info.title_id + "|" + info.title;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_rt.vm = vm;
    anyps3::SetLogSink([](anyps3::LogLevel, const std::string& msg) { PostToKotlin(msg); });
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeInit(JNIEnv* env, jobject, jstring config_dir) {
    std::string dir = JStringToStd(env, config_dir);
    g_rt.app.config_dir = dir;
    g_rt.library = std::make_unique<anyps3::GameLibrary>(dir);
    g_rt.app.global.LoadFromFile(dir + "/anyps3.ini");
    anyps3::SetLogLevel(g_rt.app.LogLevel());
}

JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeFirstRunAccepted(JNIEnv*, jobject) {
    return g_rt.app.FirstRunAccepted() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeAcceptFirstRun(JNIEnv*, jobject) {
    g_rt.app.AcceptFirstRun();
    g_rt.app.global.SaveToFile(g_rt.app.config_dir + "/anyps3.ini");
}

// No-copy ISO import: the fd belongs to a SAF ParcelFileDescriptor held open
// by Kotlin for the duration of this call. Nothing is copied; only metadata
// (title/id/icon/manifest) is stored.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportGameFd(JNIEnv* env, jobject, jint fd,
                                                    jstring display_name, jstring uri) {
    std::string name = JStringToStd(env, display_name);
    std::string uri_s = JStringToStd(env, uri);
    anyps3::GameInfo info;
    std::string err;
    if (!anyps3::InspectGameFd(fd, name, &info, &err)) {
        return env->NewStringUTF(("ERR " + err).c_str());
    }
    info.source_uri = uri_s;
    std::string res = ImportInfoReplacing(info, /*old_id=*/"");
    if (res.compare(0, 3, "OK ") != 0) return env->NewStringUTF(res.c_str());
    std::string out = res;
    if (info.encrypted == anyps3::EncState::NeedsKey) {
        out = "ENCRYPTED " + out.substr(3);
        if (!info.dkey_path.empty()) out += "|key:" + info.dkey_path;
    }
    return env->NewStringUTF(out.c_str());
}

// Path-based import: used by the SAF folder flow (which copies only
// PARAM.SFO / ICON0.PNG / EBOOT.BIN into app storage — a few MB, never the
// game itself) and by the built-in demo.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportGame(JNIEnv* env, jobject, jstring path) {
    std::string p = JStringToStd(env, path);
    anyps3::GameInfo info;
    std::string err;
    if (!g_rt.library->Import(p, &info, &err)) {
        return env->NewStringUTF(("ERR " + err).c_str());
    }
    return env->NewStringUTF(("OK " + info.title_id + "|" + info.title + "|" +
                              info.category)
                                 .c_str());
}

// Re-import metadata from a re-selected file (URI grant lost / file moved).
// If the game already has a disc key attached (settings), it is applied now:
// the re-read happens through the decrypted view and a NeedsKey entry turns
// into a fully identified, "Ready to compile" one.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeUpdateGameSource(JNIEnv* env, jobject, jstring id,
                                                        jint fd, jstring display_name,
                                                        jstring uri) {
    std::string old_id = JStringToStd(env, id);
    std::string name = JStringToStd(env, display_name);
    std::string uri_s = JStringToStd(env, uri);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, old_id);
    std::string kerr;
    std::string key_text = anyps3::EffectiveDiscKeyText(s, &kerr);
    if (!key_text.empty())
        LOGI("re-read with the attached disc key (id %s)", old_id.c_str());
    anyps3::GameInfo info;
    std::string err;
    if (!anyps3::InspectGameFd(fd, name, &info, &err, key_text)) {
        return env->NewStringUTF(("ERR " + err).c_str());
    }
    info.source_uri = uri_s;
    std::string res = ImportInfoReplacing(info, old_id);
    // Report the unlock explicitly so the UI can show "Ready to compile".
    if (res.compare(0, 3, "OK ") == 0 && info.encrypted == anyps3::EncState::KeyInUse &&
        old_id != info.title_id) {
        res += "|unlocked";
    }
    return env->NewStringUTF(res.c_str());
}

// Store the user's disc key (their own file, at most a few hundred bytes) in
// the app-private games/<id>/ directory and point the settings at it. The
// content is validated through the core key parser — a garbage file is
// rejected honestly instead of silently breaking reads later.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetGameKeyFd(JNIEnv* env, jobject, jstring id, jint fd) {
    std::string gid = JStringToStd(env, id);
    std::string dir = g_rt.app.config_dir + "/games/" + gid;
    anyps3::platform::MakeDirs(dir);
    std::string path = dir + "/dkey.bin";
    char buf[4096] = {0};
    size_t n = 0;
    FILE* in = fdopen(dup(fd), "rb");
    if (!in) return env->NewStringUTF("ERR cannot read key file descriptor");
    n = std::fread(buf, 1, sizeof(buf), in);
    std::fclose(in);
    if (n < 16) return env->NewStringUTF("ERR key file too small (expect 16 bytes or 32 hex chars)");
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return env->NewStringUTF("ERR cannot store key");
    std::fwrite(buf, 1, n, out);
    std::fclose(out);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, gid);
    s.dkey_path = path;
    s.dkey_hex.clear();  // a picked file replaces a typed key
    s.Save(g_rt.app, gid);
    // Validate through the same reader the re-read will use: honest ERR now
    // beats a confusing failure later.
    std::string kerr;
    if (anyps3::EffectiveDiscKeyText(s, &kerr).empty()) {
        std::remove(path.c_str());
        s.dkey_path.clear();
        s.Save(g_rt.app, gid);
        return env->NewStringUTF(("ERR key file not usable: " + kerr).c_str());
    }
    LOGI("disc key stored for %s", gid.c_str());
    return env->NewStringUTF("OK");
}

// Typed disc key (32 hex chars) — same storage + validation as the file
// picker path, minus the file.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetGameKeyText(JNIEnv* env, jobject, jstring id,
                                                     jstring text) {
    std::string gid = JStringToStd(env, id);
    std::string key = JStringToStd(env, text);
    uint8_t raw[16];
    std::string perr;
    if (!any_ps3::ParseDiscKey(key, raw, &perr)) {
        return env->NewStringUTF(("ERR not a valid disc key (" + perr + ")").c_str());
    }
    std::string dir = g_rt.app.config_dir + "/games/" + gid;
    anyps3::platform::MakeDirs(dir);
    std::string path = dir + "/dkey.bin";
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return env->NewStringUTF("ERR cannot store key");
    std::fwrite(raw, 1, 16, out);  // store the raw 16 bytes
    std::fclose(out);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, gid);
    s.dkey_path = path;
    s.dkey_hex = key;
    s.Save(g_rt.app, gid);
    LOGI("disc key (typed) stored for %s", gid.c_str());
    return env->NewStringUTF("OK");
}

// ---- decrypted executable (user's own ELF/fSELF for encrypted retail EBOOTs) ----
// Retail EBOOT.BIN is encrypted; Any_Ps3 ships no keys. The user can supply
// their OWN decrypted executable (from their own legal dump, produced with
// their own tools). SAF fds cannot be re-opened across restarts, so the file
// is copied into app-private storage and referenced from the game settings.

JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetDecryptedEbootFd(JNIEnv* env, jobject, jstring id,
                                                          jint fd) {
    std::string gid = JStringToStd(env, id);
    std::string dir = g_rt.app.config_dir + "/games/" + gid;
    anyps3::platform::MakeDirs(dir);
    std::string path = dir + "/eboot.dec.elf";
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return env->NewStringUTF("ERR cannot store the executable");
    char buf[65536];
    size_t n;
    FILE* in = fdopen(dup(fd), "rb");
    if (!in) { std::fclose(out); std::remove(path.c_str()); return env->NewStringUTF("ERR cannot read the file descriptor"); }
    size_t total = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        std::fwrite(buf, 1, n, out);
        total += n;
    }
    std::fclose(in);
    std::fclose(out);
    if (total < 64) {
        std::remove(path.c_str());
        return env->NewStringUTF("ERR file too small to be an executable");
    }
    // Validate honestly through the same loader Play will use.
    FILE* chk = std::fopen(path.c_str(), "rb");
    if (!chk) return env->NewStringUTF("ERR stored file cannot be re-opened");
    std::vector<unsigned char> bytes;
    char b2[65536];
    while ((n = std::fread(b2, 1, sizeof(b2), chk)) > 0) bytes.insert(bytes.end(), b2, b2 + n);
    std::fclose(chk);
    anyps3::ElfImage img;
    anyps3::SelfInfo si;
    std::string lerr;
    if (!anyps3::LoadExecutable(bytes.data(), bytes.size(), gid + " (decrypted EBOOT)", &img, &si, &lerr)) {
        std::remove(path.c_str());
        return env->NewStringUTF(("ERR not a loadable PS3 executable: " + lerr).c_str());
    }
    if (si.status == anyps3::SelfStatus::EncryptedSelf) {
        std::remove(path.c_str());
        return env->NewStringUTF("ERR this file is itself an encrypted SELF — Any_Ps3 needs the DECRYPTED executable");
    }
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, gid);
    s.eboot_path = path;
    s.Save(g_rt.app, gid);
    LOGI("decrypted executable stored for %s (%zu bytes, segments %zu)", gid.c_str(), total, img.segments.size());
    return env->NewStringUTF("OK");
}

JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeHasDecryptedEboot(JNIEnv* env, jobject, jstring id) {
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, JStringToStd(env, id));
    return s.eboot_path.empty() ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeDecryptedEbootName(JNIEnv* env, jobject, jstring id) {
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, JStringToStd(env, id));
    if (s.eboot_path.empty()) return env->NewStringUTF("");
    size_t slash = s.eboot_path.find_last_of('/');
    std::string name = slash == std::string::npos ? s.eboot_path : s.eboot_path.substr(slash + 1);
    return env->NewStringUTF(name.c_str());
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeClearDecryptedEboot(JNIEnv* env, jobject, jstring id) {
    std::string gid = JStringToStd(env, id);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, gid);
    if (!s.eboot_path.empty()) std::remove(s.eboot_path.c_str());
    s.eboot_path.clear();
    s.Save(g_rt.app, gid);
    LOGI("decrypted executable cleared for %s", gid.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeHasDiscKey(JNIEnv* env, jobject, jstring id) {
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, JStringToStd(env, id));
    if (!s.dkey_hex.empty()) return JNI_TRUE;
    if (s.dkey_path.empty()) return JNI_FALSE;
    FILE* f = std::fopen(s.dkey_path.c_str(), "rb");
    if (!f) return JNI_FALSE;
    std::fclose(f);
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_com_anyps3_app_NativeBridge_nativeEncState(JNIEnv* env, jobject, jstring id) {
    anyps3::GameInfo info;
    std::string err;
    if (!FindGame(JStringToStd(env, id), &info, &err)) return -1;
    return static_cast<jint>(info.encrypted);
}

JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeGetGameSourceUri(JNIEnv* env, jobject, jstring id) {
    anyps3::GameInfo info;
    std::string err;
    if (!FindGame(JStringToStd(env, id), &info, &err)) return env->NewStringUTF("");
    return env->NewStringUTF(info.source_uri.c_str());
}

// Built-in hello-world demo: MainActivity copies the bundled asset
// (assets/hello.elf — GNU-assembled PPC64 test code, no Sony content) into
// app storage and passes the real path here.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeInstallDemo(JNIEnv* env, jobject, jstring elf_path) {
    std::string p = JStringToStd(env, elf_path);
    anyps3::GameInfo info;
    std::string err;
    if (!anyps3::demo::InstallHelloDemo(g_rt.app.config_dir, p, &info, &err)) {
        return env->NewStringUTF(("ERR " + err).c_str());
    }
    return env->NewStringUTF(("OK " + info.title_id + "|" + info.title).c_str());
}

// Honest status string for the settings page (never pretends more than true).
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeStatus(JNIEnv* env, jobject) {
    std::string s = std::string("Any_Ps3 ") + anyps3::kEngineVersion + " — app shell.\n";
    s += "CPU backend: PPU interpreter (AOT backends target other hosts; this "
         "arm64 build uses the interpreter).\n";
    s += "Graphics: not implemented yet.\n";
    s += "Encrypted ISOs: decrypted on the fly with YOUR disc key (.dkey/.key); "
         "no keys shipped.\n";
    s += "Encrypted retail EBOOTs: refused (would need user keys).\n";
    return env->NewStringUTF(s.c_str());
}

// Entries separated by ';': "id|title|app_ver|icon_path|enc_state|has_key"
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeListGames(JNIEnv* env, jobject) {
    std::vector<anyps3::LibraryEntry> games;
    g_rt.library->List(&games);
    std::string out;
    for (const auto& g : games) {
        anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, g.info.title_id);
        bool has_key = !s.dkey_path.empty();
        out += g.info.title_id + "|" + g.info.title + "|" + g.info.app_ver + "|" +
               g.dir + "/icon.png|" +
               std::to_string(static_cast<int>(g.info.encrypted)) + "|" +
               (has_key ? "1" : "0") + ";";
    }
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeRemoveGame(JNIEnv* env, jobject, jstring id) {
    std::string err;
    return g_rt.library->Remove(JStringToStd(env, id), &err) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetCallbacks(JNIEnv* env, jobject, jobject callbacks) {
    jclass cls = env->GetObjectClass(callbacks);
    g_rt.cb_log = env->GetMethodID(cls, "onLog", "(Ljava/lang/String;)V");
    g_rt.cb_progress = env->GetMethodID(cls, "onProgress", "(ILjava/lang/String;)V");
    g_rt.cb_state = env->GetMethodID(cls, "onState", "(I)V");
    g_rt.callback_ref = env->NewGlobalRef(callbacks);
}

JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeBootFd(JNIEnv* env, jobject, jstring title_id,
                                             jint source_fd, jobject callbacks) {
    std::string id = JStringToStd(env, title_id);
    Java_com_anyps3_app_NativeBridge_nativeSetCallbacks(env, nullptr, callbacks);
    anyps3::GameSettings settings = anyps3::GameSettings::Load(g_rt.app, id);
    anyps3::EmulatorCallbacks cb;
    cb.log = [](const std::string& l) { PostToKotlin(l); };
    cb.progress = [](int pct, const std::string& n) { PostProgress(pct, n); };
    anyps3::BootResult r = g_rt.emu.Boot(id, settings, cb, source_fd);
    for (const auto& n : r.notes) PostToKotlin(n);
    if (!r.ok) {
        PostToKotlin("BOOT FAILED: " + r.error);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

// Path-based boot (demo + folder imports whose source is app storage).
JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeBoot(JNIEnv* env, jobject, jstring title_id,
                                             jobject callbacks) {
    std::string id = JStringToStd(env, title_id);
    Java_com_anyps3_app_NativeBridge_nativeSetCallbacks(env, nullptr, callbacks);
    anyps3::GameSettings settings = anyps3::GameSettings::Load(g_rt.app, id);
    anyps3::EmulatorCallbacks cb;
    cb.log = [](const std::string& l) { PostToKotlin(l); };
    cb.progress = [](int pct, const std::string& n) { PostProgress(pct, n); };
    anyps3::BootResult r = g_rt.emu.Boot(id, settings, cb, -1);
    for (const auto& n : r.notes) PostToKotlin(n);
    if (!r.ok) {
        PostToKotlin("BOOT FAILED: " + r.error);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

// Runs the emulation on the calling thread; call from a Kotlin coroutine/thread.
JNIEXPORT jint JNICALL
Java_com_anyps3_app_NativeBridge_nativeRun(JNIEnv*, jobject) {
    return g_rt.emu.Run();
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeStop(JNIEnv*, jobject) {
    g_rt.emu.Stop();
}

// Global log level (real setting): 0 info+, 1 warnings+, 2 everything incl.
// debug, 3 errors only.
JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetLogLevel(JNIEnv* env, jobject, jint level) {
    g_rt.app.SetLogLevel(level);
    anyps3::SetLogLevel(level);
    g_rt.app.global.SaveToFile(g_rt.app.config_dir + "/anyps3.ini");
}

JNIEXPORT jint JNICALL
Java_com_anyps3_app_NativeBridge_nativeGetLogLevel(JNIEnv*, jobject) {
    return g_rt.app.LogLevel();
}

// Per-game settings: only REAL features (disc key presence + pad mapping).
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeGetGameSettings(JNIEnv* env, jobject, jstring id) {
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, JStringToStd(env, id));
    std::string out;
    for (const auto& [k, v] : s.controller) out += k + "=" + std::to_string(v) + ";";
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetGameSettings(JNIEnv* env, jobject, jstring id, jstring kv) {
    std::string game = JStringToStd(env, id);
    std::string data = JStringToStd(env, kv);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, game);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find(';', pos);
        if (end == std::string::npos) end = data.size();
        std::string item = data.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string k = item.substr(0, eq);
        int v = std::atoi(item.substr(eq + 1).c_str());
        if (k.compare(0, 4, "pad_") == 0) s.controller[k.substr(4)] = v;
    }
    s.Save(g_rt.app, game);
}

}  // extern "C"
