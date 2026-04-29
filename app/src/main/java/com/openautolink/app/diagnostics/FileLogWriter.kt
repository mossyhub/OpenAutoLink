package com.openautolink.app.diagnostics

import android.content.Context
import android.os.Environment
import android.os.StatFs
import java.io.BufferedWriter
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Writes [LocalLogEntry] lines to a timestamped log file on external storage.
 *
 * Files are written to the first available removable storage (USB stick),
 * falling back to the primary external storage (internal shared storage).
 * All files go under an `openautolink/logs/` directory.
 *
 * Each start creates a new file: `oal_2026-04-28_14-30-00.log`
 * Lines use the same format as [RemoteLogServer]:
 *   `HH:mm:ss.SSS I/tag: message`
 *
 * Thread-safe: all writes go through a synchronized block.
 * Stops writing if the file exceeds [MAX_FILE_SIZE_BYTES] or disk is low.
 */
class FileLogWriter(private val context: Context) {

    companion object {
        private const val TAG = "FileLogWriter"
        private const val DIR_NAME = "openautolink/logs"
        private const val MAX_FILE_SIZE_BYTES = 50L * 1024 * 1024 // 50 MB per file
        private const val MIN_FREE_SPACE_BYTES = 10L * 1024 * 1024 // stop if < 10 MB free
    }

    @Volatile
    var isActive: Boolean = false
        private set

    /** Absolute path of the current log file, or null if not started. */
    @Volatile
    var currentFilePath: String? = null
        private set

    private var writer: BufferedWriter? = null
    private val lock = Any()
    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
    private val fileNameFormat = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US)

    /**
     * Start writing logs to a new file. Returns the file path or null on failure.
     */
    fun start(): String? {
        synchronized(lock) {
            if (isActive) return currentFilePath

            val dir = resolveLogDir() ?: run {
                OalLog.w(TAG, "No writable storage found for file logging")
                return null
            }

            val fileName = "oal_${fileNameFormat.format(Date())}.log"
            val file = File(dir, fileName)

            return try {
                val fos = FileOutputStream(file, true)
                writer = BufferedWriter(OutputStreamWriter(fos, Charsets.UTF_8), 8192)
                currentFilePath = file.absolutePath
                isActive = true
                OalLog.i(TAG, "File logging started: ${file.absolutePath}")
                file.absolutePath
            } catch (e: Exception) {
                OalLog.e(TAG, "Failed to start file logging: ${e.message}")
                null
            }
        }
    }

    /**
     * Stop writing and flush/close the file.
     */
    fun stop() {
        synchronized(lock) {
            if (!isActive) return
            isActive = false
            try {
                writer?.flush()
                writer?.close()
            } catch (_: Exception) { }
            writer = null
            OalLog.i(TAG, "File logging stopped: $currentFilePath")
            currentFilePath = null
        }
    }

    /**
     * Write a single log entry. Called from [DiagnosticLog.addLocal].
     * No-op if not active or file limits exceeded.
     */
    fun write(entry: LocalLogEntry) {
        if (!isActive) return
        synchronized(lock) {
            val w = writer ?: return
            try {
                // Check file size limit
                val path = currentFilePath ?: return
                val fileSize = File(path).length()
                if (fileSize >= MAX_FILE_SIZE_BYTES) {
                    OalLog.w(TAG, "File log reached ${MAX_FILE_SIZE_BYTES / 1024 / 1024}MB limit, stopping")
                    stop()
                    return
                }

                val levelChar = when (entry.level) {
                    DiagnosticLevel.DEBUG -> 'D'
                    DiagnosticLevel.INFO -> 'I'
                    DiagnosticLevel.WARN -> 'W'
                    DiagnosticLevel.ERROR -> 'E'
                }

                w.write(timeFormat.format(Date(entry.timestamp)))
                w.write(" ")
                w.write(levelChar.toString())
                w.write("/")
                w.write(entry.tag)
                w.write(": ")
                w.write(entry.message)
                w.newLine()
                w.flush()
            } catch (e: Exception) {
                // Don't recurse through OalLog — just stop silently
                isActive = false
                try { w.close() } catch (_: Exception) { }
                writer = null
            }
        }
    }

    /**
     * Flush any buffered entries to disk, also write existing ring buffer entries.
     */
    fun writeExistingLogs(entries: List<LocalLogEntry>) {
        if (!isActive) return
        synchronized(lock) {
            for (entry in entries) {
                write(entry)
            }
        }
    }

    /**
     * Find the best directory for log files.
     * Prefers removable storage (USB stick) over internal shared storage.
     */
    private fun resolveLogDir(): File? {
        // Try removable storage volumes first (USB sticks)
        val externalDirs = context.getExternalFilesDirs(null)
        for (dir in externalDirs) {
            if (dir == null) continue
            // Skip primary (non-removable) storage
            if (Environment.isExternalStorageRemovable(dir)) {
                val logDir = File(dir, DIR_NAME)
                if (ensureDir(logDir)) return logDir
            }
        }

        // Fallback: primary external storage (internal shared storage)
        val primary = context.getExternalFilesDir(null)
        if (primary != null) {
            val logDir = File(primary, DIR_NAME)
            if (ensureDir(logDir)) return logDir
        }

        return null
    }

    private fun ensureDir(dir: File): Boolean {
        if (!dir.exists() && !dir.mkdirs()) return false
        // Check free space
        return try {
            val stat = StatFs(dir.absolutePath)
            stat.availableBytes > MIN_FREE_SPACE_BYTES
        } catch (_: Exception) {
            false
        }
    }
}
