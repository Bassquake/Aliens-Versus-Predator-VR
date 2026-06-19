package com.bassquake.avpvr

import android.app.Application
import android.util.Log
import java.io.File
import java.util.Locale

class InstallAssets : Application() {
    override fun onCreate() {
        super.onCreate()

        // Pre-create the cache directory so the Adreno EGL shader cache
        // doesn't crash with a null path on first launch of a new package name.
        cacheDir.mkdirs()

        // Ensure the external files directory exists so the engine can write to it.
        // Game assets must be placed here manually via ADB:
        //   adb push assets/ /sdcard/Android/data/com.bassquake.avpvr/files/
        val dataDir = getExternalFilesDir(null)
        dataDir?.mkdirs()

        // Ensure the external files are in lowercase format.
        lowercaseDir(dataDir!!)
    }

    private fun lowercaseDir(dir: File) {
        val list = dir.listFiles() ?: return

        for (file in list) {
            if (file.isDirectory) {
                lowercaseDir(file)
            } else {
                lowercaseFile(file)
            }
        }
        lowercaseFile(dir)
    }

    private fun lowercaseFile(file: File) {
        val lower = file.name.lowercase(Locale.getDefault())
        if (file.name == lower) return

        val parent = file.parentFile ?: return
        val temp = File(parent, "$lower.tmp")
        if (file.renameTo(temp) && temp.renameTo(File(parent, lower))) {
            Log.i("AVPVR", "Renamed $file -> $lower")
        } else {
            Log.e("AVPVR", "Failed to rename $file -> $lower")
        }
    }
}
