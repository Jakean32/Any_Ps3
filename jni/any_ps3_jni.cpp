// Any_Ps3 — JNI bridge (Android).
// The core is called EXCLUSIVELY through file descriptors obtained from the
// Storage Access Framework (ParcelFileDescriptor). No ISO is ever copied or
// fully loaded into RAM. Descriptors are owned by the JVM; this code never
// closes them (except the temp copy target, which it also received open).
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <cstring>

#include "any_ps3/types.h"
#include "any_ps3/bytes.h"
#include "any_ps3/json.h"
#include "any_ps3/source.h"
#include "any_ps3/manifest.h"
#include "any_ps3/pipeline.h"

#define LOG_TAG "anyps3-native"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

using namespace any_ps3;

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string r = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return r;
}

void jerr(JsonValue& j, const std::string& msg) {
    j.set("ok", JsonValue::make_bool(false));
    j.set("error", JsonValue::make_str(msg));
}

// progress callback: void onProgress(int stageIndex, float fraction, String line)
class JniProgress {
public:
    JniProgress(JNIEnv* env, jobject obj) : env_(env), obj_(obj) {
        cls_ = env_->GetObjectClass(obj_);
        mid_ = env_->GetMethodID(cls_, "onProgress", "(IFLjava/lang/String;)V");
    }
    void emit(const PipelineProgress& p) {
        if (!mid_) return;
        jstring line = env_->NewStringUTF(p.line.c_str());
        env_->CallVoidMethod(obj_, mid_, (jint)p.stage_index, (jfloat)p.fraction, line);
        if (env_->ExceptionCheck()) env_->ExceptionClear();
    }
    operator bool() const { return mid_ != nullptr; }

private:
    JNIEnv* env_;
    jobject obj_;
    jclass cls_ = nullptr;
    jmethodID mid_ = nullptr;
};

std::string game_meta_json(GameSource& src, const std::string& kind) {
    JsonValue j = JsonValue::make_obj();
    std::string err;
    GameMeta meta = src.meta(&err);
    if (!meta.valid) {
        jerr(j, err);
        return j.dump();
    }
    j.set("ok", JsonValue::make_bool(true));
    j.set("title", JsonValue::make_str(meta.title));
    j.set("title_id", JsonValue::make_str(meta.title_id));
    j.set("category", JsonValue::make_str(meta.category));
    j.set("app_ver", JsonValue::make_str(meta.app_ver));
    j.set("system_ver", JsonValue::make_str(meta.system_ver));
    j.set("source_kind", JsonValue::make_str(kind));
    j.set("source_size", JsonValue::make_int((long long)src.source_size()));
    return j.dump();
}

JsonValue manifest_to_json(const Manifest& m) {
    JsonValue j = JsonValue::make_obj();
    j.set("game_id", JsonValue::make_str(m.game_id));
    j.set("title", JsonValue::make_str(m.title));
    j.set("source_kind", JsonValue::make_str(m.source_kind));
    j.set("source_hash", JsonValue::make_str(m.source_hash));
    j.set("source_size", JsonValue::make_int((long long)m.source_size));
    j.set("engine_version", JsonValue::make_str(m.engine_version));
    j.set("updated_utc", JsonValue::make_str(m.updated_utc));
    j.set("status", JsonValue::make_str(m.status));
    JsonValue arr = JsonValue::make_arr();
    for (const auto& s : m.stages) {
        JsonValue o = JsonValue::make_obj();
        o.set("id", JsonValue::make_str(s.id));
        o.set("state", JsonValue::make_str(s.state));
        o.set("detail", JsonValue::make_str(s.detail));
        arr.arr.push_back(std::move(o));
    }
    j.set("stages", std::move(arr));
    return j;
}

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_engineVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(any_ps3::kEngineVersion);
}

// Inspect a PS3 ISO through an open fd (SAF). Returns JSON.
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_inspectIsoFd(JNIEnv* env, jclass, jint fd) {
    JsonValue j;
    try {
        std::string err;
        auto src = IsoFileSource::open(std::make_unique<FdSource>(fd), err);
        if (!src) {
            jerr(j, err);
            return env->NewStringUTF(j.dump().c_str());
        }
        std::string out = game_meta_json(*src, "iso");
        return env->NewStringUTF(out.c_str());
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Parse a PARAM.SFO file directly (folder-import mode on Android).
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_parseSfoFd(JNIEnv* env, jclass, jint fd) {
    JsonValue j;
    try {
        FdSource src(fd);
        std::vector<uint8_t> data;
        data.resize((size_t)src.size());
        if (src.size() && !src.read_exact(0, data.data(), data.size())) {
            jerr(j, "read failed");
            return env->NewStringUTF(j.dump().c_str());
        }
        SfoData d = parse_sfo(data.data(), data.size());
        j.set("ok", JsonValue::make_bool(true));
        j.set("title", JsonValue::make_str(d.title()));
        j.set("title_id", JsonValue::make_str(d.title_id()));
        j.set("category", JsonValue::make_str(d.category()));
        j.set("app_ver", JsonValue::make_str(d.app_ver()));
        j.set("system_ver", JsonValue::make_str(d.system_ver()));
        j.set("source_kind", JsonValue::make_str("folder"));
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Stream PS3_GAME/<entry> from the ISO to an already-open output fd
// (thumbnail cache). Random access, chunked, never whole-image-in-RAM.
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_copyIsoEntryToFd(JNIEnv* env, jclass,
                                                   jint isoFd, jstring entry, jint outFd) {
    JsonValue j;
    try {
        std::string name = jstr(env, entry);
        std::string err;
        auto src = IsoFileSource::open(std::make_unique<FdSource>(isoFd), err);
        if (!src) {
            jerr(j, err);
            return env->NewStringUTF(j.dump().c_str());
        }
        if (!src->copy_entry_to_fd(name.c_str(), outFd)) {
            jerr(j, src->error());
            return env->NewStringUTF(j.dump().c_str());
        }
        j.set("ok", JsonValue::make_bool(true));
        j.set("bytes", JsonValue::make_int((long long)0)); // size known via outFd stat
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Whole-image signature (streamed 1 MiB chunks) for outdated detection.
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_computeIsoHash(JNIEnv* env, jclass,
                                                 jint isoFd, jobject progress) {
    JsonValue j;
    try {
        JniProgress prog(env, progress);
        auto src = std::make_unique<FdSource>(isoFd);
        uint64_t sz = src->size();
        // reuse IsoFileSource hashing path without requiring ISO9660 validity?
        // The signature is over the raw image, so hash directly:
        Sha256 h;
        std::vector<uint8_t> buf(1024 * 1024);
        uint64_t off = 0;
        while (off < sz) {
            uint64_t want = std::min<uint64_t>(buf.size(), sz - off);
            size_t got = src->read_at(off, buf.data(), (size_t)want);
            if (got == 0) break;
            h.update(buf.data(), got);
            off += got;
            if (prog) prog.emit({0, (float)((double)off / (double)sz),
                                 "[refresh] hashing source... " + std::to_string((int)(100.0 * off / sz)) + "%"});
        }
        j.set("ok", JsonValue::make_bool(true));
        j.set("hash", JsonValue::make_str(h.final_hex()));
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Run the honest AOT pipeline over an ISO fd; write the manifest to the
// provided open fd (SAF-created file in the user's cache folder).
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_compileIsoFd(JNIEnv* env, jclass,
                                               jint isoFd, jint manifestFd,
                                               jstring gameIdHint, jobject progress) {
    JsonValue j;
    try {
        std::string err;
        auto src = IsoFileSource::open(std::make_unique<FdSource>(isoFd), err);
        if (!src) {
            jerr(j, err);
            return env->NewStringUTF(j.dump().c_str());
        }
        std::string hint = jstr(env, gameIdHint);
        JniProgress prog(env, progress);
        PipelineResult res = pipeline_compile(
            *src, hint, "iso",
            [&](const PipelineProgress& p) { if (prog) prog.emit(p); });
        std::string werr;
        if (!manifest_write_fd(manifestFd, res.manifest, werr)) {
            jerr(j, "manifest write failed: " + werr);
            return env->NewStringUTF(j.dump().c_str());
        }
        j.set("ok", JsonValue::make_bool(true));
        j.set("completed", JsonValue::make_bool(res.completed));
        j.set("stopped_at", JsonValue::make_str(res.stopped_at));
        j.set("reason", JsonValue::make_str(res.reason));
        j.set("manifest", manifest_to_json(res.manifest));
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Read a manifest that lives in the user's cache folder (SAF fd).
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_readManifestFd(JNIEnv* env, jclass, jint fd) {
    JsonValue j;
    try {
        Manifest m;
        std::string err;
        if (!manifest_read_fd(fd, m, err)) {
            jerr(j, err);
            return env->NewStringUTF(j.dump().c_str());
        }
        j.set("ok", JsonValue::make_bool(true));
        JsonValue mj = manifest_to_json(m);
        mj.set("created_utc", JsonValue::make_str(m.created_utc));
        j.set("manifest", std::move(mj));
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

// Check whether the stored manifest is outdated vs the current ISO.
JNIEXPORT jstring JNICALL
Java_com_anyps3_core_NativeBridge_checkOutdatedIsoFd(JNIEnv* env, jclass,
                                                     jint isoFd, jint manifestFd,
                                                     jobject progress) {
    JsonValue j;
    try {
        Manifest m;
        std::string err;
        if (!manifest_read_fd(manifestFd, m, err)) {
            jerr(j, err);
            return env->NewStringUTF(j.dump().c_str());
        }
        JniProgress prog(env, progress);
        std::string err2;
        auto src = IsoFileSource::open(std::make_unique<FdSource>(isoFd), err2);
        if (!src) {
            jerr(j, err2);
            return env->NewStringUTF(j.dump().c_str());
        }
        int rc = pipeline_check_outdated(*src, m, [&](float f) {
            if (prog) prog.emit({0, f, "[check] hashing source... " + std::to_string((int)(f * 100)) + "%"});
        });
        j.set("ok", JsonValue::make_bool(true));
        j.set("result", JsonValue::make_int(rc)); // 0 current, 1 outdated, -1 error
    } catch (const std::exception& e) {
        jerr(j, e.what());
    } catch (...) {
        jerr(j, "unknown native error");
    }
    return env->NewStringUTF(j.dump().c_str());
}

} // extern "C"
