package com.openautolink.app.input

import android.content.Context
import android.util.Log
import com.openautolink.app.diagnostics.DiagnosticLog
import com.openautolink.app.transport.ControlMessage
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.ConcurrentHashMap

/**
 * Monitors VHAL properties via the AAOS Car API using reflection.
 * Gracefully degrades when android.car is unavailable (e.g., on non-automotive devices).
 *
 * Monitored properties include speed, gear, parking brake, night mode, turn signals,
 * battery level, fuel level, odometer, ambient temperature, and more.
 *
 * Sends batched VehicleData control messages to the bridge at a throttled rate
 * to avoid flooding the control channel.
 */
class VehicleDataForwarderImpl(
    private val context: Context,
    private val sendMessage: (ControlMessage.VehicleData) -> Unit
) : VehicleDataForwarder {

    companion object {
        private const val TAG = "VehicleDataForwarder"

        // Minimum interval between vehicle data sends (ms)
        private const val SEND_INTERVAL_MS = 500L

        // VHAL property IDs — only properties confirmed available on 2024 Blazer EV.
        // Properties that are "not exposed by this vehicle / HAL" (odometer) or
        // "permission not granted" (steering, lights, doors, HVAC) are excluded.
        private const val PERF_VEHICLE_SPEED = 291504647        // float, m/s
        private const val GEAR_SELECTION = 289408000             // int, gear enum
        private const val PARKING_BRAKE_ON = 287310850           // boolean
        private const val NIGHT_MODE = 287310855                 // boolean
        private const val EV_BATTERY_LEVEL = 291504905           // float, Wh
        private const val ENV_OUTSIDE_TEMPERATURE = 291505923    // float, celsius
        private const val EV_BATTERY_INSTANTANEOUS_CHARGE_RATE = 291504908 // float, W
        private const val RANGE_REMAINING = 291504904            // float, meters
        private const val PERF_ENGINE_RPM = 291504901            // float, RPM (ICE only, likely unavailable on EV)
    }

    override var isActive: Boolean = false
        private set

    // Car API objects obtained via reflection
    private var carObject: Any? = null
    private var propertyManager: Any? = null
    private val registeredCallbacks = mutableListOf<Any>()

    // Latest values — updated by property callbacks, sent as batch
    private val currentValues = ConcurrentHashMap<Int, Any>()
    private var lastSendTime = 0L

    private val _latestVehicleData = MutableStateFlow(ControlMessage.VehicleData())
    override val latestVehicleData: StateFlow<ControlMessage.VehicleData> = _latestVehicleData.asStateFlow()

    override fun start() {
        if (isActive) return

        try {
            connectToCar()
            registerProperties()
            isActive = true
            Log.i(TAG, "Vehicle data forwarding started")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to start vehicle data forwarding: ${e.message}")
            DiagnosticLog.w("vhal", "Failed to start vehicle data forwarding: ${e.message}")
            cleanup()
        }
    }

    override fun stop() {
        if (!isActive) return
        cleanup()
        isActive = false
        Log.i(TAG, "Vehicle data forwarding stopped")
    }

    private fun connectToCar() {
        // Use reflection to access android.car.Car
        val carClass = Class.forName("android.car.Car")
        val createMethod = carClass.getMethod(
            "createCar",
            Context::class.java,
            android.os.Handler::class.java
        )
        carObject = createMethod.invoke(null, context, null)
            ?: throw IllegalStateException("Car.createCar returned null")

        // Get CarPropertyManager
        val getManagerMethod = carClass.getMethod("getCarManager", String::class.java)
        val propertyServiceField = carClass.getField("PROPERTY_SERVICE")
        val propertyServiceName = propertyServiceField.get(null) as String
        propertyManager = getManagerMethod.invoke(carObject, propertyServiceName)
            ?: throw IllegalStateException("CarPropertyManager is null")

        Log.i(TAG, "Connected to Car API via reflection")
    }

    private fun registerProperties() {
        val pm = propertyManager ?: return

        val propertyIds = listOf(
            PERF_VEHICLE_SPEED,
            GEAR_SELECTION,
            PARKING_BRAKE_ON,
            NIGHT_MODE,
            EV_BATTERY_LEVEL,
            ENV_OUTSIDE_TEMPERATURE,
            EV_BATTERY_INSTANTANEOUS_CHARGE_RATE,
            RANGE_REMAINING,
            PERF_ENGINE_RPM
        )

        val pmClass = pm::class.java

        // Check property availability and register
        val isAvailableMethod = try {
            pmClass.getMethod("isPropertyAvailable", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
        } catch (_: NoSuchMethodException) {
            null
        }

        for (propId in propertyIds) {
            try {
                // Check if property is available (area 0 = global)
                val available = isAvailableMethod?.invoke(pm, propId, 0) as? Boolean ?: true
                if (!available) {
                    Log.d(TAG, "Property $propId not available, skipping")
                    DiagnosticLog.i("vhal", "Property $propId not available, skipping")
                    continue
                }

                registerPropertyCallback(pm, propId)
            } catch (e: Exception) {
                Log.d(TAG, "Cannot register property $propId: ${e.message}")
                DiagnosticLog.w("vhal", "Cannot register property $propId: ${e.message}")
            }
        }
    }

    private fun registerPropertyCallback(pm: Any, propertyId: Int) {
        val pmClass = pm::class.java

        // Create a CarPropertyEventCallback via dynamic proxy
        val callbackInterface = Class.forName("android.car.hardware.property.CarPropertyManager\$CarPropertyEventCallback")

        val callback = java.lang.reflect.Proxy.newProxyInstance(
            callbackInterface.classLoader,
            arrayOf(callbackInterface)
        ) { _, method, args ->
            when (method.name) {
                "onChangeEvent" -> {
                    args?.firstOrNull()?.let { event ->
                        handlePropertyChange(propertyId, event)
                    }
                }
                "onErrorEvent" -> {
                    Log.w(TAG, "Property error for $propertyId")
                }
                "toString" -> "VehicleDataCallback($propertyId)"
                "hashCode" -> propertyId
                "equals" -> this === args?.firstOrNull()
                else -> null
            }
            null
        }

        // Register callback: registerCallback(callback, propertyId, rate)
        // rate = SENSOR_RATE_ONCHANGE (0.0f) for most, SENSOR_RATE_NORMAL (1.0f) for continuous
        val rate = when (propertyId) {
            PERF_VEHICLE_SPEED -> 1.0f // SENSOR_RATE_NORMAL
            else -> 0.0f // SENSOR_RATE_ONCHANGE
        }

        val registerMethod = pmClass.getMethod(
            "registerCallback",
            callbackInterface,
            Int::class.javaPrimitiveType,
            Float::class.javaPrimitiveType
        )
        registerMethod.invoke(pm, callback, propertyId, rate)
        registeredCallbacks.add(callback)
    }

    private fun handlePropertyChange(propertyId: Int, event: Any) {
        try {
            val valueMethod = event::class.java.getMethod("getValue")
            val value = valueMethod.invoke(event) ?: return

            currentValues[propertyId] = value
            throttledSend()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to extract property value for $propertyId: ${e.message}")
        }
    }

    @Synchronized
    private fun throttledSend() {
        val now = System.currentTimeMillis()
        if (now - lastSendTime < SEND_INTERVAL_MS) return
        lastSendTime = now

        val data = buildVehicleData()
        _latestVehicleData.value = data
        sendMessage(data)
    }

    private fun buildVehicleData(): ControlMessage.VehicleData {
        val speed = (currentValues[PERF_VEHICLE_SPEED] as? Float)?.let { it * 3.6f } // m/s → km/h
        val gearInt = currentValues[GEAR_SELECTION] as? Int
        val gear = gearInt?.let { gearToString(it) }
        val parkingBrake = currentValues[PARKING_BRAKE_ON] as? Boolean
        val nightMode = currentValues[NIGHT_MODE] as? Boolean

        // EV battery level in Wh — send raw value, bridge handles conversion
        val batteryPct = (currentValues[EV_BATTERY_LEVEL] as? Float)?.toInt()

        val ambientTemp = currentValues[ENV_OUTSIDE_TEMPERATURE] as? Float
        val chargeRate = currentValues[EV_BATTERY_INSTANTANEOUS_CHARGE_RATE] as? Float
        val rangeRemaining = (currentValues[RANGE_REMAINING] as? Float)?.let { it / 1000f } // m → km
        val rpmRaw = currentValues[PERF_ENGINE_RPM] as? Float
        val rpmE3 = rpmRaw?.let { (it * 1000).toInt() }  // RPM × 1000

        return ControlMessage.VehicleData(
            speedKmh = speed,
            gear = gear,
            batteryPct = batteryPct,
            turnSignal = null,
            parkingBrake = parkingBrake,
            nightMode = nightMode,
            fuelLevelPct = null,
            rangeKm = rangeRemaining,
            lowFuel = null,
            odometerKm = null,
            ambientTempC = ambientTemp,
            steeringAngleDeg = null,
            headlight = null,
            hazardLights = null,
            rpmE3 = rpmE3
        )
    }

    private fun gearToString(gear: Int): String = when (gear) {
        0 -> "Unknown"
        1 -> "N"     // GEAR_NEUTRAL
        2 -> "R"     // GEAR_REVERSE
        4 -> "P"     // GEAR_PARK
        8 -> "D"     // GEAR_DRIVE
        16 -> "1"    // GEAR_1
        32 -> "2"    // GEAR_2
        64 -> "3"    // GEAR_3
        128 -> "4"   // GEAR_4
        else -> "D"
    }

    private fun turnSignalToString(signal: Int): String = when (signal) {
        0 -> "none"
        1 -> "right"
        2 -> "left"
        else -> "none"
    }

    private fun cleanup() {
        val pm = propertyManager
        if (pm != null) {
            val pmClass = pm::class.java
            val callbackInterface = try {
                Class.forName("android.car.hardware.property.CarPropertyManager\$CarPropertyEventCallback")
            } catch (_: ClassNotFoundException) {
                null
            }

            if (callbackInterface != null) {
                val unregisterMethod = try {
                    pmClass.getMethod("unregisterCallback", callbackInterface)
                } catch (_: NoSuchMethodException) {
                    null
                }

                for (callback in registeredCallbacks) {
                    try {
                        unregisterMethod?.invoke(pm, callback)
                    } catch (e: Exception) {
                        Log.w(TAG, "Failed to unregister callback: ${e.message}")
                    }
                }
            }
        }
        registeredCallbacks.clear()
        currentValues.clear()

        // Disconnect car
        try {
            carObject?.let { car ->
                val disconnectMethod = car::class.java.getMethod("disconnect")
                disconnectMethod.invoke(car)
            }
        } catch (e: Exception) {
            Log.d(TAG, "Car disconnect: ${e.message}")
        }
        carObject = null
        propertyManager = null
    }
}
