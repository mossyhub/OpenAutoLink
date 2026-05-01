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
    }
}
