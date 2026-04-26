package com.openautolink.app.transport.direct

import android.content.Context
import android.net.wifi.WifiManager
import com.openautolink.app.diagnostics.OalLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.InetSocketAddress
import java.net.Socket

/**
 * TCP client that connects to the companion app's TCP server on the phone.
 *
 * Used in "hotspot" transport mode: the phone runs its hotspot, the car
 * joins it, and this connector reaches out to the phone's gateway IP
 * on port [COMPANION_PORT].
 *
 * The phone's IP is detected from the WiFi gateway (DHCP server = hotspot host).
 */
class TcpConnector(
    private val context: Context,
    private val scope: CoroutineScope,
    private val onSocketReady: (Socket) -> Unit,
) {
    companion object {
        private const val TAG = "OAL-TcpConn"
        const val COMPANION_PORT = 5277
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val RETRY_DELAY_MS = 3000L
    }

    private var connectJob: Job? = null

    @Volatile
    private var isRunning = false

    fun start() {
        if (isRunning) return
        isRunning = true
        OalLog.i(TAG, "Starting TCP connector (port $COMPANION_PORT)")

        connectJob = scope.launch(Dispatchers.IO) {
            while (isActive && isRunning) {
                val gatewayIp = getGatewayIp()
                if (gatewayIp == null) {
                    OalLog.d(TAG, "No WiFi gateway — waiting...")
                    delay(RETRY_DELAY_MS)
                    continue
                }

                OalLog.i(TAG, "Connecting to $gatewayIp:$COMPANION_PORT")
                try {
                    val socket = Socket()
                    socket.connect(InetSocketAddress(gatewayIp, COMPANION_PORT), CONNECT_TIMEOUT_MS)
                    socket.tcpNoDelay = true
                    OalLog.i(TAG, "Connected to companion at $gatewayIp:$COMPANION_PORT")
                    isRunning = false
                    onSocketReady(socket)
                    return@launch
                } catch (e: Exception) {
                    OalLog.d(TAG, "Connection failed: ${e.message} — retrying in ${RETRY_DELAY_MS}ms")
                    delay(RETRY_DELAY_MS)
                }
            }
        }
    }

    fun stop() {
        OalLog.i(TAG, "Stopping TCP connector")
        isRunning = false
        connectJob?.cancel()
        connectJob = null
    }

    private fun getGatewayIp(): String? {
        try {
            val wifiManager = context.applicationContext
                .getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return null
            @Suppress("DEPRECATION")
            val dhcp = wifiManager.dhcpInfo ?: return null
            val gateway = dhcp.gateway
            if (gateway == 0) return null
            // Convert int IP to string (little-endian on Android)
            return "${gateway and 0xFF}.${(gateway shr 8) and 0xFF}.${(gateway shr 16) and 0xFF}.${(gateway shr 24) and 0xFF}"
        } catch (e: Exception) {
            OalLog.e(TAG, "Gateway detection failed: ${e.message}")
            return null
        }
    }
}
