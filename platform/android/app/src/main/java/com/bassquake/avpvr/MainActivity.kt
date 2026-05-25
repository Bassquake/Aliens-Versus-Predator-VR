package com.bassquake.avpvr

import androidx.appcompat.app.AppCompatActivity
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.widget.TextView
import com.bassquake.avpvr.databinding.ActivityMainBinding
import org.libsdl.app.SDLActivity

import android.util.Log

class MainActivity : SDLActivity() {

    override fun getLibraries(): Array<String> {
        return arrayOf(
            "openxr_loader",
            "SDL3",
            "avpvr"
        )
    }

    override fun getMainSharedObject(): String {
        return "libavpvr.so"
    }

    override fun getMainFunction(): String {
        return "SDL_main"
    }

    // The Meta Quest OS automatically binds OVRMetricsToolClient using the Activity context.
    // Redirect all bindService calls to applicationContext so connections are tied to the
    // process lifetime rather than the Activity — prevents the ServiceConnectionLeaked warning.
    override fun bindService(service: Intent, conn: ServiceConnection, flags: Int): Boolean {
        return applicationContext.bindService(service, conn, flags)
    }

    companion object {
        // Used to load the 'avp' library on application startup.
        init {
            try {
                System.loadLibrary("openxr_loader")
                System.loadLibrary("SDL3")
                System.loadLibrary("avpvr")
                android.util.Log.i("SDL_CHECK", "SUCCESS: SDL3 library loaded!")
            } catch (e: UnsatisfiedLinkError) {
                android.util.Log.e("SDL_CHECK", "FAILURE: Could not load native library: ${e.message}")
            }
        }
    }
}
