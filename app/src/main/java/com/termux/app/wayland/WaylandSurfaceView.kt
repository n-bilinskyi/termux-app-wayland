package com.termux.app.wayland

import android.content.Context
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

class WaylandSurfaceView(context: Context) : SurfaceView(context), SurfaceHolder.Callback {

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun surfaceCreated(holder: SurfaceHolder) {
        WaylandBridge.setSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        WaylandBridge.setSurface(null)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        WaylandBridge.resize(width, height)
    }

    fun showKeyboard() {
        post {
            isFocusable = true
            isFocusableInTouchMode = true
            requestFocus()

            // Re-assert Wayland keyboard focus
            WaylandBridge.focusKeyboard()

            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            imm?.restartInput(this)
            imm?.showSoftInput(this, InputMethodManager.SHOW_FORCED)
        }
    }

    fun hideKeyboard() {
        post {
            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            imm?.hideSoftInputFromWindow(windowToken, 0)
        }
    }

    fun toggleKeyboard() {
        post {
            val isVisible = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                rootWindowInsets?.isVisible(android.view.WindowInsets.Type.ime()) == true
            } else {
                false
            }

            if (isVisible) {
                hideKeyboard()
            } else {
                showKeyboard()
            }
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val action = event.actionMasked
        val index = event.actionIndex
        val pointerId = event.getPointerId(index)

        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                // Focus the view for hardware keyboards/inputs without opening Gboard
                requestFocus()
                WaylandBridge.sendTouch(pointerId, event.getX(index), event.getY(index), 0)
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    WaylandBridge.sendTouch(event.getPointerId(i), event.getX(i), event.getY(i), 1)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                // Pure touch release - no keyboard popping up!
                WaylandBridge.sendTouch(pointerId, 0f, 0f, 2)
            }
        }
        return true
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        var scanCode = event.scanCode
        if (scanCode == 0) {
            scanCode = androidKeyCodeToEvdev(event.keyCode)
        }

        if (scanCode > 0) {
            val down = (event.action == KeyEvent.ACTION_DOWN)
            WaylandBridge.sendKey(scanCode, down)
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT or EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE

        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (text != null) {
                    for (i in 0 until text.length) {
                        sendCharToLinux(text[i])
                    }
                }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                return dispatchKeyEvent(event)
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                for (i in 0 until beforeLength) {
                    WaylandBridge.sendKey(14, true)
                    WaylandBridge.sendKey(14, false)
                }
                return true
            }
        }
    }

    // Handles shifted symbols and raw ASCII control codes (Ctrl+A .. Ctrl+Z)
    private fun sendCharToLinux(c: Char) {
        // 1. Direct ASCII Control codes (Ctrl+A = 1, Ctrl+C = 3, Ctrl+D = 4, Ctrl+Z = 26)
        if (c.code in 1..26) {
            val letterScanCode = EVDEV_LETTERS[c.code - 1]
            WaylandBridge.sendKey(29, true)            // KEY_LEFTCTRL DOWN
            WaylandBridge.sendKey(letterScanCode, true)
            WaylandBridge.sendKey(letterScanCode, false)
            WaylandBridge.sendKey(29, false)           // KEY_LEFTCTRL UP
            return
        }

        // 2. Shifted symbols (~, $, !, @, #, etc.)
        val shifted = SHIFTED_SYMBOLS[c]
        if (shifted != null) {
            WaylandBridge.sendKey(42, true)      // KEY_LEFTSHIFT DOWN
            WaylandBridge.sendKey(shifted, true)
            WaylandBridge.sendKey(shifted, false)
            WaylandBridge.sendKey(42, false)     // KEY_LEFTSHIFT UP
            return
        }

        // 3. Uppercase Letters
        if (c in 'A'..'Z') {
            val code = EVDEV_LETTERS[c - 'A']
            WaylandBridge.sendKey(42, true)
            WaylandBridge.sendKey(code, true)
            WaylandBridge.sendKey(code, false)
            WaylandBridge.sendKey(42, false)
            return
        }

        // 4. Standard unshifted characters
        val scanCode = charToEvdev(c)
        if (scanCode > 0) {
            WaylandBridge.sendKey(scanCode, true)
            WaylandBridge.sendKey(scanCode, false)
        }
    }

    companion object {
        val EVDEV_LETTERS = intArrayOf(
            30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
            49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
        ) // A-Z in Linux evdev

        // Symbols that require Shift
        val SHIFTED_SYMBOLS = mapOf(
            '~' to 41,  // Shift + KEY_GRAVE (TILDE!)
            '$' to 5,   // Shift + KEY_4 (DOLLAR!)
            '!' to 2,   // Shift + KEY_1
            '@' to 3,   // Shift + KEY_2
            '#' to 4,   // Shift + KEY_3
            '%' to 6,   // Shift + KEY_5
            '^' to 7,   // Shift + KEY_6
            '&' to 8,   // Shift + KEY_7
            '*' to 9,   // Shift + KEY_8
            '(' to 10,  // Shift + KEY_9
            ')' to 11,  // Shift + KEY_0
            '_' to 12,  // Shift + KEY_MINUS
            '+' to 13,  // Shift + KEY_EQUAL
            '{' to 26,  // Shift + KEY_LEFTBRACE
            '}' to 27,  // Shift + KEY_RIGHTBRACE
            '|' to 43,  // Shift + KEY_BACKSLASH (PIPE!)
            ':' to 39,  // Shift + KEY_SEMICOLON
            '"' to 40,  // Shift + KEY_APOSTROPHE
            '<' to 51,  // Shift + KEY_COMMA
            '>' to 52,  // Shift + KEY_DOT
            '?' to 53   // Shift + KEY_SLASH
        )

        fun charToEvdev(c: Char): Int = when (c) {
            in 'a'..'z' -> EVDEV_LETTERS[c - 'a']
            in '1'..'9' -> 2 + (c - '1')
            '0' -> 11
            ' ' -> 57
            '\n', '\r' -> 28
            '`' -> 41
            '-' -> 12
            '=' -> 13
            '[' -> 26
            ']' -> 27
            '\\' -> 43
            ';' -> 39
            '\'' -> 40
            ',' -> 51
            '.' -> 52
            '/' -> 53
            else -> 0
        }

        fun androidKeyCodeToEvdev(keyCode: Int): Int = when (keyCode) {
            // A-Z (CRITICAL: Fixes Ctrl+C, Ctrl+D, Ctrl+Z, etc.!)
            in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> EVDEV_LETTERS[keyCode - KeyEvent.KEYCODE_A]

            // 0-9
            KeyEvent.KEYCODE_0 -> 11
            in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9 -> 2 + (keyCode - KeyEvent.KEYCODE_1)

            // Modifiers
            KeyEvent.KEYCODE_CTRL_LEFT       -> 29  // KEY_LEFTCTRL
            KeyEvent.KEYCODE_CTRL_RIGHT      -> 97  // KEY_RIGHTCTRL
            KeyEvent.KEYCODE_ALT_LEFT        -> 56  // KEY_LEFTALT
            KeyEvent.KEYCODE_ALT_RIGHT       -> 100 // KEY_RIGHTALT
            KeyEvent.KEYCODE_SHIFT_LEFT      -> 42  // KEY_LEFTSHIFT
            KeyEvent.KEYCODE_SHIFT_RIGHT     -> 54  // KEY_RIGHTSHIFT

            // Control & Punctuation
            KeyEvent.KEYCODE_DEL             -> 14  // KEY_BACKSPACE
            KeyEvent.KEYCODE_ENTER           -> 28  // KEY_ENTER
            KeyEvent.KEYCODE_SPACE           -> 57  // KEY_SPACE
            KeyEvent.KEYCODE_TAB             -> 15  // KEY_TAB
            KeyEvent.KEYCODE_ESCAPE          -> 1   // KEY_ESC
            KeyEvent.KEYCODE_GRAVE           -> 41  // KEY_GRAVE (`)
            KeyEvent.KEYCODE_MINUS           -> 12  // KEY_MINUS (-)
            KeyEvent.KEYCODE_EQUALS          -> 13  // KEY_EQUAL (=)
            KeyEvent.KEYCODE_LEFT_BRACKET    -> 26  // KEY_LEFTBRACE ([)
            KeyEvent.KEYCODE_RIGHT_BRACKET   -> 27  // KEY_RIGHTBRACE (])
            KeyEvent.KEYCODE_BACKSLASH       -> 43  // KEY_BACKSLASH (\)
            KeyEvent.KEYCODE_SEMICOLON       -> 39  // KEY_SEMICOLON (;)
            KeyEvent.KEYCODE_APOSTROPHE      -> 40  // KEY_APOSTROPHE (')
            KeyEvent.KEYCODE_COMMA           -> 51  // KEY_COMMA (,)
            KeyEvent.KEYCODE_PERIOD          -> 52  // KEY_DOT (.)
            KeyEvent.KEYCODE_SLASH           -> 53  // KEY_SLASH (/)
            KeyEvent.KEYCODE_DPAD_UP         -> 103 // KEY_UP
            KeyEvent.KEYCODE_DPAD_DOWN       -> 108 // KEY_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT       -> 105 // KEY_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT      -> 106 // KEY_RIGHT
            else -> 0
        }
    }
}
