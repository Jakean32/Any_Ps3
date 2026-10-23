// Any_Ps3 JNI bridge: exposes the C++ core (core/library.h) to the Android
// shell. Deliberately small surface: heavy work in native, JSON over the
// boundary, no game data ever copied into app storage.
#include <jni.h>

#include <cstring>
#include <string>

#include "core/cache_manifest.h"
#include "core/game_import.h"
#include "core/library.h"
#include "core/minijson.h"
#include "core/recompiler.h"

using namespace anyps3;

static JavaVM* g_vm = nullptr;
static jobject g_bridge_cls = nullptr;
static jmethodID g_progress_mid = nullptr;

jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass local = env->FindClass("com/anyps3/app/NativeBridge");
    if (!local) return JNI_ERR;
    g_bridge_cls = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    g_progress_mid = env->GetStaticMethodID(
        env->GetObjectClass(g_bridge_cls) ? (jclass)g_bridge_cls : local,
        "onCompileProgress", "(Ljava/lang/String;DLjava/lang/String;)V");
    return JNI_VERSION_1_6;
}

static std::string to_str(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    env->ReleaseStringUTFChars(s, c);
    return out;
}

static jstring to_jstr(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

static Library* lib(jlong h) { return reinterpret_cast<Library*>((intptr_t)h); }

// ---------------- lifecycle ----------------
extern "C" JNIEXPORT jlong JNICALL
Java_com_anyps3_app_NativeBridge_nativeInit(JNIEnv* env, jclass, jstring metaDir,
                                            jstring cacheRoot) {
    AppConfig cfg;
    cfg.load(to_str(env, metaDir));
    std::string root = to_str(env, cacheRoot);
    if (!root.empty()) cfg.cache_root = root;
    return (jlong)(intptr_t)new Library(cfg);
}

extern "C" JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetCacheRoot(JNIEnv* env, jclass, jlong h,
                                                    jstring cacheRoot) {
    std::string root = to_str(env, cacheRoot);
    lib(h)->set_cache_root(root);
}

extern "C" JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeDestroy(JNIEnv*, jclass, jlong h) {
    delete lib(h);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
    return to_jstr(env, kVersion);
}

// ---------------- config ----------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeConfigJson(JNIEnv* env, jclass, jlong h) {
    const AppConfig& c = lib(h)->config();
    JVal v = JVal::make_obj();
    v.set("cache_root", JVal::s(c.cache_root));
    v.set("wizard_done", JVal::bl(c.wizard_done));
    v.set("legal_version_accepted", JVal::s(c.legal_version_accepted));
    for (const auto& kv : c.extra) v.set(kv.first, JVal::s(kv.second));
    return to_jstr(env, json_dump(v));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeConfigSet(JNIEnv* env, jclass, jlong h, jstring key,
                                                 jstring value) {
    AppConfig& c = const_cast<AppConfig&>(lib(h)->config());
    bool ok = c.set(to_str(env, key), to_str(env, value));
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ---------------- entries ----------------
static JVal entry_json(const LibraryEntry& e) {
    JVal o = JVal::make_obj();
    o.set("title", JVal::s(e.rec.title));
    o.set("title_id", JVal::s(e.rec.title_id));
    o.set("app_ver", JVal::s(e.rec.app_ver));
    o.set("category", JVal::s(e.rec.category));
    o.set("src_type", JVal::s(e.rec.src_type == SourceType::IsoFile
                                  ? "iso"
                                  : e.rec.src_type == SourceType::SafUri ? "saf_uri" : "folder"));
    o.set("src_ref", JVal::s(e.rec.src_ref));
    o.set("src_size", JVal::n((double)e.rec.src_size));
    o.set("missing", JVal::bl(e.rec.missing));
    o.set("icon_path", JVal::s(e.rec.icon_path));
    o.set("pic1_path", JVal::s(e.rec.pic1_path));
    std::string status = e.has_manifest ? cache_status_name(e.manifest.status) : "not_compiled";
    o.set("status", JVal::s(status));
    o.set("status_name", JVal::s(Library::status_ui_name(
                            e.has_manifest ? e.manifest.status : CacheStatus::NotCompiled)));
    o.set("note", JVal::s(e.has_manifest ? e.manifest.note : ""));
    JVal stages = JVal::make_arr();
    if (e.has_manifest) {
        for (const auto& s : e.manifest.stages) {
            JVal st = JVal::make_obj();
            st.set("name", JVal::s(s.name));
            st.set("status", JVal::s(s.status));
            st.set("note", JVal::s(s.note));
            stages.push(st);
        }
    }
    o.set("stages", stages);
    return o;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeEntriesJson(JNIEnv* env, jclass, jlong h) {
    auto es = lib(h)->entries();
    JVal arr = JVal::make_arr();
    for (const auto& e : es) arr.push(entry_json(e));
    return to_jstr(env, json_dump(arr));
}

// ---------------- import ----------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeScanFolder(JNIEnv* env, jclass, jlong h, jstring path,
                                                  jboolean recursive) {
    std::string err;
    ScanStats st = lib(h)->scan_folder(to_str(env, path), recursive == JNI_TRUE, &err);
    JVal v = JVal::make_obj();
    v.set("found", JVal::n(st.found));
    v.set("imported", JVal::n(st.imported));
    v.set("updated", JVal::n(st.updated));
    v.set("skipped", JVal::n(st.skipped));
    return to_jstr(env, json_dump(v));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportIsoPath(JNIEnv* env, jclass, jlong h, jstring path) {
    std::string err;
    return lib(h)->import_iso_path(to_str(env, path), &err) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportIsoFd(JNIEnv* env, jclass, jlong h, jstring uri,
                                                   jint fd) {
    std::string err;
    return lib(h)->import_saf_uri(to_str(env, uri), (int)fd, true, &err) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportSfoFd(JNIEnv* env, jclass, jlong h, jstring gameDirUri,
                                                   jstring sfoUri, jint sfoFd, jint iconFd,
                                                   jint pic1Fd) {
    std::string err;
    return lib(h)
        ->import_saf_folder(to_str(env, gameDirUri), to_str(env, sfoUri), (int)sfoFd, (int)iconFd,
                            (int)pic1Fd, &err)
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeUpdateSafRecord(JNIEnv* env, jclass, jlong h, jstring uri,
                                                       jint fd) {
    std::string u = to_str(env, uri);
    Library* L = lib(h);
    if (fd < 0) {
        return L->importer().set_saf_present(u, false, 0) ? JNI_TRUE : JNI_FALSE;
    }
    auto src = FdSource::attach((int)fd, true, nullptr);
    if (!src) return L->importer().set_saf_present(u, false, 0) ? JNI_TRUE : JNI_FALSE;
    uint64_t size = src->size();
    return L->importer().set_saf_present(u, true, size) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeRefresh(JNIEnv*, jclass, jlong h) {
    std::string err;
    return lib(h)->refresh(nullptr, &err) ? JNI_TRUE : JNI_FALSE;
}

// ---------------- compile ----------------
static void emit_progress(JNIEnv* env, const CompileProgress& p) {
    if (!env || !g_bridge_cls || !g_progress_mid) return;
    jstring js = env->NewStringUTF(p.stage.c_str());
    jstring jn = env->NewStringUTF(p.note.c_str());
    env->CallStaticVoidMethod((jclass)g_bridge_cls, g_progress_mid, js, (jdouble)p.fraction, jn);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(js);
    env->DeleteLocalRef(jn);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeCompile(JNIEnv* env, jclass, jlong h, jstring gameId) {
    std::string err;
    std::string id = to_str(env, gameId);
    Library* L = lib(h);
    JNIEnv* threadEnv = nullptr;
    bool attached = false;
    if (env == nullptr && g_vm && g_vm->GetEnv(reinterpret_cast<void**>(&threadEnv),
                                               JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&threadEnv, nullptr);
        attached = threadEnv != nullptr;
    }
    JNIEnv* use = threadEnv ? threadEnv : env;
    ProgressFn cb = [use](const CompileProgress& p) { emit_progress(use, p); };
    bool ok = L->compile(id, cb, &err);
    if (attached && g_vm) g_vm->DetachCurrentThread();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeRecompile(JNIEnv* env, jclass, jlong h, jstring gameId) {
    std::string err;
    JNIEnv* threadEnv = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&threadEnv), JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&threadEnv, nullptr);
        attached = true;
    }
    ProgressFn cb = [threadEnv](const CompileProgress& p) { emit_progress(threadEnv, p); };
    bool ok = lib(h)->recompile(to_str(env, gameId), cb, &err);
    if (attached && g_vm) g_vm->DetachCurrentThread();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeDeleteCache(JNIEnv* env, jclass, jlong h, jstring gameId) {
    std::string err;
    return lib(h)->delete_cache(to_str(env, gameId), &err) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anyps3_app_NativeBridge_nativeRemoveGame(JNIEnv* env, jclass, jlong h, jstring gameId,
                                                  jboolean alsoDeleteCache) {
    std::string err;
    return lib(h)
        ->remove_game(to_str(env, gameId), alsoDeleteCache == JNI_TRUE, &err)
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeCacheDirFor(JNIEnv* env, jclass, jlong h, jstring gameId) {
    return to_jstr(env, lib(h)->cache().game_dir(to_str(env, gameId)));
}

// ---------------- misc ----------------
extern "C" JNIEXPORT jlong JNICALL
Java_com_anyps3_app_NativeBridge_nativeFreeSpaceBytes(JNIEnv*, jclass, jlong h) {
    return (jlong)lib(h)->cache_free_bytes();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeUiLog(JNIEnv* env, jclass, jlong h) {
    JVal arr = JVal::make_arr();
    for (const auto& l : lib(h)->ui_log()) arr.push(JVal::s(l));
    return to_jstr(env, json_dump(arr));
}
