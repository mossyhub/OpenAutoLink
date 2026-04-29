package com.openautolink.app.navigation

import com.openautolink.app.transport.ControlMessage
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Processes nav_state control messages from the bridge and maintains
 * the current maneuver state for display in UI or cluster service.
 */
class NavigationDisplayImpl : NavigationDisplay {

    private val _currentManeuver = MutableStateFlow<ManeuverState?>(null)
    override val currentManeuver: StateFlow<ManeuverState?> = _currentManeuver.asStateFlow()

    override fun onNavState(state: ControlMessage.NavState) {
        // Turn events and distance events arrive as separate JNI callbacks with
        // partial fields.  Merge incoming fields onto the previous ManeuverState
        // so the cluster (and UI) always sees a complete snapshot.
        val prev = _currentManeuver.value

        val type = if (state.maneuver != null) ManeuverMapper.fromWire(state.maneuver) else prev?.type ?: ManeuverType.UNKNOWN
        val distanceMeters = state.distanceMeters ?: prev?.distanceMeters
        val road = state.road ?: prev?.roadName
        val etaSeconds = state.etaSeconds ?: prev?.etaSeconds
        val navImage = state.navImageBase64 ?: prev?.navImageBase64
        val cue = state.cue ?: prev?.cue
        val roundaboutExit = state.roundaboutExitNumber ?: prev?.roundaboutExitNumber
        val currentRoad = state.currentRoad ?: prev?.currentRoad
        val destination = state.destination ?: prev?.destination
        val etaFormatted = state.etaFormatted ?: prev?.etaFormatted
        val displayDistance = state.displayDistance ?: prev?.displayDistance
        val displayDistanceUnit = state.displayDistanceUnit ?: prev?.displayDistanceUnit
        val timeToArrival = state.timeToArrivalSeconds ?: prev?.timeToArrivalSeconds
        val destDistMeters = state.destDistanceMeters ?: prev?.destDistanceMeters
        val destDistDisplay = state.destDistanceDisplay ?: prev?.destDistanceDisplay
        val destDistUnit = state.destDistanceUnit ?: prev?.destDistanceUnit

        // Use pre-formatted distance from bridge if available, else format locally
        val formattedDist = if (!displayDistance.isNullOrEmpty() && !displayDistanceUnit.isNullOrEmpty()) {
            "$displayDistance ${DistanceFormatter.unitLabel(displayDistanceUnit)}"
        } else {
            DistanceFormatter.format(distanceMeters)
        }

        val lanes = state.lanes?.map { lane ->
            LaneInfo(lane.directions.map { dir ->
                LaneDirectionInfo(dir.shape, dir.highlighted)
            })
        } ?: prev?.lanes

        _currentManeuver.value = ManeuverState(
            type = type,
            distanceMeters = distanceMeters,
            formattedDistance = formattedDist,
            roadName = road,
            etaSeconds = etaSeconds,
            navImageBase64 = navImage,
            lanes = lanes,
            cue = cue,
            roundaboutExitNumber = roundaboutExit,
            currentRoad = currentRoad,
            destination = destination,
            etaFormatted = etaFormatted,
            displayDistance = displayDistance,
            displayDistanceUnit = displayDistanceUnit,
            timeToArrivalSeconds = timeToArrival,
            destDistanceMeters = destDistMeters,
            destDistanceDisplay = destDistDisplay,
            destDistanceUnit = destDistUnit
        )
    }

    override fun clear() {
        _currentManeuver.value = null
    }
}
