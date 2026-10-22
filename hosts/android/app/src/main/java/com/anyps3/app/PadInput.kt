// Any_Ps3 — gamepad input capture for Android.
// Records physical gamepad events; the core input module (Phase 6) consumes the
// event ring via JNI. Touch-overlay events feed the same queue.
package com.anyps3.app

import android.view.KeyEvent

object PadInput {
    data class PadEvent(val keyCode: Int, val action: Int, val source: Int, val tMs: Long)

    // Small ring buffer (single producer: the UI thread; consumer: emu thread).
    private val lock = Any()
    private val ring = ArrayDeque<PadEvent>()

    fun onAndroidKey(keyCode: Int, event: KeyEvent) {
        push(PadEvent(keyCode, event.action, event.source, event.eventTime))
    }

    fun onTouchOverlayButton(guestButton: Int, pressed: Boolean) {
        push(PadEvent(guestButton, if (pressed) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP,
            0x8000, System.currentTimeMillis()))
    }

    fun push(e: PadEvent) {
        synchronized(lock) {
            ring.addLast(e)
            if (ring.size > 128) ring.removeFirst()
        }
    }

    /** Drains pending events; called by the emulation thread via JNI. */
    fun drain(): List<PadEvent> {
        synchronized(lock) {
            val out = ring.toList()
            ring.clear()
            return out
        }
    }
}
