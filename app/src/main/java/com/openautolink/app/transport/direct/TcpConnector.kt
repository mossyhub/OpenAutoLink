package com.openautolink.app.transport.direct

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
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
 * Discovery strategy (tried in order):
 * 1. mDNS/NSD: discovers `_openautolink._tcp` service (works on any shared network)
 * 2. WiFi gateway IP: connects to DHCP gateway on [COMPANION_PORT] (hotspot mode)
 *
 * On phone hotspot, gateway = phone, so strategy 2 works immediately.
 * On shared WiFi (home/office), strategy 1 finds the phone via mDNS.
 */
class TcpConnector(
    private val context: Context,
    private val scope: CoroutineScope,
    private val onSocketReady: (Socket) -> Unit,
) {
    companion object {
        private const val TAG = "OAL-TcpConn"
        const val COMPANION_PORT = 5277
        const val NSD_SERVICE_TYPE = "_openautolink._tcp"
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val RETRY_DELAY_MS = 3000L
    }

    private var connectJob: Job? = null
    private var nsdManager: NsdManager? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null

    @Volatile
    private var isRunning = false

    @Volatile
    private var nsdFoundHost: String? = null

    @Volatile
    private var nsdFoundPort: Int = 0

    fun start() {
        if (isRunning) return
        isRunning = true
        OalLog.i(TAG, "Starting TCP connector (port $COMPANION_PORT)")

        startNsdDiscovery()

        connectJob = scope.launch(Dispatchers.IO) {
            while (isActive && isRunning) {
                // Try mDNS-discovered host first
                val nsdHost = nsdFoundHost
                if (nsdHost != null && nsdFoundPort > 0) {
                    if (tryConnect(nsdHost, nsdFoundPort, "mDNS")) return@launch
                }

                // Fall back to gateway IP (works on phone hotspot)
                val gatewayIp = getGatewayIp()
                if (gatewayIp != null) {
                    if (tryConnect(gatewayIp, COMPANION_PORT, "gateway")) return@launch
                } else {
                    OalLog.d(TAG, "No WiFi gateway — waiting for mDNS or network...")
                }

                delay(RETRY_DELAY_MS)
            }
        }
    }

    private fun tryConnect(host: String, port: Int, source: String): Boolean {
        OalLog.i(TAG, "Connecting to $host:$port ($source)")
        return try {
            val socket = Socket()
            socket.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            socket.tcpNoDelay = true
            OalLog.i(TAG, "Connected to companion at $host:$port ($source)")
            isRunning = false
            stopNsdDiscovery()
            onSocketReady(socket)
            true
        } catch (e: Exception) {
            OalLog.d(TAG, "Connection to $host:$port ($source) failed: ${e.message}")
            false
        }
    }

    private fun startNsdDiscovery() {
        try {
            nsdManager = context.getSystemService(Context.NSD_SERVICE) as? NsdManager
            discoveryListener = object : NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(serviceType: String) {
                    OalLog.i(TAG, "mDNS discovery started for $serviceType")
                }

                override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                    OalLog.i(TAG, "mDNS service found: ${serviceInfo.serviceName}")
                    nsdManager?.resolveService(serviceInfo, object : NsdManager.ResolveListener {
                        override fun onResolveFailed(si: NsdServiceInfo, errorCode: Int) {
                            OalLog.w(TAG, "mDNS resolve failed: error $errorCode")
                        }

                        override fun onServiceResolved(si: NsdServiceInfo) {
                            val host = si.host?.hostAddress
                            val port = si.port
                            OalLog.i(TAG, "mDNS resolved: $host:$port")
                            if (host != null && port > 0) {
                                nsdFoundHost = host
                                nsdFoundPort = port
                            }
                        }
                    })
                }

                override fun onServiceLost(serviceInfo: NsdServiceInfo) {
                    OalLog.d(TAG, "mDNS service lost: ${serviceInfo.serviceName}")
                    nsdFoundHost = null
                    nsdFoundPort = 0
                }

                override fun onDiscoveryStopped(serviceType: String) {
                    OalLog.d(TAG, "mDNS discovery stopped")
                }

                override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                    OalLog.w(TAG, "mDNS discovery start failed: error $errorCode")
                }

                override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {
                    OalLog.w(TAG, "mDNS discovery stop failed: error $errorCode")
                }
            }
            nsdManager?.discoverServices(NSD_SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
        } catch (e: Exception) {
            OalLog.w(TAG, "mDNS discovery init failed: ${e.message}")
        }
    }

    private fun stopNsdDiscovery() {
        try {
            discoveryListener?.let { nsdManager?.stopServiceDiscovery(it) }
        } catch (_: Exception) {}
        discoveryListener = null
    }

    fun stop() {
        OalLog.i(TAG, "Stopping TCP connector")
        isRunning = false
        connectJob?.cancel()
        connectJob = null
        stopNsdDiscovery()
    }

    private fun getGatewayIp(): String? {
        try {
            val wifiManager = context.applicationContext
                .getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return null
            @Suppress("DEPRECATION")
            val dhcp = wifiManager.dhcpInfo ?: return null
            val gateway = dhcp.gateway
            if (gateway == 0) return null
            return "${gateway and 0xFF}.${(gateway shr 8) and 0xFF}.${(gateway shr 16) and 0xFF}.${(gateway shr 24) and 0xFF}"
        } catch (e: Exception) {
            OalLog.e(TAG, "Gateway detection failed: ${e.message}")
            return null
        }
    }
}
