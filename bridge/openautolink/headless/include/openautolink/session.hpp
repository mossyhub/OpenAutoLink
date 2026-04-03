#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "openautolink/aasdk_control.hpp"
#include "openautolink/contract.hpp"
#include "openautolink/service_catalog.hpp"

namespace openautolink {

struct BackendState {
    bool connected = false;
    bool session_active = false;
    bool force_idr = true;
    bool playback_paused = false;
    int heartbeat_tick = 0;
    int video_pts_ms = 0;
    int touch_count = 0;
    int touch_uplink_bytes = 0;
    int last_touch_input_bytes = 0;
    int microphone_uplink_count = 0;
    int microphone_uplink_bytes = 0;
    int last_audio_input_bytes = 0;
    int gnss_count = 0;
    int gnss_uplink_bytes = 0;
    int last_gnss_input_bytes = 0;
    int video_width = 1920;
    int video_height = 1080;
    int video_fps = 30;
    bool gnss_reporting_enabled = true;
    bool microphone_capture_enabled = true;
    std::string last_gnss_sentence;
};

using OutputSink = std::function<void(const std::string&)>;

enum class SessionMode {
    Stub,
    AasdkPlaceholder,
    AasdkLive,
    OalMock,       // Synthetic OAL data — no phone/aasdk needed
    CarPlayLive,   // CarPlay only — iPhone via RTSP/AirPlay
    Auto,          // Both AA + CarPlay, first phone to connect wins
};

class IAndroidAutoSession {
public:
    virtual ~IAndroidAutoSession() = default;

    virtual void on_host_command(int command_id) = 0;
    virtual void on_host_open(const ParsedInputMessage& message) = 0;
    virtual void on_host_box_settings(const ParsedInputMessage& message) = 0;
    virtual void on_host_disconnect() = 0;
    virtual void on_heartbeat() = 0;
    virtual void on_touch(const ParsedInputMessage& message) = 0;
    virtual void on_audio_input(const ParsedInputMessage& message) = 0;
    virtual void on_gnss(const ParsedInputMessage& message) = 0;
    virtual void on_vehicle_data(const ParsedInputMessage& message) = 0;
    virtual void replay_cached_keyframe() {}  // Override in live session
    virtual const BackendState& state() const = 0;
};

class StubAndroidAutoSession final : public IAndroidAutoSession {
public:
    explicit StubAndroidAutoSession(OutputSink sink, std::string phone_name = "OpenAuto Headless");

    void on_host_command(int command_id) override;
    void on_host_open(const ParsedInputMessage& message) override;
    void on_host_box_settings(const ParsedInputMessage& message) override;
    void on_host_disconnect() override;
    void on_heartbeat() override;
    void on_touch(const ParsedInputMessage& message) override;
    void on_audio_input(const ParsedInputMessage& message) override;
    void on_gnss(const ParsedInputMessage& message) override;
    void on_vehicle_data(const ParsedInputMessage& message) override;
    const BackendState& state() const override;

private:
    void emit(const std::string& payload) const;
    void emit_event(const std::string& event_type) const;
    void emit_audio_command(int command_id) const;

    OutputSink sink_;
    std::string phone_name_;
    BackendState state_;
};

class AasdkAndroidAutoSession final : public IAndroidAutoSession {
public:
    explicit AasdkAndroidAutoSession(OutputSink sink, std::string phone_name = "OpenAuto Headless");

    void on_host_command(int command_id) override;
    void on_host_open(const ParsedInputMessage& message) override;
    void on_host_box_settings(const ParsedInputMessage& message) override;
    void on_host_disconnect() override;
    void on_heartbeat() override;
    void on_touch(const ParsedInputMessage& message) override;
    void on_audio_input(const ParsedInputMessage& message) override;
    void on_gnss(const ParsedInputMessage& message) override;
    void on_vehicle_data(const ParsedInputMessage& message) override;
    const BackendState& state() const override;

private:
    struct QueuedServiceEvent {
        int media_type;
        std::string payload_json;
    };

    using QueuedOutput = std::string;

    void emit(const std::string& payload) const;
    void emit_event(const std::string& event_type);
    void emit_placeholder_status();
    void enqueue_service_event(int media_type, std::string payload_json);
    void flush_service_events();
    void enqueue_ordered_output(std::string payload);
    void flush_ordered_outputs();
    bool maybe_emit_shutdown();
    void maybe_emit_service_catalog();
    void maybe_emit_input_sensor_bootstrap();
    void maybe_emit_touch_updates();
    void maybe_emit_main_stream();
    void maybe_emit_focus_bootstrap();
    void maybe_emit_focus_release();
    void maybe_emit_voice_session();

    OutputSink sink_;
    std::string phone_name_;
    BackendState state_;
    std::unique_ptr<IAasdkControlAdapter> control_adapter_;
    bool service_catalog_emitted_ = false;
    bool input_binding_emitted_ = false;
    bool sensor_bootstrap_emitted_ = false;
    bool bluetooth_bootstrap_emitted_ = false;
    bool bluetooth_runtime_emitted_ = false;
    int bluetooth_bootstrap_heartbeat_ = -1;
    bool bluetooth_pairing_emitted_ = false;
    int bluetooth_runtime_heartbeat_ = -1;
    bool bluetooth_transport_ready_emitted_ = false;
    int bluetooth_pairing_heartbeat_ = -1;
    bool bluetooth_session_established_emitted_ = false;
    int bluetooth_transport_ready_heartbeat_ = -1;
    bool bluetooth_transport_lost_emitted_ = false;
    int bluetooth_session_established_heartbeat_ = -1;
    bool bluetooth_reconnect_established_emitted_ = false;
    int bluetooth_transport_lost_heartbeat_ = -1;
    bool primary_focus_bootstrap_emitted_ = false;
    bool primary_video_focus_active_ = false;
    bool focus_bootstrap_emitted_ = false;
    bool focus_release_emitted_ = false;
    int handled_audio_focus_request_count_ = 0;
    int handled_navigation_focus_request_count_ = 0;
    int handled_shutdown_request_count_ = 0;
    int focus_bootstrap_heartbeat_ = -1;
    bool nav_content_started_ = false;
    bool nav_content_completed_ = false;
    bool nav_focus_ready_ = false;
    bool pending_nav_release_ = false;
    int nav_content_started_heartbeat_ = -1;
    int nav_video_pts_ms_ = 0;
    bool pending_touch_feedback_ = false;
    bool pending_touch_playback_focus_update_ = false;
    bool pending_gnss_feedback_ = false;
    bool force_next_nav_idr_ = false;
    int nav_session_start_count_ = 0;
    bool pending_main_route_feedback_ = false;
    int main_route_update_count_ = 0;
    std::string last_touch_action_ = "none";
    int assistant_session_start_count_ = 0;
    int assistant_session_stop_count_ = 0;
    bool voice_session_started_ = false;
    bool voice_session_stopped_ = false;
    bool shutdown_emitted_ = false;
    std::vector<QueuedServiceEvent> pending_service_events_;
    std::vector<QueuedOutput> pending_ordered_outputs_;
};

std::unique_ptr<IAndroidAutoSession> create_session(SessionMode mode, OutputSink sink, std::string phone_name = "OpenAuto Headless");
std::optional<SessionMode> parse_session_mode(std::string_view value);

#ifdef PI_AA_ENABLE_AASDK_LIVE
struct HeadlessConfig;
std::unique_ptr<IAndroidAutoSession> create_live_session(OutputSink sink, std::string phone_name, HeadlessConfig config);
#endif

} // namespace openautolink
