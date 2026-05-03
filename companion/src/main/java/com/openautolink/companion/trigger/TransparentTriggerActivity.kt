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
            val port = targetIntent.getIntExtra("PARAM_SERVICE_PORT", 5288)
            // Try the broadcast path first — it works reliably on all known AA
            // versions and avoids the ~8s watchdog penalty when WirelessStartupActivity
            // doesn't exist (e.g. Pixel 9+/AA 13+).
            var broadcastSent = false
            try {
                val receiverIntent = Intent().apply {
                    setClassName(
                        "com.google.android.projection.gearhead",
                        "com.google.android.apps.auto.wireless.setup.receiver.WirelessStartupReceiver"
                    )
                    action = "com.google.android.apps.auto.wireless.setup.receiver.wirelessstartup.START"
                    putExtra("ip_address", "127.0.0.1")
                    putExtra("projection_port", port)
                    addFlags(Intent.FLAG_RECEIVER_FOREGROUND)
                }
                sendBroadcast(receiverIntent)
                broadcastSent = true
                CompanionLog.i(TAG, "Broadcast sent (port=$port)")
            } catch (e: Exception) {
                CompanionLog.w(TAG, "Broadcast failed: ${e.message}")
            }

            // Also fire the Activity intent as a fallback for older AA versions
            // that need it. Failure here is non-fatal if broadcast already sent.
            try {
                startActivity(targetIntent)
                CompanionLog.i(TAG, "Activity launch succeeded (port=$port)")
            } catch (e: Exception) {
                if (!broadcastSent) {
                    CompanionLog.e(TAG, "Both triggers failed: ${e.message}")
                } else {
                    CompanionLog.d(TAG, "Activity launch skipped (not available on this AA version): ${e.message}")
                }
            }
        }
        finish()
    }

    companion object {
        private const val TAG = "OAL_Trigger"
    }
}
