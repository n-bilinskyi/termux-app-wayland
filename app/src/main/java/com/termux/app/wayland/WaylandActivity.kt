package com.termux.app.wayland

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.view.ViewGroup
import android.widget.FrameLayout


class WaylandActivity : Activity() {

    private lateinit var surfaceView: WaylandSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        surfaceView = WaylandSurfaceView(this)
        setContentView(surfaceView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        surfaceView.toggleKeyboard()
    }

    private val exitReceiver: BroadcastReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            finish()
        }
    }

    override fun onResume() {
        super.onResume()
        val filter = IntentFilter("com.termux.app.wayland.ACTION_EXIT")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(exitReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(exitReceiver, filter)
        }

    }

    override fun onPause() {
        super.onPause()
        unregisterReceiver(exitReceiver)
    }

}
