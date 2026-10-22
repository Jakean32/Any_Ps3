// Any_Ps3 Android JNI bridge — connects the Kotlin/Compose UI to the Any_Ps3 core.
// Java class: com.anyps3.app.NativeBridge
#include <jni.h>
#include <android/log.h>

#include <string>
#include <thread>
#include <vector>

#include "anyps3/config.h"
#include "anyps3/loader/import.h"
#include "anyps3/log.h"
#include "anyps3/runtime/emulator.h"

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

// Import: copy the SAF-picked content into the app data dir first from Kotlin
// (see MainActivity), then hand a real filesystem path to the core importer.
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeImportGame(JNIEnv* env, jobject, jstring path) {
    std::string p = JStringToStd(env, path);
    anyps3::GameInfo info;
    std::string err;
    if (!g_rt.library->Import(p, &info, &err)) {
        std::string out = "ERR " + err;
        return env->NewStringUTF(out.c_str());
    }
    // "OK id|title|category"
    std::string out = "OK " + info.title_id + "|" + info.title + "|" + info.category;
    return env->NewStringUTF(out.c_str());
}

// Returns entries separated by ';': "id|title|app_ver|icon_path"
JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeListGames(JNIEnv* env, jobject) {
    std::vector<anyps3::LibraryEntry> games;
    g_rt.library->List(&games);
    std::string out;
    for (const auto& g : games) {
        out += g.info.title_id + "|" + g.info.title + "|" + g.info.app_ver + "|" +
               g.dir + "/icon.png;";
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
Java_com_anyps3_app_NativeBridge_nativeBoot(JNIEnv* env, jobject, jstring title_id,
                                             jobject callbacks) {
    std::string id = JStringToStd(env, title_id);
    Java_com_anyps3_app_NativeBridge_nativeSetCallbacks(env, nullptr, callbacks);
    anyps3::GameSettings settings = anyps3::GameSettings::Load(g_rt.app, id);
    anyps3::EmulatorCallbacks cb;
    cb.log = [](const std::string& l) { PostToKotlin(l); };
    cb.progress = [](int pct, const std::string& n) { PostProgress(pct, n); };
    anyps3::BootResult r = g_rt.emu.Boot(id, settings, cb);
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

JNIEXPORT jstring JNICALL
Java_com_anyps3_app_NativeBridge_nativeGetGameSettings(JNIEnv* env, jobject, jstring id) {
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, JStringToStd(env, id));
    std::string out = std::to_string(s.resolution_scale) + "|" +
                      std::to_string(s.frame_limit) + "|" +
                      std::to_string(s.shader_cache_enabled) + "|" +
                      std::to_string(s.ppu_backend);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL
Java_com_anyps3_app_NativeBridge_nativeSetGameSettings(JNIEnv* env, jobject, jstring id, jstring kv) {
    std::string game = JStringToStd(env, id);
    std::string data = JStringToStd(env, kv);
    anyps3::GameSettings s = anyps3::GameSettings::Load(g_rt.app, game);
    // parse "key=value;key=value"
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
        if (k == "resolution_scale") s.resolution_scale = v;
        else if (k == "frame_limit") s.frame_limit = v;
        else if (k == "shader_cache") s.shader_cache_enabled = v;
        else if (k == "ppu_backend") s.ppu_backend = v;
    }
    s.Save(g_rt.app, game);
}

}  // extern "C"
