package com.anyps3.app;

/**
 * JNI bridge to the Any_Ps3 C++ core (core/library.h).
 * All heavy work happens in native code; this surface is deliberately small.
 * Records/statuses cross the boundary as JSON (parsed with the framework's
 * org.json — no external dependencies).
 */
public final class NativeBridge {

    private NativeBridge() {}

    // ---- lifecycle ----
    public static native long nativeInit(String metaDir, String cacheRoot);
    public static native void nativeSetCacheRoot(long h, String cacheRoot);
    public static native void nativeDestroy(long h);
    public static native String nativeVersion();

    // ---- configuration (config.json, persisted) ----
    public static native String nativeConfigJson(long h);
    public static native boolean nativeConfigSet(long h, String key, String value);

    // ---- library / import (NO-COPY: sources stay where they are) ----
    /** JSON array of {title,title_id,status,status_name,missing,icon_path,pic1_path,note,src_ref,src_type,app_ver,category} */
    public static native String nativeEntriesJson(long h);
    /** Scan a real filesystem folder (used when raw path access is available). */
    public static native String nativeScanFolder(long h, String path, boolean recursive);
    /** Import a single ISO by path. */
    public static native boolean nativeImportIsoPath(long h, String path);
    /** Import an ISO through an open file descriptor (SAF). fd is consumed. */
    public static native boolean nativeImportIsoFd(long h, String uri, int fd);
    /** SAF folder game: fds for PARAM.SFO / ICON0.PNG / PIC1.PNG (icon/pic1 may be -1). */
    public static native boolean nativeImportSfoFd(long h, String gameDirUri, String sfoUri,
                                                   int sfoFd, int iconFd, int pic1Fd);
    /** Update SAF presence/size for a record; fd optional (-1 = absent). */
    public static native boolean nativeUpdateSafRecord(long h, String uri, int fd);
    /** Full refresh: re-check sources, staleness, re-extract missing thumbs. */
    public static native boolean nativeRefresh(long h);

    // ---- compile cache ----
    public static native boolean nativeCompile(long h, String gameId);
    public static native boolean nativeRecompile(long h, String gameId);
    public static native boolean nativeDeleteCache(long h, String gameId);
    public static native boolean nativeRemoveGame(long h, String gameId, boolean alsoDeleteCache);
    public static native String nativeCacheDirFor(long h, String gameId);

    // ---- misc ----
    public static native long nativeFreeSpaceBytes(long h);
    public static native String nativeUiLog(long h);

    /** Called from native compile progress callbacks. */
    @SuppressWarnings("unused")
    public static void onCompileProgress(String stage, double fraction, String note) {
        if (listener != null) listener.onProgress(stage, fraction, note);
    }

    public interface ProgressListener {
        void onProgress(String stage, double fraction, String note);
    }

    private static volatile ProgressListener listener;

    public static void setProgressListener(ProgressListener l) {
        listener = l;
    }
}
