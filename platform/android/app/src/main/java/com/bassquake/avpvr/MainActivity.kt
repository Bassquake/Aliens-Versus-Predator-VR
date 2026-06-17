package com.bassquake.avpvr

import android.content.Intent
import android.content.ServiceConnection
import org.libsdl.app.SDLActivity

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

    // The matching unbind MUST go through the same context the bind used, otherwise the
    // Activity's LoadedApk has no record of the connection and unbindService() throws
    // "IllegalArgumentException: Service not registered". On exit the runtime's
    // OVRMetricsToolClient.shutdown() unbinds on this Activity context, which previously
    // crashed the process ("Aliens Versus Predator has stopped"). Redirect to
    // applicationContext to match bindService above, and swallow the benign case where the
    // connection was never registered / already unbound.
    override fun unbindService(conn: ServiceConnection) {
        try {
            applicationContext.unbindService(conn)
        } catch (e: IllegalArgumentException) {
            android.util.Log.w("AvP", "unbindService ignored (not registered): ${e.message}")
        }
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
