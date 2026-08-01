package com.bassquake.avpvr

import android.app.Application

class InstallAssets : Application() {
    override fun onCreate() {
        super.onCreate()

        // Pre-create the cache directory so the Adreno EGL shader cache
        // doesn't crash with a null path on first launch of a new package name.
        cacheDir.mkdirs()

        // Game assets live in /sdcard/AvPVR/ (NOT /sdcard/Android/data/<pkg>/files/),
        // because Android 11+ scoped storage blocks libc-level open()/access() on the
        // app-private external dir even with READ_EXTERNAL_STORAGE granted. To read
        // assets from a public top-level dir we use MANAGE_EXTERNAL_STORAGE (granted
        // via `adb shell appops set <pkg> MANAGE_EXTERNAL_STORAGE allow`), which adds
        // the media_rw supplementary GID to our process so libc syscalls succeed.
        // Asset push:
        //   adb push assets/. /sdcard/AvPVR/
    }
}
