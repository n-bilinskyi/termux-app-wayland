package com.termux.app.wayland

import android.view.Surface

object WaylandBridge {
    init {
        System.loadLibrary("wayland_bridge")
    }

    external fun initServer(runtimeDir: String)
    external fun setSurface(surface: Surface?)
    external fun resize(width: Int, height: Int)
    external fun refresh()
    external fun setScale(scale: Float) // <--- ADD THIS
    external fun focusKeyboard()
    external fun sendKey(scanCode: Int, down: Boolean)
    external fun sendTouch(id: Int, x: Float, y: Float, action: Int)
}
