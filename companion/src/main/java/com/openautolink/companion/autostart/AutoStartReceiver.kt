package com.openautolink.companion.autostart

import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.content.ContextCompat
import com.openautolink.companion.CompanionPrefs
import com.openautolink.companion.diagnostics.CompanionLog
import com.openautolink.companion.service.CompanionService

/**
 * Broadcast receiver for Bluetooth ACL connect/disconnect events.
 * When a target BT device connects and auto-start mode is BT,
 * starts the CompanionService to begin Nearby advertising.
 */
class AutoStartReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val prefs = context.getSharedPreferences(CompanionPrefs.NAME, Context.MODE_PRIVATE)
        val autoStartMode = prefs.getInt(CompanionPrefs.AUTO_START_MODE, 0)
        if (autoStartMode != CompanionPrefs.AUTO_START_BT &&
            autoStartMode != CompanionPrefs.AUTO_START_BT_AND_WIFI) return

        val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
        } ?: return

        val deviceAddress = device.address
        val targetMacs = prefs.getStringSet(CompanionPrefs.AUTO_START_BT_MACS, emptySet())
            ?: emptySet()
        if (!targetMacs.contains(deviceAddress)) return

        when (intent.action) {
            BluetoothDevice.ACTION_ACL_CONNECTED -> {
                CompanionLog.i(TAG, "Target BT connected: $deviceAddress")

                // Check if device is fully connected (not just ACL)
                val isFullyConnected = try {
                    val method = device.javaClass.getMethod("isConnected")
                    method.invoke(device) as? Boolean ?: true
                } catch (_: Exception) {
                    true
                }

                if (!isFullyConnected) {
                    CompanionLog.w(TAG, "ACL up but isConnected()=false, skipping")
                    return
                }

                CompanionLog.i(TAG, "Starting CompanionService via BT auto-start")

                // Pre-warm AA immediately so it's receptive by the time the car
                // establishes its TCP connection (~10s later). Without this, AA
                // can take 60-90s to respond to the real trigger because it starts
                // from a stopped/deep-sleep state. The pre-warm broadcast wakes AA
                // with a dummy port; AA starts its wireless setup service and will
                // respond to the real port trigger almost instantly.
                prewarmAndroidAuto(context)

                val serviceIntent = Intent(context, CompanionService::class.java).apply {
                    action = CompanionService.ACTION_START
                }
                try {
                    ContextCompat.startForegroundService(context, serviceIntent)
                } catch (e: Exception) {
                    CompanionLog.e(TAG, "Failed to start service: ${e.message}")
                }
            }

            BluetoothDevice.ACTION_ACL_DISCONNECTED -> {
                CompanionLog.i(TAG, "Target BT disconnected: $deviceAddress")
                val stopOnDisconnect = prefs.getBoolean(CompanionPrefs.BT_DISCONNECT_STOP, false)
                if (stopOnDisconnect) {
                    CompanionLog.i(TAG, "Stopping service (bt_disconnect_stop=true)")
                    val serviceIntent = Intent(context, CompanionService::class.java).apply {
                        action = CompanionService.ACTION_STOP
                    }
                    context.startService(serviceIntent)
                }
            }
        }
    }

    companion object {
        private const val TAG = "OAL_BtAutoStart"

        /**
         * Send a broadcast to Google AA to wake it from stopped/deep-sleep state.
         * Uses port 0 as a dummy — AA will start its wireless projection service
         * but won't be able to connect anywhere yet. When the real trigger fires
         * with the actual proxy port a few seconds later, AA is already warm and
         * connects in <1s instead of 60-90s.
         */
        fun prewarmAndroidAuto(context: Context) {
            try {
                val intent = Intent().apply {
                    setClassName(
                        "com.google.android.projection.gearhead",
                        "com.google.android.apps.auto.wireless.setup.receiver.WirelessStartupReceiver"
                    )
                    action = "com.google.android.apps.auto.wireless.setup.receiver.wirelessstartup.START"
                    putExtra("ip_address", "127.0.0.1")
                    putExtra("projection_port", 0)
                    addFlags(Intent.FLAG_RECEIVER_FOREGROUND)
                }
                context.sendBroadcast(intent)
                CompanionLog.i(TAG, "AA pre-warm broadcast sent")
            } catch (e: Exception) {
                CompanionLog.w(TAG, "AA pre-warm failed: ${e.message}")
            }
        }
    }
}
