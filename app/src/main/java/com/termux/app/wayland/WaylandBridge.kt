package com.termux.app.wayland

import android.view.Surface
import androidx.annotation.Keep

object WaylandBridge {
    init {
        System.loadLibrary("wayland_bridge")
    }

    @Keep
    interface ActivityListener {
        @Keep
        fun onClientLaunched()
        @Keep
        fun onClientExited()
    }

    @JvmStatic
    external fun registerActivityListener(listener: ActivityListener?)

    @JvmStatic
    external fun initServer(runtimeDir: String)
    external fun setSurface(surface: Surface?)
    external fun resize(width: Int, height: Int)
    external fun refresh()
    external fun setScale(scale: Float)
    external fun focusKeyboard()
    external fun sendKey(scanCode: Int, down: Boolean)
    external fun sendTouch(id: Int, x: Float, y: Float, action: Int)

}
