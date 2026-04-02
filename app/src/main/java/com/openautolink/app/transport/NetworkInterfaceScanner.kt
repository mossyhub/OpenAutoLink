package com.openautolink.app.transport

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import java.net.Inet4Address
import java.net.NetworkInterface

data class NetworkInterfaceInfo(
    val name: String,
    val displayName: String,
    val macAddress: String,
    val ipAddress: String?,
    val isUp: Boolean,
    val transport: String = "Ethernet",
    val networkHandle: Long = -1,
)

/**
 * Discovers network interfaces on the AAOS head unit.
 *
 * Uses ConnectivityManager.allNetworks (the Android-proper approach) which works
 * on both physical devices and emulators. Falls back to java.net.NetworkInterface
 * if ConnectivityManager returns nothing.
 */
class NetworkInterfaceScanner(private val context: Context) {

    companion object {
        private const val TAG = "NetworkInterfaceScanner"
    }

    private val _interfaces = MutableStateFlow<List<NetworkInterfaceInfo>>(emptyList())
    val interfaces: StateFlow<List<NetworkInterfaceInfo>> = _interfaces

    suspend fun scan(): List<NetworkInterfaceInfo> = withContext(Dispatchers.IO) {
        val result = scanViaConnectivityManager() .ifEmpty { scanViaJavaNet() }
        Log.d(TAG, "Found ${result.size} interfaces: ${result.map { "${it.name}(${it.transport})" }}")
        _interfaces.value = result
        result
    }

    private fun scanViaConnectivityManager(): List<NetworkInterfaceInfo> {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            ?: return emptyList()
        val result = mutableListOf<NetworkInterfaceInfo>()
        try {
            for (network in cm.allNetworks) {
                val caps = cm.getNetworkCapabilities(network) ?: continue
                val linkProps = cm.getLinkProperties(network) ?: continue
                val ifaceName = linkProps.interfaceName ?: continue

                val transport = when {
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "Ethernet"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_USB) -> "USB"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "WiFi"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "Cellular"
                    else -> "Other"
                }

                val ipv4 = linkProps.linkAddresses
                    .map { it.address }
                    .filterIsInstance<Inet4Address>()
                    .firstOrNull()
                    ?.hostAddress

                val hwAddr = try {
                    NetworkInterface.getByName(ifaceName)?.hardwareAddress
                        ?.joinToString(":") { "%02x".format(it) } ?: ""
                } catch (_: Exception) { "" }

                result.add(
                    NetworkInterfaceInfo(
                        name = ifaceName,
                        displayName = "$ifaceName ($transport)",
                        macAddress = hwAddr,
                        ipAddress = ipv4,
                        isUp = true,
                        transport = transport,
                        networkHandle = network.networkHandle,
                    )
                )
            }
        } catch (e: Exception) {
            Log.w(TAG, "ConnectivityManager scan failed", e)
        }
        return result
    }

    /** Fallback: scan via java.net.NetworkInterface (works when CM returns nothing). */
    private fun scanViaJavaNet(): List<NetworkInterfaceInfo> {
        val excluded = listOf(
            "lo", "dummy", "tun", "sit", "ip6", "gre", "erspan",
            "ip_vti", "ifb", "hwsim", "rmnet",
        )
        val result = mutableListOf<NetworkInterfaceInfo>()
        try {
            val netInterfaces = NetworkInterface.getNetworkInterfaces() ?: return emptyList()
            for (iface in netInterfaces) {
                if (iface.isLoopback) continue
                if (excluded.any { iface.name.startsWith(it) }) continue
                if (!iface.isUp) continue
                val hwAddr = iface.hardwareAddress
                val mac = hwAddr?.joinToString(":") { "%02x".format(it) } ?: ""
                val ipv4 = iface.inetAddresses.toList()
                    .filterIsInstance<Inet4Address>()
                    .firstOrNull()
                    ?.hostAddress
                if (ipv4 == null && hwAddr == null) continue
                val transport = when {
                    iface.name.startsWith("wlan") || iface.name.startsWith("p2p") -> "WiFi"
                    else -> "Ethernet"
                }
                result.add(
                    NetworkInterfaceInfo(
                        name = iface.name,
                        displayName = "${iface.name} ($transport)",
                        macAddress = mac,
                        ipAddress = ipv4,
                        isUp = true,
                        transport = transport,
                    )
                )
            }
        } catch (e: Exception) {
            Log.w(TAG, "java.net scan failed", e)
        }
        return result
    }
}
