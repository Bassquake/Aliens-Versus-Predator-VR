package com.bassquake.avpvr

import android.app.Application
import android.content.Context
// Import the function directly from the utils package
import com.bassquake.avpvr.utils.copyAllAssetsToInternalStorage

class InstallAssets : Application() {
    override fun onCreate() {
        super.onCreate()

        // Pre-create the cache directory so the Adreno EGL shader cache
        // doesn't crash with a null path on first launch of a new package name.
        cacheDir.mkdirs()

        val prefs = getSharedPreferences("app_setup", Context.MODE_PRIVATE)

        if (prefs.getBoolean("is_first_run", true)) {
            // This runs synchronously and blocks the app launch until finished
            copyAllAssetsToInternalStorage(applicationContext)

            prefs.edit().putBoolean("is_first_run", false).apply()
        }
    }
}