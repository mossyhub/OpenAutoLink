package com.openautolink.app.input

import android.view.MotionEvent
import com.openautolink.app.transport.ControlMessage

/**
 * Converts Android MotionEvents to OAL touch control messages and sends them
 * via the provided callback. Handles single-touch and multi-touch events.
 *
 * OAL action codes (matching Android MotionEvent):
 * - 0 = ACTION_DOWN
 * - 1 = ACTION_UP
 * - 2 = ACTION_MOVE
 * - 3 = ACTION_CANCEL
 * - 5 = ACTION_POINTER_DOWN
 * - 6 = ACTION_POINTER_UP
 *
 * Single-touch uses x/y/pointer_id fields.
 * Multi-touch (2+ pointers) uses the pointers array.
 */
class TouchForwarderImpl(
    private val sendMessage: (ControlMessage.Touch) -> Unit
) : TouchForwarder {

    override fun onTouch(
        event: MotionEvent,
        surfaceWidth: Int,
        surfaceHeight: Int,
        videoWidth: Int,
        videoHeight: Int
    ) {
        if (surfaceWidth <= 0 || surfaceHeight <= 0 || videoWidth <= 0 || videoHeight <= 0) return

        val actionMasked = event.actionMasked

        // Map Android action to OAL action code
        val oalAction = when (actionMasked) {
            MotionEvent.ACTION_DOWN -> 0
            MotionEvent.ACTION_UP -> 1
            MotionEvent.ACTION_MOVE -> 2
            MotionEvent.ACTION_CANCEL -> 3
            MotionEvent.ACTION_POINTER_DOWN -> 5
            MotionEvent.ACTION_POINTER_UP -> 6
            else -> return // Ignore hover, scroll, etc.
        }

        val pointerCount = event.pointerCount

        if (pointerCount == 1 && actionMasked != MotionEvent.ACTION_POINTER_DOWN
            && actionMasked != MotionEvent.ACTION_POINTER_UP
        ) {
            // Single-touch: use x/y/pointer_id fields
            val (scaledX, scaledY) = TouchScaler.scalePoint(
                event.x, event.y,
                surfaceWidth, surfaceHeight,
                videoWidth, videoHeight
            )
            sendMessage(
                ControlMessage.Touch(
                    action = oalAction,
                    x = scaledX,
                    y = scaledY,
                    pointerId = event.getPointerId(0),
                    pointers = null
                )
            )
        } else {
            // Multi-touch: use pointers array
            val pointers = (0 until pointerCount).map { i ->
                val (scaledX, scaledY) = TouchScaler.scalePoint(
                    event.getX(i), event.getY(i),
                    surfaceWidth, surfaceHeight,
                    videoWidth, videoHeight
                )
                ControlMessage.Pointer(
                    id = event.getPointerId(i),
                    x = scaledX,
                    y = scaledY
                )
            }
            sendMessage(
                ControlMessage.Touch(
                    action = oalAction,
                    x = null,
                    y = null,
                    pointerId = null,
                    pointers = pointers,
                    actionIndex = event.actionIndex
                )
            )
        }
    }
}
