package com.openautolink.companion.trigger

import android.app.Activity
import android.content.Intent
import android.os.Build
import android.os.Bundle
import com.openautolink.companion.diagnostics.CompanionLog

/**
 * Transparent activity that surfaces the app to the foreground before
 * launching Android Auto. Required to bypass Background Activity Launch
 * (BAL) restrictions on Android 14+.
 */
class TransparentTriggerActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setBackgroundDrawableResource(android.R.color.transparent)

        val targetIntent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra("intent", Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra("intent")
        }

        if (targetIntent != null) {
            // The broadcast was already sent directly by TcpAdvertiser (works from
            // background/locked). This activity only supplements with the explicit
            // activity launch for AA versions that need it.
            try {
                startActivity(targetIntent)
                CompanionLog.d(TAG, "Activity launch succeeded")
            } catch (e: Exception) {
                CompanionLog.d(TAG, "Activity launch not available on this AA version: ${e.message}")
            }
        }
        finish()
    }

    companion object {
        private const val TAG = "OAL_Trigger"
    }
}
