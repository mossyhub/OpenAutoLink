package com.openautolink.app.input

import android.view.MotionEvent
import com.openautolink.app.transport.ControlMessage
import io.mockk.every
import io.mockk.mockk
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchForwarderImplTest {

    private val sentMessages = mutableListOf<ControlMessage.Touch>()
    private val forwarder = TouchForwarderImpl { sentMessages.add(it) }

    // Surface: 800x480, Video: 1920x1080

    @Test
    fun `single ACTION_DOWN sends touch with scaled coordinates`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_DOWN,
            x = 400f, y = 240f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        val msg = sentMessages[0]
        assertEquals(0, msg.action) // ACTION_DOWN = 0
        assertNotNull(msg.x)
        assertNotNull(msg.y)
        assertEquals(960f, msg.x!!, 0.5f)
        assertEquals(540f, msg.y!!, 0.5f)
        assertEquals(0, msg.pointerId)
        assertNull(msg.pointers)
    }

    @Test
    fun `single ACTION_UP sends touch with action 1`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_UP,
            x = 100f, y = 50f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        assertEquals(1, sentMessages[0].action)
    }

    @Test
    fun `single ACTION_MOVE sends touch with action 2`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_MOVE,
            x = 200f, y = 100f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        assertEquals(2, sentMessages[0].action)
    }

    @Test
    fun `ACTION_CANCEL sends action 3`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_CANCEL,
            x = 0f, y = 0f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        assertEquals(3, sentMessages[0].action)
    }

    @Test
    fun `ACTION_POINTER_DOWN sends multi-touch with pointers array`() {
        val event = mockMultiTouchEvent(
            action = MotionEvent.ACTION_POINTER_DOWN,
            pointers = listOf(
                PointerData(id = 0, x = 200f, y = 100f),
                PointerData(id = 1, x = 600f, y = 300f)
            )
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        val msg = sentMessages[0]
        assertEquals(5, msg.action) // ACTION_POINTER_DOWN = 5
        assertNull(msg.x)
        assertNull(msg.y)
        assertNull(msg.pointerId)
        assertNotNull(msg.pointers)
        assertEquals(2, msg.pointers!!.size)

        // Pointer 0: (200/800)*1920=480, (100/480)*1080=225
        assertEquals(0, msg.pointers!![0].id)
        assertEquals(480f, msg.pointers!![0].x, 0.5f)
        assertEquals(225f, msg.pointers!![0].y, 0.5f)

        // Pointer 1: (600/800)*1920=1440, (300/480)*1080=675
        assertEquals(1, msg.pointers!![1].id)
        assertEquals(1440f, msg.pointers!![1].x, 0.5f)
        assertEquals(675f, msg.pointers!![1].y, 0.5f)
    }

    @Test
    fun `ACTION_POINTER_UP sends multi-touch with action 6`() {
        val event = mockMultiTouchEvent(
            action = MotionEvent.ACTION_POINTER_UP,
            pointers = listOf(
                PointerData(id = 0, x = 400f, y = 240f),
                PointerData(id = 1, x = 600f, y = 300f)
            )
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        assertEquals(6, sentMessages[0].action)
        assertEquals(2, sentMessages[0].pointers!!.size)
    }

    @Test
    fun `ACTION_MOVE with multiple pointers sends pointers array`() {
        val event = mockMultiTouchEvent(
            action = MotionEvent.ACTION_MOVE,
            pointers = listOf(
                PointerData(id = 0, x = 200f, y = 100f),
                PointerData(id = 1, x = 600f, y = 300f)
            )
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertEquals(1, sentMessages.size)
        val msg = sentMessages[0]
        assertEquals(2, msg.action)
        assertNotNull(msg.pointers)
        assertEquals(2, msg.pointers!!.size)
    }

    @Test
    fun `ignores unsupported actions like hover`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_HOVER_MOVE,
            x = 100f, y = 100f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 1920, 1080)

        assertTrue(sentMessages.isEmpty())
    }

    @Test
    fun `does not send when video dimensions are zero`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_DOWN,
            x = 100f, y = 100f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 800, 480, 0, 0)

        assertTrue(sentMessages.isEmpty())
    }

    @Test
    fun `does not send when surface dimensions are zero`() {
        val event = mockMotionEvent(
            action = MotionEvent.ACTION_DOWN,
            x = 100f, y = 100f,
            pointerCount = 1,
            pointerId = 0
        )

        forwarder.onTouch(event, 0, 0, 1920, 1080)

        assertTrue(sentMessages.isEmpty())
    }

    // --- Helpers ---

    private data class PointerData(val id: Int, val x: Float, val y: Float)

    private fun mockMotionEvent(
        action: Int,
        x: Float,
        y: Float,
        pointerCount: Int,
        pointerId: Int
    ): MotionEvent = mockk {
        every { actionMasked } returns action
        every { this@mockk.pointerCount } returns pointerCount
        every { this@mockk.x } returns x
        every { this@mockk.y } returns y
        every { getPointerId(0) } returns pointerId
        every { getX(0) } returns x
        every { getY(0) } returns y
    }

    private fun mockMultiTouchEvent(
        action: Int,
        pointers: List<PointerData>,
        actionIdx: Int = 0
    ): MotionEvent = mockk {
        every { actionMasked } returns action
        every { actionIndex } returns actionIdx
        every { pointerCount } returns pointers.size
        pointers.forEachIndexed { index, data ->
            every { getX(index) } returns data.x
            every { getY(index) } returns data.y
            every { getPointerId(index) } returns data.id
        }
        if (pointers.isNotEmpty()) {
            every { x } returns pointers[0].x
            every { y } returns pointers[0].y
        }
    }
}
