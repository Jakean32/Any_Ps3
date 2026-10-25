package com.anyps3.app.core

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri

/** Persistent app settings (first-run wizard, cache folder, thresholds). */
class Settings(context: Context) {
    private val sp: SharedPreferences =
        context.getSharedPreferences("anyps3_settings", Context.MODE_PRIVATE)

    var wizardDone: Boolean
        get() = sp.getBoolean("wizard_done", false)
        set(v) = sp.edit().putBoolean("wizard_done", v).apply()

    var cacheTreeUri: String
        get() = sp.getString("cache_tree_uri", "") ?: ""
        set(v) = sp.edit().putString("cache_tree_uri", v).apply()

    var legalAccepted: Boolean
        get() = sp.getBoolean("legal_accepted", false)
        set(v) = sp.edit().putBoolean("legal_accepted", v).apply()

    /** Low-space warning threshold in GB (compile caches can get large). */
    var lowSpaceThresholdGb: Int
        get() = sp.getInt("low_space_threshold_gb", 8)
        set(v) = sp.edit().putInt("low_space_threshold_gb", v).apply()

    /**
     * Documented approach for the Android executable-code constraint:
     * compiled artifacts live in the shared folder as data files and are
     * copied into the private dir at launch to become loadable/executable.
     */
    var keepExecutableCopyInPrivateDir: Boolean
        get() = sp.getBoolean("exec_copy_private", true)
        set(v) = sp.edit().putBoolean("exec_copy_private", v).apply()

    fun cacheTreeUriOrNull(): Uri? =
        cacheTreeUri.takeIf { it.isNotEmpty() }?.let(Uri::parse)
}
