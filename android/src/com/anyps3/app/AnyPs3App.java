package com.anyps3.app;

import android.app.Application;
import android.content.Context;

/**
 * Any_Ps3 application: owns the native Library handle and app configuration.
 * One instance lives for the whole process; activities talk to it.
 */
public class AnyPs3App extends Application {

    static {
        // libanyps3_core.so contains the full C++ core (see core/)
        System.loadLibrary("anyps3_core");
    }

    public static final String LEGAL_VERSION = "0.3.0";

    public static final String LEGAL_TEXT =
        "Any_Ps3 is an original, clean-room engineered launcher.\n\n"
        + "• It contains NO Sony firmware, decryption keys, or game content.\n"
        + "• It does not bypass copy protection.\n"
        + "• You may only import and compile games that you legally own.\n"
        + "• The project is not affiliated with Sony.";

    private long libraryHandle;
    private String metaDir;

    public static AnyPs3App get(Context c) {
        return (AnyPs3App) c.getApplicationContext();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        metaDir = getExternalFilesDir(null) + "/meta";
        libraryHandle = 0;
    }

    /** Creates/loads the library with the given cache root. Thread-safe. */
    public synchronized void ensureLibrary(String cacheRoot) {
        if (libraryHandle != 0) {
            NativeBridge.nativeSetCacheRoot(libraryHandle, cacheRoot);
            return;
        }
        libraryHandle = NativeBridge.nativeInit(metaDir, cacheRoot);
        if (libraryHandle == 0)
            throw new IllegalStateException("native library init failed");
    }

    public synchronized long library() {
        if (libraryHandle == 0)
            ensureLibrary(defaultCacheRoot());
        return libraryHandle;
    }

    /** Default cache location: app's own external dir (always writable). */
    public String defaultCacheRoot() {
        return getExternalFilesDir(null) + "/cache";
    }

    public String metaDir() {
        return metaDir;
    }
}
