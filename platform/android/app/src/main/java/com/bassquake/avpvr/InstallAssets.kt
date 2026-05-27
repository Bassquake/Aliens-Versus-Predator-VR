package com.bassquake.avpvr

import android.app.Application

class InstallAssets : Application() {
    override fun onCreate() {
        super.onCreate()

        // Pre-create the cache directory so the Adreno EGL shader cache
        // doesn't crash with a null path on first launch of a new package name.
        cacheDir.mkdirs()

        // Ensure the external files directory exists so the engine can write to it.
        // Game assets must be placed here manually via ADB:
        //   adb push assets/ /sdcard/Android/data/com.bassquake.avpvr/files/
        getExternalFilesDir(null)?.mkdirs()
    }
}
