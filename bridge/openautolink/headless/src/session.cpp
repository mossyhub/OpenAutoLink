#include "openautolink/session.hpp"

#include <memory>
#include <sstream>
#include <utility>

#ifdef PI_AA_ENABLE_AASDK_LIVE
#include "openautolink/live_session.hpp"
#endif

namespace openautolink {

namespace {

constexpr int kCommandWifiConnect = 1002;
constexpr int kCommandFrame = 12;
constexpr int kCommandRequestVideoFocus = 500;
constexpr int kCommandReleaseVideoFocus = 501;
constexpr int kCommandRequestAudioFocusDuck = 504;
constexpr int kCommandReleaseAudioFocus = 505;

std::string assistant_session_state_name(
    bool voice_session_started,
    bool voice_session_stopped,
    const AasdkControlSnapshot& control_snapshot
)
{
    if(control_snapshot.shutdown_response_sent) {
        return "disconnected";
    }

    if(control_snapshot.shutdown_request_pending) {
        return "shutdown_pending";
    }

    if(voice_session_stopped) {
        return "stopped";
    }

    if(voice_session_started) {
        return "active";
    }

    return "idle";
}

std::string primary_focus_state_name(const AasdkControlSnapshot& control_snapshot)
{
    if(control_snapshot.shutdown_response_sent) {
        return "disconnected";
    }

    if(control_snapshot.audio_focus_response_count > 0) {
        return "granted";
    }

    if(
        control_snapshot.audio_focus_request_pending
        || control_snapshot.audio_focus_request_count > 0
    ) {
        return "pending";
    }

    return "idle";
}

std::string primary_playback_state_name(
    const BackendState& state,
    const AasdkControlSnapshot& control_snapshot
)
{
    if(control_snapshot.shutdown_response_sent) {
        return "disconnected";
    }

    if(control_snapshot.audio_focus_response_count <= 0) {
        return "waiting_focus";
    }

    if(state.playback_paused) {
        return "paused";
    }

    return "streaming";
}

std::string navigation_focus_state_name(const AasdkControlSnapshot& control_snapshot)
{
    if(control_snapshot.shutdown_response_sent) {
        return "disconnected";
    }

    if(control_snapshot.navigation_focus_response_count > 0) {
        return "granted";
    }

    if(
        control_snapshot.navigation_focus_request_pending
        || control_snapshot.navigation_focus_request_count > 0
    ) {
        return "pending";
    }

    return "idle";
}

std::string navigation_session_state_name(bool focus_release_pending)
{
    if(focus_release_pending) {
        return "releasing";
    }

    return "active";
}

constexpr int kCommandRequestNaviFocus = 506;
constexpr int kCommandReleaseNaviFocus = 507;
constexpr int kCommandRequestNaviScreenFocus = 508;
constexpr int kCommandReleaseNaviScreenFocus = 509;
constexpr int kCommandAudioTransferOn = 22;
constexpr int kCommandAudioTransferOff = 23;
constexpr int kCommandStartGnssReport = 18;
constexpr int kCommandStopGnssReport = 19;
constexpr int kCommandStartRecordAudio = 1;
constexpr int kCommandStopRecordAudio = 2;
constexpr int kAudioMediaStart = 1;
constexpr int kAudioOutputStart = 5;
constexpr int kAudioInputConfig = 3;
constexpr int kAudioNaviStart = 6;
constexpr int kAudioNaviStop = 7;
constexpr int kAudioSiriStart = 8;
constexpr int kAudioSiriStop = 9;
constexpr int kAudioNaviComplete = 16;

struct OpenPayloadConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;
};

std::optional<OpenPayloadConfig> parse_open_payload(const ParsedInputMessage& message)
{
    if(!message.payload_b64.has_value()) {
        return std::nullopt;
    }

    const auto payload = base64_decode(*message.payload_b64);
    if(payload.size() < 28) {
        return std::nullopt;
    }

    auto read_u32 = [&payload](std::size_t offset) -> int {
        const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data() + offset);
        return static_cast<int>(
            (static_cast<unsigned int>(bytes[0]))
            | (static_cast<unsigned int>(bytes[1]) << 8U)
            | (static_cast<unsigned int>(bytes[2]) << 16U)
            | (static_cast<unsigned int>(bytes[3]) << 24U)
        );
    };

    OpenPayloadConfig config;
    config.width = read_u32(0);
    config.height = read_u32(4);
    config.fps = read_u32(8);
    return config;
}

std::string build_nav_media_payload_json(
    int heartbeat_tick,
    int nav_session_start_count,
    int route_update_count,
    bool focus_release_pending,
    const AasdkControlSnapshot& control_snapshot
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"turnDistanceMeters\":" << (120 - ((heartbeat_tick % 3) * 10)) << ','
            << "\"turnSide\":\"left\"," 
            << "\"roadName\":\"Placeholder Route\"," 
            << "\"maneuver\":\"turn-left\"," 
            << "\"NavigationSessionState\":\""
            << navigation_session_state_name(focus_release_pending)
            << "\"," 
            << "\"NavigationFocusState\":\""
            << navigation_focus_state_name(control_snapshot)
            << "\"," 
            << "\"PromptSequence\":" << nav_session_start_count << ','
            << "\"RouteUpdateCount\":" << route_update_count << ','
            << "\"FocusReleasePending\":" << (focus_release_pending ? "true" : "false")
            << '}';
    return payload.str();
}

std::string build_touch_feedback_payload_json(
    int touch_count,
    int touch_bytes,
    std::string_view touch_action,
    bool nav_refresh_requested,
    bool playback_paused,
    bool primary_video_focus_active
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"touchEventCount\":" << touch_count << ','
            << "\"touchBytes\":" << touch_bytes << ','
            << "\"touchAction\":\"" << touch_action << "\"," 
            << "\"navRefreshRequested\":" << (nav_refresh_requested ? "true" : "false") << ','
            << "\"playbackPaused\":" << (playback_paused ? "true" : "false") << ','
            << "\"primaryVideoFocusActive\":" << (primary_video_focus_active ? "true" : "false")
            << '}';
    return payload.str();
}

std::string extract_gnss_sentence(std::string_view raw_payload)
{
    if(raw_payload.empty()) {
        return "";
    }

    if(raw_payload.size() >= 4) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(raw_payload.data());
        const auto sentence_size = static_cast<std::size_t>(
            static_cast<unsigned int>(bytes[0])
            | (static_cast<unsigned int>(bytes[1]) << 8U)
            | (static_cast<unsigned int>(bytes[2]) << 16U)
            | (static_cast<unsigned int>(bytes[3]) << 24U)
        );
        if(sentence_size > 0 && raw_payload.size() >= 4 + sentence_size) {
            return std::string(raw_payload.substr(4, sentence_size));
        }
    }

    return std::string(raw_payload);
}

std::string build_gnss_feedback_payload_json(int gnss_count, int gnss_bytes, std::string_view gnss_sentence)
{
    std::ostringstream payload;
    payload << '{'
            << "\"navRouteUpdate\":" << gnss_count << ','
            << "\"navVideoVariant\":" << gnss_count << ','
            << "\"gnssBytes\":" << gnss_bytes << ','
            << "\"gnssSentence\":\"" << json_escape(gnss_sentence) << "\"," 
            << "\"routeSource\":\"gnss_placeholder\""
            << '}';
    return payload.str();
}

std::string build_main_media_payload_json(
    const BackendState& state,
    int route_update_count,
    const AasdkControlSnapshot& control_snapshot
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"MediaSongName\":\"Placeholder Drive\"," 
            << "\"MediaArtistName\":\"OpenAutoLink\"," 
            << "\"MediaAlbumName\":\"Aasdk Placeholder Session\"," 
            << "\"MediaAPPName\":\"OpenAuto Headless\"," 
            << "\"MediaSongPlayTime\":" << (state.heartbeat_tick * 1000) << ','
            << "\"MediaPlayStatus\":" << (state.playback_paused ? 0 : 1) << ','
            << "\"PlaybackState\":\""
            << primary_playback_state_name(state, control_snapshot)
            << "\"," 
            << "\"PrimaryFocusState\":\""
            << primary_focus_state_name(control_snapshot)
            << "\"," 
            << "\"TouchCount\":" << state.touch_count << ','
            << "\"GnssCount\":" << state.gnss_count << ','
            << "\"RouteUpdateCount\":" << route_update_count << ','
            << "\"NegotiatedVideoWidth\":" << state.video_width << ','
            << "\"NegotiatedVideoHeight\":" << state.video_height << ','
            << "\"NegotiatedVideoFps\":" << state.video_fps << ','
            << "\"RouteSource\":\"gnss_placeholder\""
            << '}';
    return payload.str();
}

std::string build_main_route_feedback_payload_json(int route_update_count)
{
    std::ostringstream payload;
    payload << '{'
            << "\"mainRouteUpdate\":" << route_update_count << ','
            << "\"mainVideoVariant\":" << route_update_count << ','
            << "\"routeSource\":\"gnss_placeholder\""
            << '}';
    return payload.str();
}

std::string build_input_binding_payload_json(int touch_count)
{
    std::ostringstream payload;
    payload << '{'
            << "\"BindingType\":\"touchscreen\"," 
            << "\"BindingStatus\":\"acknowledged\"," 
            << "\"ScanCodes\":[1,2,3],"
            << "\"TouchCount\":" << touch_count << ','
            << "\"Source\":\"input_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_input_event_payload_json(
    int touch_count,
    int touch_bytes,
    std::string_view touch_action,
    bool playback_paused,
    bool primary_video_focus_active
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"touch\"," 
            << "\"TouchEventCount\":" << touch_count << ','
            << "\"TouchBytes\":" << touch_bytes << ','
            << "\"TouchAction\":\"" << touch_action << "\"," 
            << "\"PlaybackPaused\":" << (playback_paused ? "true" : "false") << ','
            << "\"PrimaryVideoFocusActive\":" << (primary_video_focus_active ? "true" : "false") << ','
            << "\"Source\":\"input_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_sensor_bootstrap_payload_json(int gnss_count)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EnabledSensors\":[\"driving_status\",\"night_mode\"],"
            << "\"DrivingStatus\":\"unrestricted\"," 
            << "\"NightMode\":false,"
            << "\"GnssCount\":" << gnss_count << ','
            << "\"Source\":\"sensor_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_sensor_event_payload_json(
    int gnss_count,
    int gnss_uplink_bytes,
    int last_gnss_input_bytes,
    std::string_view gnss_sentence,
    bool gnss_reporting_enabled
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"gnss_uplink\"," 
            << "\"GnssEventCount\":" << gnss_count << ','
            << "\"GnssUplinkBytes\":" << gnss_uplink_bytes << ','
            << "\"LastGnssInputBytes\":" << last_gnss_input_bytes << ','
            << "\"GnssSentence\":\"" << json_escape(gnss_sentence) << "\"," 
            << "\"GnssReportingEnabled\":" << (gnss_reporting_enabled ? "true" : "false") << ','
            << "\"Source\":\"sensor_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_audio_input_service_payload_json(
    int microphone_uplink_count,
    int microphone_uplink_bytes,
    int last_audio_input_bytes,
    std::string_view assistant_session_state
)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"microphone_uplink\"," 
            << "\"MicrophoneUplinkCount\":" << microphone_uplink_count << ','
            << "\"MicrophoneUplinkBytes\":" << microphone_uplink_bytes << ','
            << "\"LastAudioInputBytes\":" << last_audio_input_bytes << ','
            << "\"AssistantSessionState\":\"" << assistant_session_state << "\"," 
            << "\"Source\":\"audio_input_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_bootstrap_payload_json()
{
    std::ostringstream payload;
    payload << '{'
            << "\"ServiceName\":\"OpenAuto Bluetooth Service\"," 
            << "\"ServiceDescription\":\"AndroidAuto WiFi projection automatic setup\"," 
            << "\"ServiceProvider\":\"f1xstudio.com\"," 
            << "\"ServiceUuid\":\"4de17a00-52cb-11e6-bdf4-0800200c9a66\"," 
            << "\"RfcommPort\":3,"
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_runtime_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"service_ready\"," 
            << "\"BluetoothState\":\"listening\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_pairing_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"pairing_requested\"," 
            << "\"BluetoothState\":\"pairing_pending\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"PairingTransport\":\"bluetooth_placeholder\"," 
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_transport_ready_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"transport_ready\"," 
            << "\"BluetoothState\":\"transport_ready\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"TransportMode\":\"wireless_projection_placeholder\"," 
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_session_established_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"session_established\"," 
            << "\"BluetoothState\":\"connected\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"ProjectionSessionState\":\"established\"," 
            << "\"TransportMode\":\"wireless_projection_placeholder\"," 
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_transport_lost_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"transport_lost\"," 
            << "\"BluetoothState\":\"recovery_pending\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"ProjectionSessionState\":\"lost\"," 
            << "\"RecoveryAction\":\"reconnect_requested\"," 
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_bluetooth_reconnect_established_payload_json(std::string_view control_phase)
{
    std::ostringstream payload;
    payload << '{'
            << "\"EventType\":\"reconnect_established\"," 
            << "\"BluetoothState\":\"connected\"," 
            << "\"ConnectionType\":\"rfcomm\"," 
            << "\"RfcommPort\":3,"
            << "\"WirelessSetupSupported\":true,"
            << "\"ProjectionSessionState\":\"recovered\"," 
            << "\"RecoveryResult\":\"reconnected\"," 
            << "\"TransportMode\":\"wireless_projection_placeholder\"," 
            << "\"ControlPhase\":\"" << control_phase << "\"," 
            << "\"Source\":\"bluetooth_service_placeholder\""
            << '}';
    return payload.str();
}

std::string build_placeholder_media_message(
    const BackendState& state,
    int heartbeat_tick,
    int touch_count,
    int touch_uplink_bytes,
    int last_touch_input_bytes,
    int microphone_uplink_count,
    int microphone_uplink_bytes,
    int last_audio_input_bytes,
    int gnss_count,
    int gnss_uplink_bytes,
    int last_gnss_input_bytes,
    std::string_view last_gnss_sentence,
    bool voice_session_started,
    bool voice_session_stopped,
    int assistant_session_start_count,
    int assistant_session_stop_count,
    const AasdkControlSnapshot& control_snapshot
)
{
    std::ostringstream payload;
    payload
        << "{\"type\":\"media\",\"media_type\":901,\"payload\":{"
        << "\"SessionImplementation\":\"aasdk_control_adapter\","
        << "\"SessionMode\":\"aasdk_placeholder\","
        << "\"Status\":\"awaiting_transport_wiring\","
        << "\"AssistantSessionState\":\""
        << assistant_session_state_name(voice_session_started, voice_session_stopped, control_snapshot)
        << "\","
        << "\"AssistantSessionStartCount\":" << assistant_session_start_count << ','
        << "\"AssistantSessionStopCount\":" << assistant_session_stop_count << ','
        << "\"ControlPhase\":\"" << phase_name(control_snapshot.phase) << "\","
        << "\"ControlHeartbeatCount\":" << control_snapshot.heartbeat_count << ','
        << "\"ControlLastOutbound\":\"" << control_message_name(control_snapshot.last_outbound) << "\","
        << "\"ControlLastInbound\":\"" << control_message_name(control_snapshot.last_inbound) << "\","
        << "\"ControlNextExpectedInbound\":\"" << control_message_name(control_snapshot.next_expected_inbound) << "\","
        << "\"ControlPingRoundtripCount\":" << control_snapshot.ping_roundtrip_count << ','
        << "\"ControlHandshakeRoundtripCount\":" << control_snapshot.handshake_roundtrip_count << ','
        << "\"ControlAudioFocusRequestCount\":" << control_snapshot.audio_focus_request_count << ','
        << "\"ControlAudioFocusResponseCount\":" << control_snapshot.audio_focus_response_count << ','
        << "\"ControlNavigationFocusRequestCount\":" << control_snapshot.navigation_focus_request_count << ','
        << "\"ControlNavigationFocusResponseCount\":" << control_snapshot.navigation_focus_response_count << ','
        << "\"ControlShutdownRequestCount\":" << control_snapshot.shutdown_request_count << ','
        << "\"ControlShutdownResponseCount\":" << control_snapshot.shutdown_response_count << ','
        << "\"ControlAuthCompleteSent\":" << (control_snapshot.auth_complete_sent ? "true" : "false") << ','
        << "\"ControlServiceDiscoverySent\":" << (control_snapshot.service_discovery_sent ? "true" : "false") << ','
        << "\"ControlAudioFocusRequestPending\":" << (control_snapshot.audio_focus_request_pending ? "true" : "false") << ','
        << "\"ControlAudioFocusResponseSent\":" << (control_snapshot.audio_focus_response_sent ? "true" : "false") << ','
        << "\"ControlNavigationFocusRequestPending\":" << (control_snapshot.navigation_focus_request_pending ? "true" : "false") << ','
        << "\"ControlNavigationFocusResponseSent\":" << (control_snapshot.navigation_focus_response_sent ? "true" : "false") << ','
        << "\"ControlShutdownRequestPending\":" << (control_snapshot.shutdown_request_pending ? "true" : "false") << ','
        << "\"ControlShutdownResponseSent\":" << (control_snapshot.shutdown_response_sent ? "true" : "false") << ','
        << "\"LocalAasdkSupportEnabled\":" << (control_snapshot.local_aasdk_support_enabled ? "true" : "false") << ','
        << "\"LocalAasdkControlProtoEnabled\":" << (control_snapshot.local_aasdk_control_proto_enabled ? "true" : "false") << ','
        << "\"ControlAuthCompleteProtoSize\":" << control_snapshot.auth_complete_proto_size << ','
        << "\"ControlAudioFocusResponseProtoSize\":" << control_snapshot.audio_focus_response_proto_size << ','
        << "\"ControlNavigationFocusResponseProtoSize\":" << control_snapshot.navigation_focus_response_proto_size << ','
        << "\"ControlPingRequestProtoSize\":" << control_snapshot.ping_request_proto_size << ','
        << "\"ControlShutdownRequestProtoSize\":" << control_snapshot.shutdown_request_proto_size << ','
        << "\"ControlShutdownResponseProtoSize\":" << control_snapshot.shutdown_response_proto_size << ','
        << "\"HeartbeatCount\":" << heartbeat_tick << ','
        << "\"TouchCount\":" << touch_count << ','
        << "\"TouchUplinkBytes\":" << touch_uplink_bytes << ','
        << "\"LastTouchInputBytes\":" << last_touch_input_bytes << ','
        << "\"MicrophoneUplinkCount\":" << microphone_uplink_count << ','
        << "\"MicrophoneUplinkBytes\":" << microphone_uplink_bytes << ','
        << "\"LastAudioInputBytes\":" << last_audio_input_bytes << ','
        << "\"GnssCount\":" << gnss_count << ','
        << "\"GnssUplinkBytes\":" << gnss_uplink_bytes << ','
        << "\"LastGnssInputBytes\":" << last_gnss_input_bytes << ','
        << "\"LastGnssSentence\":\"" << json_escape(last_gnss_sentence) << "\"," 
        << "\"GnssReportingEnabled\":" << (state.gnss_reporting_enabled ? "true" : "false") << ','
        << "\"MicrophoneCaptureEnabled\":" << (state.microphone_capture_enabled ? "true" : "false")
        << "}}";
    return payload.str();
}

} // namespace

StubAndroidAutoSession::StubAndroidAutoSession(OutputSink sink, std::string phone_name)
    : sink_(std::move(sink))
    , phone_name_(std::move(phone_name))
{
}

void StubAndroidAutoSession::on_host_command(int command_id)
{
    if(command_id == kCommandWifiConnect && !state_.connected) {
        state_.connected = true;
        emit_event("discovery_started");
        emit_event("phone_connected");
        return;
    }

    if(command_id == kCommandFrame) {
        state_.force_idr = true;
    }

    if(command_id == kCommandAudioTransferOff) {
        state_.playback_paused = true;
    }

    if(command_id == kCommandAudioTransferOn) {
        state_.playback_paused = false;
        state_.force_idr = true;
    }

    if(command_id == kCommandStopGnssReport) {
        state_.gnss_reporting_enabled = false;
    }

    if(command_id == kCommandStartGnssReport) {
        state_.gnss_reporting_enabled = true;
    }

    if(command_id == kCommandStopRecordAudio) {
        state_.microphone_capture_enabled = false;
    }

    if(command_id == kCommandStartRecordAudio) {
        state_.microphone_capture_enabled = true;
    }
}

void StubAndroidAutoSession::on_host_open(const ParsedInputMessage& message)
{
    const auto config = parse_open_payload(message);
    if(!config.has_value()) {
        return;
    }

    state_.video_width = config->width;
    state_.video_height = config->height;
    state_.video_fps = config->fps;
}

void StubAndroidAutoSession::on_host_box_settings(const ParsedInputMessage& /* message */)
{
    // Stub: no action needed — config is handled by the bridge Python side
}

void StubAndroidAutoSession::on_host_disconnect()
{
    if(!state_.connected) {
        return;
    }

    emit_event("phone_disconnected");
    state_.connected = false;
    state_.session_active = false;
}

void StubAndroidAutoSession::on_heartbeat()
{
    if(!state_.connected) {
        return;
    }

    ++state_.heartbeat_tick;
    if(!state_.session_active) {
        state_.session_active = true;
        emit_event("session_active");
        emit_audio_command(kAudioMediaStart);
        emit_audio_command(kAudioOutputStart);
    }

    emit(build_media_message(
        state_.heartbeat_tick,
        state_.playback_paused,
        state_.touch_count,
        state_.microphone_uplink_count,
        state_.gnss_count,
        state_.last_gnss_sentence
    ));

    if(!state_.playback_paused) {
        emit(build_audio_message());
        emit(build_video_message(state_.video_pts_ms, state_.force_idr, "VIDEO_DATA", state_.video_width, state_.video_height));
        state_.force_idr = false;
        state_.video_pts_ms += 33;
    }
}

void StubAndroidAutoSession::on_touch(const ParsedInputMessage& message)
{
    ++state_.touch_count;
    state_.last_touch_input_bytes = 0;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_touch_input_bytes = static_cast<int>(payload.size());
        state_.touch_uplink_bytes += state_.last_touch_input_bytes;
    }
    state_.force_idr = true;
}

void StubAndroidAutoSession::on_audio_input(const ParsedInputMessage& message)
{
    if(!state_.microphone_capture_enabled) {
        return;
    }

    ++state_.microphone_uplink_count;
    state_.last_audio_input_bytes = 0;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_audio_input_bytes = static_cast<int>(payload.size());
        state_.microphone_uplink_bytes += state_.last_audio_input_bytes;
    }
}

void StubAndroidAutoSession::on_gnss(const ParsedInputMessage& message)
{
    if(!state_.gnss_reporting_enabled) {
        return;
    }

    ++state_.gnss_count;
    state_.last_gnss_input_bytes = 0;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_gnss_input_bytes = static_cast<int>(payload.size());
        state_.gnss_uplink_bytes += state_.last_gnss_input_bytes;
        state_.last_gnss_sentence = extract_gnss_sentence(payload);
    } else {
        state_.last_gnss_sentence = "$GPGGA";
    }
}

const BackendState& StubAndroidAutoSession::state() const
{
    return state_;
}

void StubAndroidAutoSession::on_vehicle_data(const ParsedInputMessage& /* message */)
{
    // Stub: no sensor channel to forward to
}

void StubAndroidAutoSession::emit(const std::string& payload) const
{
    sink_(payload);
}

void StubAndroidAutoSession::emit_event(const std::string& event_type) const
{
    emit(build_event_message(event_type, phone_name_));
}

void StubAndroidAutoSession::emit_audio_command(int command_id) const
{
    emit(build_audio_command_message(command_id));
}

AasdkAndroidAutoSession::AasdkAndroidAutoSession(OutputSink sink, std::string phone_name)
    : sink_(std::move(sink))
    , phone_name_(std::move(phone_name))
    , control_adapter_(create_aasdk_control_adapter())
{
}

void AasdkAndroidAutoSession::on_host_command(int command_id)
{
    if(command_id == kCommandWifiConnect && !state_.connected) {
        state_.connected = true;
        control_adapter_->on_connect_requested();
        emit_event("discovery_started");
        emit_event("phone_connected");
        flush_ordered_outputs();
        return;
    }

    if(command_id == kCommandFrame) {
        state_.force_idr = true;
    }

    if(command_id == kCommandAudioTransferOff) {
        state_.playback_paused = true;
    }

    if(command_id == kCommandAudioTransferOn) {
        state_.playback_paused = false;
        state_.force_idr = true;
    }

    if(command_id == kCommandStopGnssReport) {
        state_.gnss_reporting_enabled = false;
    }

    if(command_id == kCommandStartGnssReport) {
        state_.gnss_reporting_enabled = true;
    }

    if(command_id == kCommandStopRecordAudio) {
        state_.microphone_capture_enabled = false;
    }

    if(command_id == kCommandStartRecordAudio) {
        state_.microphone_capture_enabled = true;
    }

    if(command_id == kCommandRequestNaviScreenFocus && focus_bootstrap_emitted_ && !nav_content_completed_) {
        nav_focus_ready_ = true;
    }

    if(command_id == kCommandReleaseNaviScreenFocus) {
        nav_focus_ready_ = false;
        if(nav_content_started_ && !nav_content_completed_) {
            pending_nav_release_ = true;
        }
    }
}

void AasdkAndroidAutoSession::on_host_open(const ParsedInputMessage& message)
{
    const auto config = parse_open_payload(message);
    if(!config.has_value()) {
        return;
    }

    state_.video_width = config->width;
    state_.video_height = config->height;
    state_.video_fps = config->fps;
}

void AasdkAndroidAutoSession::on_host_box_settings(const ParsedInputMessage& /* message */)
{
    // Placeholder: no aasdk session to reconfigure
}

void AasdkAndroidAutoSession::on_host_disconnect()
{
    if(!state_.connected) {
        return;
    }

    emit_event("phone_disconnected");
    flush_ordered_outputs();
    state_.connected = false;
    state_.session_active = false;
    shutdown_emitted_ = true;
}

void AasdkAndroidAutoSession::on_heartbeat()
{
    if(!state_.connected) {
        return;
    }

    ++state_.heartbeat_tick;
    control_adapter_->on_heartbeat();
    if(!state_.session_active) {
        state_.session_active = true;
        emit_event("session_active");
    }

    emit_placeholder_status();
    if(maybe_emit_shutdown()) {
        return;
    }
    maybe_emit_service_catalog();
    flush_ordered_outputs();
    maybe_emit_input_sensor_bootstrap();
    if(
        bluetooth_bootstrap_emitted_
        && !bluetooth_runtime_emitted_
        && state_.heartbeat_tick > bluetooth_bootstrap_heartbeat_
    ) {
        enqueue_service_event(
            912,
            build_bluetooth_runtime_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_runtime_emitted_ = true;
        bluetooth_runtime_heartbeat_ = state_.heartbeat_tick;
    }
    if(
        bluetooth_runtime_emitted_
        && !bluetooth_pairing_emitted_
        && state_.heartbeat_tick > bluetooth_runtime_heartbeat_
    ) {
        enqueue_service_event(
            913,
            build_bluetooth_pairing_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_pairing_emitted_ = true;
        bluetooth_pairing_heartbeat_ = state_.heartbeat_tick;
    }
    if(
        bluetooth_pairing_emitted_
        && !bluetooth_transport_ready_emitted_
        && state_.heartbeat_tick > bluetooth_pairing_heartbeat_
    ) {
        enqueue_service_event(
            914,
            build_bluetooth_transport_ready_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_transport_ready_emitted_ = true;
        bluetooth_transport_ready_heartbeat_ = state_.heartbeat_tick;
    }
    if(
        bluetooth_transport_ready_emitted_
        && !bluetooth_session_established_emitted_
        && state_.heartbeat_tick > bluetooth_transport_ready_heartbeat_
    ) {
        enqueue_service_event(
            915,
            build_bluetooth_session_established_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_session_established_emitted_ = true;
        bluetooth_session_established_heartbeat_ = state_.heartbeat_tick;
    }
    if(
        bluetooth_session_established_emitted_
        && !bluetooth_transport_lost_emitted_
        && state_.heartbeat_tick > bluetooth_session_established_heartbeat_
    ) {
        enqueue_service_event(
            916,
            build_bluetooth_transport_lost_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_transport_lost_emitted_ = true;
        bluetooth_transport_lost_heartbeat_ = state_.heartbeat_tick;
    }
    if(
        bluetooth_transport_lost_emitted_
        && !bluetooth_reconnect_established_emitted_
        && state_.heartbeat_tick > bluetooth_transport_lost_heartbeat_
    ) {
        enqueue_service_event(
            917,
            build_bluetooth_reconnect_established_payload_json(phase_name(control_adapter_->snapshot().phase))
        );
        bluetooth_reconnect_established_emitted_ = true;
    }
    flush_service_events();
    maybe_emit_focus_bootstrap();
    maybe_emit_touch_updates();
    maybe_emit_main_stream();
    if(
        control_adapter_->snapshot().navigation_focus_response_count > 0
        && !nav_content_completed_
        && state_.heartbeat_tick > focus_bootstrap_heartbeat_
        && (nav_focus_ready_ || nav_content_started_)
    ) {
        if(!nav_content_started_) {
            enqueue_ordered_output(build_ducking_message(0.2, 0.35, 2));
            enqueue_ordered_output(build_audio_command_message(kAudioNaviStart, 2));
            flush_ordered_outputs();
            ++nav_session_start_count_;
            enqueue_service_event(
                200,
                build_nav_media_payload_json(
                    state_.heartbeat_tick,
                    nav_session_start_count_,
                    state_.gnss_count,
                    pending_nav_release_,
                    control_adapter_->snapshot()
                )
            );
            flush_service_events();
            nav_content_started_ = true;
            nav_content_started_heartbeat_ = state_.heartbeat_tick;
        }

        if(pending_gnss_feedback_) {
            enqueue_service_event(
                904,
                build_gnss_feedback_payload_json(
                    state_.gnss_count,
                    state_.last_gnss_input_bytes,
                    state_.last_gnss_sentence
                )
            );
            enqueue_service_event(
                200,
                build_nav_media_payload_json(
                    state_.heartbeat_tick,
                    nav_session_start_count_,
                    state_.gnss_count,
                    pending_nav_release_,
                    control_adapter_->snapshot()
                )
            );
            flush_service_events();
            pending_gnss_feedback_ = false;
        }

        const bool force_nav_idr = state_.heartbeat_tick == nav_content_started_heartbeat_ || force_next_nav_idr_;
        emit(build_audio_message(2));
        emit(build_video_message(nav_video_pts_ms_, force_nav_idr, "NAVI_VIDEO_DATA", 1200, 500));
        nav_video_pts_ms_ += 33;
        force_next_nav_idr_ = false;

        if(pending_nav_release_ || state_.heartbeat_tick > nav_content_started_heartbeat_) {
            enqueue_ordered_output(build_audio_command_message(kAudioNaviStop, 2));
            enqueue_ordered_output(build_audio_command_message(kAudioNaviComplete, 2));
            enqueue_ordered_output(build_ducking_message(1.0, 0.2, 2));
            flush_ordered_outputs();
            nav_content_completed_ = true;
            nav_focus_ready_ = false;
            pending_nav_release_ = false;
        }
    }
    maybe_emit_focus_release();
    maybe_emit_voice_session();
}

void AasdkAndroidAutoSession::on_touch(const ParsedInputMessage& message)
{
    ++state_.touch_count;
    state_.last_touch_input_bytes = 0;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_touch_input_bytes = static_cast<int>(payload.size());
        state_.touch_uplink_bytes += state_.last_touch_input_bytes;
    }
    pending_touch_feedback_ = true;
    const bool nav_active = nav_content_started_ && !nav_content_completed_;
    const bool can_toggle_primary_playback =
        primary_focus_bootstrap_emitted_
        && !focus_release_emitted_
        && !nav_active
        && !voice_session_started_;

    if(can_toggle_primary_playback) {
        state_.playback_paused = !state_.playback_paused;
        pending_touch_playback_focus_update_ = true;
        if(state_.playback_paused) {
            last_touch_action_ = "pause_playback";
        } else {
            last_touch_action_ = "resume_playback";
            state_.force_idr = true;
        }
        return;
    }

    last_touch_action_ = "nav_refresh";
    force_next_nav_idr_ = true;
}

void AasdkAndroidAutoSession::on_audio_input(const ParsedInputMessage& message)
{
    if(!state_.microphone_capture_enabled) {
        return;
    }

    ++state_.microphone_uplink_count;
    state_.last_audio_input_bytes = 0;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_audio_input_bytes = static_cast<int>(payload.size());
        state_.microphone_uplink_bytes += state_.last_audio_input_bytes;
    }

    if(service_catalog_emitted_) {
        enqueue_service_event(
            910,
            build_audio_input_service_payload_json(
                state_.microphone_uplink_count,
                state_.microphone_uplink_bytes,
                state_.last_audio_input_bytes,
                assistant_session_state_name(voice_session_started_, voice_session_stopped_, control_adapter_->snapshot())
            )
        );
        flush_service_events();
    }

    if(voice_session_started_ && !voice_session_stopped_) {
        enqueue_ordered_output(build_audio_command_message(kAudioSiriStop, 3, 5));
        flush_ordered_outputs();
        voice_session_stopped_ = true;
        ++assistant_session_stop_count_;
        control_adapter_->on_voice_session_stopped();
        emit_placeholder_status();
        maybe_emit_shutdown();
    }
}

void AasdkAndroidAutoSession::on_gnss(const ParsedInputMessage& message)
{
    if(!state_.gnss_reporting_enabled) {
        return;
    }

    ++state_.gnss_count;
    state_.last_gnss_input_bytes = 0;
    pending_gnss_feedback_ = true;
    pending_main_route_feedback_ = true;
    ++main_route_update_count_;
    force_next_nav_idr_ = true;
    state_.force_idr = true;
    if(message.payload_b64.has_value()) {
        const auto payload = base64_decode(*message.payload_b64);
        state_.last_gnss_input_bytes = static_cast<int>(payload.size());
        state_.gnss_uplink_bytes += state_.last_gnss_input_bytes;
        state_.last_gnss_sentence = extract_gnss_sentence(payload);
    }

    if(sensor_bootstrap_emitted_) {
        enqueue_service_event(
            911,
            build_sensor_event_payload_json(
                state_.gnss_count,
                state_.gnss_uplink_bytes,
                state_.last_gnss_input_bytes,
                state_.last_gnss_sentence,
                state_.gnss_reporting_enabled
            )
        );
        flush_service_events();
    }
}

const BackendState& AasdkAndroidAutoSession::state() const
{
    return state_;
}

void AasdkAndroidAutoSession::on_vehicle_data(const ParsedInputMessage& /* message */)
{
    // Aasdk session: no live sensor handler available
}

void AasdkAndroidAutoSession::emit(const std::string& payload) const
{
    sink_(payload);
}

void AasdkAndroidAutoSession::emit_event(const std::string& event_type)
{
    enqueue_ordered_output(build_event_message(event_type, phone_name_));
}

void AasdkAndroidAutoSession::emit_placeholder_status()
{
    const auto control_snapshot = control_adapter_->snapshot();
    enqueue_ordered_output(build_placeholder_media_message(
        state_,
        state_.heartbeat_tick,
        state_.touch_count,
        state_.touch_uplink_bytes,
        state_.last_touch_input_bytes,
        state_.microphone_uplink_count,
        state_.microphone_uplink_bytes,
        state_.last_audio_input_bytes,
        state_.gnss_count,
        state_.gnss_uplink_bytes,
        state_.last_gnss_input_bytes,
        state_.last_gnss_sentence,
        voice_session_started_,
        voice_session_stopped_,
        assistant_session_start_count_,
        assistant_session_stop_count_,
        control_snapshot
    ));
}

void AasdkAndroidAutoSession::enqueue_service_event(int media_type, std::string payload_json)
{
    pending_service_events_.push_back(QueuedServiceEvent{media_type, std::move(payload_json)});
}

void AasdkAndroidAutoSession::flush_service_events()
{
    for(const auto& event : pending_service_events_) {
        emit(build_json_media_message(event.media_type, event.payload_json));
    }
    pending_service_events_.clear();
}

void AasdkAndroidAutoSession::enqueue_ordered_output(std::string payload)
{
    pending_ordered_outputs_.push_back(std::move(payload));
}

void AasdkAndroidAutoSession::flush_ordered_outputs()
{
    for(const auto& payload : pending_ordered_outputs_) {
        emit(payload);
    }
    pending_ordered_outputs_.clear();
}

bool AasdkAndroidAutoSession::maybe_emit_shutdown()
{
    if(shutdown_emitted_) {
        return true;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.shutdown_request_count <= handled_shutdown_request_count_) {
        return false;
    }

    emit_event("phone_disconnected");
    control_adapter_->on_shutdown_response_sent();
    emit_placeholder_status();
    flush_ordered_outputs();
    handled_shutdown_request_count_ = control_snapshot.shutdown_request_count;
    shutdown_emitted_ = true;
    state_.connected = false;
    state_.session_active = false;
    return true;
}

void AasdkAndroidAutoSession::maybe_emit_service_catalog()
{
    if(service_catalog_emitted_) {
        return;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.phase != AasdkControlPhase::Active) {
        return;
    }

    enqueue_ordered_output(build_service_catalog_message(phone_name_));
    service_catalog_emitted_ = true;
}

void AasdkAndroidAutoSession::maybe_emit_input_sensor_bootstrap()
{
    if(!service_catalog_emitted_) {
        return;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.phase != AasdkControlPhase::Active) {
        return;
    }

    if(!input_binding_emitted_) {
        enqueue_service_event(906, build_input_binding_payload_json(state_.touch_count));
        input_binding_emitted_ = true;
    }

    if(!sensor_bootstrap_emitted_) {
        enqueue_service_event(907, build_sensor_bootstrap_payload_json(state_.gnss_count));
        sensor_bootstrap_emitted_ = true;
    }

    if(!bluetooth_bootstrap_emitted_) {
        enqueue_service_event(908, build_bluetooth_bootstrap_payload_json());
        bluetooth_bootstrap_emitted_ = true;
        bluetooth_bootstrap_heartbeat_ = state_.heartbeat_tick;
    }
}

void AasdkAndroidAutoSession::maybe_emit_touch_updates()
{
    if(pending_touch_playback_focus_update_) {
        if(state_.playback_paused) {
            if(primary_video_focus_active_) {
                enqueue_ordered_output(build_command_message(kCommandReleaseVideoFocus));
                primary_video_focus_active_ = false;
            }
        } else {
            if(!primary_video_focus_active_ && !focus_release_emitted_) {
                enqueue_ordered_output(build_command_message(kCommandRequestVideoFocus));
                primary_video_focus_active_ = true;
            }
            state_.force_idr = true;
        }

        flush_ordered_outputs();
        pending_touch_playback_focus_update_ = false;
    }

    if(!pending_touch_feedback_) {
        return;
    }

    const bool nav_refresh_requested = nav_content_started_ && !nav_content_completed_;
    if(input_binding_emitted_) {
        enqueue_service_event(
            909,
            build_input_event_payload_json(
                state_.touch_count,
                state_.last_touch_input_bytes,
                last_touch_action_,
                state_.playback_paused,
                primary_video_focus_active_
            )
        );
        flush_service_events();
    }

    enqueue_service_event(
        903,
        build_touch_feedback_payload_json(
            state_.touch_count,
            state_.last_touch_input_bytes,
            last_touch_action_,
            nav_refresh_requested,
            state_.playback_paused,
            primary_video_focus_active_
        )
    );
    flush_service_events();
    pending_touch_feedback_ = false;
}

void AasdkAndroidAutoSession::maybe_emit_main_stream()
{
    const auto control_snapshot = control_adapter_->snapshot();
    if(
        control_snapshot.phase != AasdkControlPhase::Active
        || control_snapshot.audio_focus_response_count <= 0
    ) {
        return;
    }

    enqueue_service_event(1, build_main_media_payload_json(state_, main_route_update_count_, control_snapshot));

    if(pending_main_route_feedback_) {
        enqueue_service_event(905, build_main_route_feedback_payload_json(main_route_update_count_));
        pending_main_route_feedback_ = false;
    }

    flush_service_events();

    if(!state_.playback_paused) {
        emit(build_audio_message(1));
        emit(build_video_message(state_.video_pts_ms, state_.force_idr, "VIDEO_DATA", state_.video_width, state_.video_height));
        state_.video_pts_ms += 33;
        state_.force_idr = false;
    }
}

void AasdkAndroidAutoSession::maybe_emit_focus_bootstrap()
{
    if(!service_catalog_emitted_) {
        return;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.phase != AasdkControlPhase::Active) {
        return;
    }

    if(
        control_snapshot.audio_focus_request_pending
        && control_snapshot.audio_focus_request_count > handled_audio_focus_request_count_
        && !primary_focus_bootstrap_emitted_
    ) {
        enqueue_ordered_output(build_command_message(kCommandRequestVideoFocus));
        enqueue_ordered_output(build_command_message(kCommandRequestAudioFocusDuck));
        flush_ordered_outputs();
        control_adapter_->on_audio_focus_response_sent();
        handled_audio_focus_request_count_ = control_snapshot.audio_focus_request_count;
        primary_focus_bootstrap_emitted_ = true;
        primary_video_focus_active_ = true;
        return;
    }

    if(
        control_snapshot.navigation_focus_request_pending
        && control_snapshot.navigation_focus_request_count > handled_navigation_focus_request_count_
        && !focus_bootstrap_emitted_
    ) {
        enqueue_ordered_output(build_command_message(kCommandRequestNaviFocus));
        enqueue_ordered_output(build_navi_focus_message(true));
        flush_ordered_outputs();
        control_adapter_->on_navigation_focus_response_sent();
        handled_navigation_focus_request_count_ = control_snapshot.navigation_focus_request_count;
        focus_bootstrap_emitted_ = true;
        focus_bootstrap_heartbeat_ = state_.heartbeat_tick;
    }
}

void AasdkAndroidAutoSession::maybe_emit_focus_release()
{
    if(focus_release_emitted_) {
        return;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.navigation_focus_response_count <= 0) {
        return;
    }

    if(state_.heartbeat_tick <= focus_bootstrap_heartbeat_ || !nav_content_completed_) {
        return;
    }

    if(control_snapshot.phase != AasdkControlPhase::Active) {
        return;
    }

    enqueue_ordered_output(build_command_message(kCommandReleaseVideoFocus));
    enqueue_ordered_output(build_command_message(kCommandReleaseAudioFocus));
    enqueue_ordered_output(build_command_message(kCommandReleaseNaviFocus));
    enqueue_ordered_output(build_navi_focus_message(false));
    flush_ordered_outputs();
    primary_video_focus_active_ = false;
    nav_focus_ready_ = false;
    focus_release_emitted_ = true;
}

void AasdkAndroidAutoSession::maybe_emit_voice_session()
{
    if(voice_session_started_ || !focus_release_emitted_) {
        return;
    }

    const auto control_snapshot = control_adapter_->snapshot();
    if(control_snapshot.phase != AasdkControlPhase::Active) {
        return;
    }

    enqueue_ordered_output(build_audio_command_message(kAudioInputConfig, 3, 5));
    enqueue_ordered_output(build_audio_command_message(kAudioSiriStart, 3, 5));
    flush_ordered_outputs();
    voice_session_started_ = true;
    ++assistant_session_start_count_;
    emit_placeholder_status();
}

std::unique_ptr<IAndroidAutoSession> create_session(SessionMode mode, OutputSink sink, std::string phone_name)
{
    switch(mode) {
    case SessionMode::Stub:
        return std::make_unique<StubAndroidAutoSession>(std::move(sink), std::move(phone_name));
    case SessionMode::AasdkPlaceholder:
        return std::make_unique<AasdkAndroidAutoSession>(std::move(sink), std::move(phone_name));
    case SessionMode::AasdkLive:
#ifdef PI_AA_ENABLE_AASDK_LIVE
        // When created via the generic factory without config, use defaults.
        // For full configuration, use create_live_session() directly.
        {
            HeadlessConfig config;
            return create_live_session(std::move(sink), std::move(phone_name), std::move(config));
        }
#else
        // Fall through to stub if live mode was not compiled in.
        return std::make_unique<StubAndroidAutoSession>(std::move(sink), std::move(phone_name));
#endif
    }

    return std::make_unique<StubAndroidAutoSession>(std::move(sink), std::move(phone_name));
}

std::optional<SessionMode> parse_session_mode(std::string_view value)
{
    if(value == "stub") {
        return SessionMode::Stub;
    }
    if(value == "aasdk-placeholder") {
        return SessionMode::AasdkPlaceholder;
    }
    if(value == "aasdk-live") {
        return SessionMode::AasdkLive;
    }
    if(value == "oal-mock") {
        return SessionMode::OalMock;
    }
    if(value == "carplay-live") {
        return SessionMode::CarPlayLive;
    }
    if(value == "auto") {
        return SessionMode::Auto;
    }
    return std::nullopt;
}

} // namespace openautolink
