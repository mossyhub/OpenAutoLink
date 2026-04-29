package com.openautolink.app.diagnostics

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.ArrayDeque

/**
 * A single local log entry captured by [DiagnosticLog].
 */
data class LocalLogEntry(
    val timestamp: Long,
    val level: DiagnosticLevel,
    val tag: String,
    val message: String,
)

/**
 * Global diagnostic logger — subsystems call this to emit diagnostic log events.
 *
 * This is a thin facade over [RemoteDiagnostics] that allows any subsystem
 * to log without holding a direct reference to the diagnostics instance.
 * The session manager sets the active instance; when null, calls are no-ops.
 *
 * Local buffering is **always on** — entries are kept in a fixed-capacity
 * ring buffer (500 entries, ~500 KB worst case) and snapshotted to a
 * StateFlow for UI collection. This ensures logs are available when the
 * diagnostics screen opens, even if the log-producing event happened earlier.
 *
 * Thread-safe: synchronized ring buffer, volatile flags.
 */
object DiagnosticLog {

    private const val MAX_LOCAL_ENTRIES = 500

    /**
     * Called from native C++ via JNI to route native log lines into the
     * Kotlin diagnostic pipeline (ring buffer, file logger, TCP server).
     * Registered in aasdk_jni.cpp JNI_OnLoad.
     */
    @JvmStatic
    fun nativeLog(level: Int, tag: String, msg: String) {
        val diagLevel = when (level) {
            3 -> DiagnosticLevel.DEBUG   // ANDROID_LOG_DEBUG
            4 -> DiagnosticLevel.INFO    // ANDROID_LOG_INFO
            5 -> DiagnosticLevel.WARN    // ANDROID_LOG_WARN
            else -> DiagnosticLevel.ERROR // ANDROID_LOG_ERROR + anything else
        }
        addLocal(diagLevel, tag, msg)
        instance?.log(diagLevel, tag, msg)
    }

    @Volatile
    var instance: RemoteDiagnostics? = null

    /** Remote log server for TCP streaming to laptop. Set by DiagnosticsViewModel. */
    @Volatile
    var remoteLogServer: RemoteLogServer? = null

    /** File log writer for USB stick logging. Set by ProjectionViewModel. */
    @Volatile
    var fileLogWriter: FileLogWriter? = null

    private val ring = ArrayDeque<LocalLogEntry>(MAX_LOCAL_ENTRIES)
    private val _localLogs = MutableStateFlow<List<LocalLogEntry>>(emptyList())
    val localLogs: StateFlow<List<LocalLogEntry>> = _localLogs.asStateFlow()

    /** No-op — local capture is always active. Kept for API compatibility. */
    fun startLocalCapture() { /* always on */ }

    /** No-op — logs are retained across screen navigation. Kept for API compatibility. */
    fun stopLocalCapture() { /* don't clear */ }

    fun d(tag: String, msg: String) {
        addLocal(DiagnosticLevel.DEBUG, tag, msg)
        instance?.log(DiagnosticLevel.DEBUG, tag, msg)
    }

    fun i(tag: String, msg: String) {
        addLocal(DiagnosticLevel.INFO, tag, msg)
        instance?.log(DiagnosticLevel.INFO, tag, msg)
    }

    fun w(tag: String, msg: String) {
        addLocal(DiagnosticLevel.WARN, tag, msg)
        instance?.log(DiagnosticLevel.WARN, tag, msg)
    }

    fun e(tag: String, msg: String) {
        addLocal(DiagnosticLevel.ERROR, tag, msg)
        instance?.log(DiagnosticLevel.ERROR, tag, msg)
    }

    fun clearLocal() {
        synchronized(ring) { ring.clear() }
        _localLogs.value = emptyList()
    }

    private fun addLocal(level: DiagnosticLevel, tag: String, msg: String) {
        val entry = LocalLogEntry(
            timestamp = System.currentTimeMillis(),
            level = level,
            tag = tag,
            message = if (msg.length > 500) msg.take(500) else msg,
        )
        synchronized(ring) {
            if (ring.size >= MAX_LOCAL_ENTRIES) ring.pollFirst()
            ring.addLast(entry)
            _localLogs.value = ring.toList()
        }
        // Stream to TCP clients if remote log server is active
        remoteLogServer?.broadcast(entry)
        // Write to file if file logging is active
        fileLogWriter?.write(entry)
    }
}
