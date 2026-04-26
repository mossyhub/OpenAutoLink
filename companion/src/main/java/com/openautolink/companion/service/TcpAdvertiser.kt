package com.openautolink.companion.service

import android.content.Context
import android.content.Intent
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import com.openautolink.companion.connection.AaProxy
import com.openautolink.companion.trigger.TransparentTriggerActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * TCP transport for car ←→ phone AA connection.
 *
 * Listens on [PORT] for incoming TCP connections from the car app.
 * Also registers an mDNS/NSD service so the car can discover the phone
 * on any shared network (hotspot or home WiFi).
 */
class TcpAdvertiser(
    private val context: Context,
    private val stateListener: NearbyAdvertiser.StateListener,
) {
    companion object {
        private const val TAG = "OAL_TcpAdv"
        const val PORT = 5277
        const val NSD_SERVICE_TYPE = "_openautolink._tcp"
        const val NSD_SERVICE_NAME = "OpenAutoLink"
    }

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var serverSocket: ServerSocket? = null
    private var activeProxy: AaProxy? = null
    private var activeCarSocket: Socket? = null
    private var nsdManager: NsdManager? = null
    private var nsdRegistrationListener: NsdManager.RegistrationListener? = null

    @Volatile
    private var isRunning = false

    @Volatile
    private var isLaunching = false

    fun start() {
        if (isRunning) return
        isRunning = true
        Log.i(TAG, "Starting TCP server on port $PORT")

        scope.launch {
            try {
                val server = ServerSocket()
                server.reuseAddress = true
                server.bind(InetSocketAddress(PORT))
                serverSocket = server
                Log.i(TAG, "Listening on 0.0.0.0:$PORT")

                registerNsd()

                while (isRunning) {
                    val carSocket = server.accept()
                    Log.i(TAG, "Car connected from ${carSocket.inetAddress.hostAddress}")
                    stateListener.onConnecting()
                    handleCarConnection(carSocket)
                }
            } catch (e: Exception) {
                if (isRunning) {
                    Log.e(TAG, "TCP server error: ${e.message}")
                }
            }
        }
    }

    private fun handleCarConnection(carSocket: Socket) {
        // Close any previous session
        activeProxy?.stop()
        activeCarSocket?.let { runCatching { it.close() } }
        activeCarSocket = carSocket
        isLaunching = false

        launchAndroidAuto(carSocket)
    }

    private fun launchAndroidAuto(carSocket: Socket) {
        if (isLaunching) return
        isLaunching = true

        scope.launch {
            try {
                val proxy = AaProxy(
                    preConnectedSocket = carSocket,
                    listener = object : AaProxy.Listener {
                        override fun onConnected() {
                            Log.i(TAG, "AA flowing through TCP proxy")
                            stateListener.onProxyConnected()
                        }

                        override fun onDisconnected() {
                            Log.i(TAG, "AA TCP proxy disconnected")
                            stateListener.onProxyDisconnected()
                            // Re-accept next connection
                            isLaunching = false
                        }
                    },
                )
                activeProxy = proxy
                val localPort = proxy.start()

                val aaIntent = Intent().apply {
                    setClassName(
                        "com.google.android.projection.gearhead",
                        "com.google.android.apps.auto.wireless.setup.service.impl.WirelessStartupActivity"
                    )
                    addFlags(
                        Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_ACTIVITY_CLEAR_TOP or
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                    )
                    putExtra("PARAM_HOST_ADDRESS", "127.0.0.1")
                    putExtra("PARAM_SERVICE_PORT", localPort)
                    putExtra("ip_address", "127.0.0.1")
                    putExtra("projection_port", localPort)
                }

                Log.i(TAG, "Launching AA via TransparentTrigger, proxy port=$localPort")
                val triggerIntent =
                    Intent(context, TransparentTriggerActivity::class.java).apply {
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
                        putExtra("intent", aaIntent)
                    }
                context.startActivity(triggerIntent)

                // Don't call onLaunchTimeout — that's for Nearby retry logic.
                // TCP stays connected; AA will connect to the proxy when ready.
            } catch (e: Exception) {
                Log.e(TAG, "Failed to launch AA: ${e.message}")
                isLaunching = false
                stateListener.onProxyDisconnected()
            }
        }
    }

    fun stop() {
        Log.i(TAG, "Stopping TCP server")
        isRunning = false
        unregisterNsd()
        activeProxy?.stop()
        activeProxy = null
        activeCarSocket?.let { runCatching { it.close() } }
        activeCarSocket = null
        runCatching { serverSocket?.close() }
        serverSocket = null
        scope.cancel()
    }

    private fun registerNsd() {
        try {
            nsdManager = context.getSystemService(Context.NSD_SERVICE) as? NsdManager
            val serviceInfo = NsdServiceInfo().apply {
                serviceName = NSD_SERVICE_NAME
                serviceType = NSD_SERVICE_TYPE
                port = PORT
            }
            nsdRegistrationListener = object : NsdManager.RegistrationListener {
                override fun onRegistrationFailed(si: NsdServiceInfo, errorCode: Int) {
                    Log.w(TAG, "mDNS registration failed: error $errorCode")
                }
                override fun onUnregistrationFailed(si: NsdServiceInfo, errorCode: Int) {
                    Log.w(TAG, "mDNS unregistration failed: error $errorCode")
                }
                override fun onServiceRegistered(si: NsdServiceInfo) {
                    Log.i(TAG, "mDNS registered: ${si.serviceName} on port $PORT")
                }
                override fun onServiceUnregistered(si: NsdServiceInfo) {
                    Log.d(TAG, "mDNS unregistered")
                }
            }
            nsdManager?.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, nsdRegistrationListener)
        } catch (e: Exception) {
            Log.w(TAG, "mDNS registration failed: ${e.message}")
        }
    }

    private fun unregisterNsd() {
        try {
            nsdRegistrationListener?.let { nsdManager?.unregisterService(it) }
        } catch (_: Exception) {}
        nsdRegistrationListener = null
    }
}
