package com.openautolink.app.input

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.GnssStatus
import android.location.LocationManager
import android.util.Log
import com.openautolink.app.diagnostics.DiagnosticLog
import com.openautolink.app.transport.ControlMessage

/**
 * Reads accelerometer, gyroscope, and magnetic field sensors from the AAOS hardware
 * and forwards them as IMU fields in VehicleData messages.
 *
 * Also reads GnssStatus for satellite count.
 *
 * Rate-limited to ~10 Hz to avoid flooding the control channel.
 */
class ImuForwarder(
    private val context: Context,
    private val sendMessage: (ControlMessage.VehicleData) -> Unit
) : SensorEventListener {

    companion object {
        private const val TAG = "ImuForwarder"
        private const val SEND_INTERVAL_MS = 100L // ~10 Hz
    }

    var isActive: Boolean = false
        private set

    private var sensorManager: SensorManager? = null
    private var locationManager: LocationManager? = null
    private var gnssCallback: GnssStatus.Callback? = null

    // Latest sensor values (×1000 for e3 format)
    private var accelX: Int? = null
    private var accelY: Int? = null
    private var accelZ: Int? = null
    private var gyroRx: Int? = null
    private var gyroRy: Int? = null
    private var gyroRz: Int? = null
    private var compassBearing: Int? = null
    private var compassPitch: Int? = null
    private var compassRoll: Int? = null
    private var satInUse: Int? = null
    private var satInView: Int? = null
    private var satellites: List<ControlMessage.SatelliteInfo>? = null

    // For compass computation from magnetic + accel
    private val gravity = FloatArray(3)
    private val geomagnetic = FloatArray(3)
    private var hasGravity = false
    private var hasMagnetic = false

    private var lastSendTime = 0L

    fun start() {
        if (isActive) return

        sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val sm = sensorManager ?: run {
            Log.w(TAG, "SensorManager unavailable")
            return
        }

        // Register accelerometer
        sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { sensor ->
            sm.registerListener(this, sensor, SensorManager.SENSOR_DELAY_UI)
        } ?: DiagnosticLog.i("imu", "Accelerometer not available")

        // Register gyroscope
        sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let { sensor ->
            sm.registerListener(this, sensor, SensorManager.SENSOR_DELAY_UI)
        } ?: DiagnosticLog.i("imu", "Gyroscope not available")

        // Register magnetic field (for compass computation)
        sm.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)?.let { sensor ->
            sm.registerListener(this, sensor, SensorManager.SENSOR_DELAY_UI)
        } ?: DiagnosticLog.i("imu", "Magnetic field sensor not available")

        // Register GNSS satellite status
        try {
            locationManager = context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager
            gnssCallback = object : GnssStatus.Callback() {
                override fun onSatelliteStatusChanged(status: GnssStatus) {
                    satInView = status.satelliteCount
                    var inUse = 0
                    val sats = mutableListOf<ControlMessage.SatelliteInfo>()
                    for (i in 0 until status.satelliteCount) {
                        if (status.usedInFix(i)) inUse++
                        sats.add(ControlMessage.SatelliteInfo(
                            prn = status.getSvid(i),
                            snrE3 = (status.getCn0DbHz(i) * 1000).toInt(),
                            usedInFix = status.usedInFix(i),
                            azimuthE3 = (status.getAzimuthDegrees(i) * 1000).toInt(),
                            elevationE3 = (status.getElevationDegrees(i) * 1000).toInt()
                        ))
                    }
                    satInUse = inUse
                    satellites = sats
                }
            }
            locationManager?.registerGnssStatusCallback(gnssCallback!!, null)
        } catch (e: SecurityException) {
            DiagnosticLog.w("imu", "GNSS status: permission denied")
        } catch (e: Exception) {
            DiagnosticLog.w("imu", "GNSS status: ${e.message}")
        }

        isActive = true
        Log.i(TAG, "IMU forwarding started")
    }

    fun stop() {
        if (!isActive) return
        sensorManager?.unregisterListener(this)
        gnssCallback?.let { locationManager?.unregisterGnssStatusCallback(it) }
        sensorManager = null
        locationManager = null
        gnssCallback = null
        isActive = false
        Log.i(TAG, "IMU forwarding stopped")
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                // m/s² × 1000 → e3
                accelX = (event.values[0] * 1000).toInt()
                accelY = (event.values[1] * 1000).toInt()
                accelZ = (event.values[2] * 1000).toInt()
                gravity[0] = event.values[0]
                gravity[1] = event.values[1]
                gravity[2] = event.values[2]
                hasGravity = true
                computeCompassIfReady()
            }
            Sensor.TYPE_GYROSCOPE -> {
                // rad/s × 1000 → e3
                gyroRx = (event.values[0] * 1000).toInt()
                gyroRy = (event.values[1] * 1000).toInt()
                gyroRz = (event.values[2] * 1000).toInt()
            }
            Sensor.TYPE_MAGNETIC_FIELD -> {
                geomagnetic[0] = event.values[0]
                geomagnetic[1] = event.values[1]
                geomagnetic[2] = event.values[2]
                hasMagnetic = true
                computeCompassIfReady()
            }
        }
        throttledSend()
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    private fun computeCompassIfReady() {
        if (!hasGravity || !hasMagnetic) return
        val r = FloatArray(9)
        val i = FloatArray(9)
        if (SensorManager.getRotationMatrix(r, i, gravity, geomagnetic)) {
            val orientation = FloatArray(3)
            SensorManager.getOrientation(r, orientation)
            // orientation[0] = azimuth (rad), [1] = pitch, [2] = roll
            compassBearing = (Math.toDegrees(orientation[0].toDouble()) * 1_000_000).toInt()
            compassPitch = (Math.toDegrees(orientation[1].toDouble()) * 1_000_000).toInt()
            compassRoll = (Math.toDegrees(orientation[2].toDouble()) * 1_000_000).toInt()
        }
    }

    @Synchronized
    private fun throttledSend() {
        val now = System.currentTimeMillis()
        if (now - lastSendTime < SEND_INTERVAL_MS) return
        lastSendTime = now

        sendMessage(
            ControlMessage.VehicleData(
                accelXe3 = accelX,
                accelYe3 = accelY,
                accelZe3 = accelZ,
                gyroRxe3 = gyroRx,
                gyroRye3 = gyroRy,
                gyroRze3 = gyroRz,
                compassBearingE6 = compassBearing,
                compassPitchE6 = compassPitch,
                compassRollE6 = compassRoll,
                satInUse = satInUse,
                satInView = satInView,
                satellites = satellites
            )
        )
    }
}
