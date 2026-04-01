#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace openautolink {

enum class AasdkControlPhase {
    Idle,
    TransportReady,
    VersionSent,
    HandshakePending,
    ServiceDiscoveryPending,
    Active,
};

enum class AasdkControlMessage {
    None,
    VersionRequest,
    VersionResponse,
    SslHandshake,
    AuthComplete,
    ServiceDiscoveryRequest,
    ServiceDiscoveryResponse,
    AudioFocusRequest,
    AudioFocusResponse,
    NavigationFocusRequest,
    NavigationFocusResponse,
    PingRequest,
    PingResponse,
    ShutdownRequest,
    ShutdownResponse,
};

struct AasdkControlSnapshot {
    AasdkControlPhase phase = AasdkControlPhase::Idle;
    int heartbeat_count = 0;
    int ping_roundtrip_count = 0;
    int handshake_roundtrip_count = 0;
    int audio_focus_request_count = 0;
    int audio_focus_response_count = 0;
    int navigation_focus_request_count = 0;
    int navigation_focus_response_count = 0;
    int shutdown_request_count = 0;
    int shutdown_response_count = 0;
    int auth_complete_proto_size = 0;
    int audio_focus_response_proto_size = 0;
    int navigation_focus_response_proto_size = 0;
    int ping_request_proto_size = 0;
    int shutdown_request_proto_size = 0;
    int shutdown_response_proto_size = 0;
    bool auth_complete_sent = false;
    bool service_discovery_sent = false;
    bool audio_focus_request_pending = false;
    bool audio_focus_response_sent = false;
    bool navigation_focus_request_pending = false;
    bool navigation_focus_response_sent = false;
    bool shutdown_request_pending = false;
    bool shutdown_response_sent = false;
    bool local_aasdk_support_enabled = false;
    bool local_aasdk_control_proto_enabled = false;
    AasdkControlMessage last_outbound = AasdkControlMessage::None;
    AasdkControlMessage last_inbound = AasdkControlMessage::None;
    AasdkControlMessage next_expected_inbound = AasdkControlMessage::None;
};

class IAasdkControlAdapter {
public:
    virtual ~IAasdkControlAdapter() = default;

    virtual void on_connect_requested() = 0;
    virtual void on_heartbeat() = 0;
    virtual void on_version_response_received(std::uint16_t major_code, std::uint16_t minor_code) = 0;
    virtual void on_handshake_received() = 0;
    virtual void on_service_discovery_request_received() = 0;
    virtual void on_audio_focus_request_received() = 0;
    virtual void on_navigation_focus_request_received() = 0;
    virtual void on_ping_response_received() = 0;
    virtual void on_shutdown_request_received() = 0;
    virtual void on_audio_focus_response_sent() = 0;
    virtual void on_navigation_focus_response_sent() = 0;
    virtual void on_voice_session_stopped() = 0;
    virtual void on_shutdown_response_sent() = 0;
    virtual AasdkControlSnapshot snapshot() const = 0;
};

class PlaceholderAasdkControlAdapter final : public IAasdkControlAdapter {
public:
    void on_connect_requested();
    void on_heartbeat() override;
    void on_version_response_received(std::uint16_t major_code, std::uint16_t minor_code) override;
    void on_handshake_received() override;
    void on_service_discovery_request_received() override;
    void on_audio_focus_request_received() override;
    void on_navigation_focus_request_received() override;
    void on_ping_response_received() override;
    void on_shutdown_request_received() override;
    void on_audio_focus_response_sent() override;
    void on_navigation_focus_response_sent() override;
    void on_voice_session_stopped() override;
    void on_shutdown_response_sent() override;

    AasdkControlSnapshot snapshot() const override;

private:
    AasdkControlSnapshot snapshot_;
};

std::unique_ptr<IAasdkControlAdapter> create_aasdk_control_adapter();

AasdkControlPhase phase_from_snapshot(const AasdkControlSnapshot& snapshot);
std::string phase_name(AasdkControlPhase phase);
std::string control_message_name(AasdkControlMessage message);

} // namespace openautolink
