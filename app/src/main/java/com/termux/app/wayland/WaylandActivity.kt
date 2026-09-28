package com.termux.app.wayland

import android.app.Activity
import android.os.Bundle
import android.view.ViewGroup
import android.widget.FrameLayout
import java.io.File

class WaylandActivity : Activity() {

    private lateinit var surfaceView: WaylandSurfaceView

        override fun onCreate(savedInstanceState: Bundle?) {
            super.onCreate(savedInstanceState)

            // 1. Socket lives inside Termux's standard /tmp directory!
            val runtimeDir = File("/data/data/com.termux/files/usr/tmp").apply {
                if (!exists()) mkdirs()
            }

            // 2. Start the native Wayland server
            WaylandBridge.initServer(runtimeDir.absolutePath)

            // 3. Fullscreen SurfaceView
            surfaceView = WaylandSurfaceView(this)
            setContentView(surfaceView, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ))
        }
}
