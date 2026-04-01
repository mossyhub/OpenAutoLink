#include "openautolink/aasdk_control.hpp"

#if defined(PI_AA_HAVE_LOCAL_AASDK_CONTROL_PROTO)
#include <AuthCompleteIndicationMessage.pb.h>
#include <AudioFocusResponseMessage.pb.h>
#include <AudioFocusStateEnum.pb.h>
#include <NavigationFocusResponseMessage.pb.h>
#include <PingRequestMessage.pb.h>
#include <ShutdownReasonEnum.pb.h>
#include <ShutdownRequestMessage.pb.h>
#include <ShutdownResponseMessage.pb.h>
#endif

#include <utility>

namespace openautolink {

namespace {

#if defined(PI_AA_HAVE_LOCAL_AASDK_SOURCE)
constexpr bool kLocalAasdkSupportEnabled = true;
#else
constexpr bool kLocalAasdkSupportEnabled = false;
#endif

constexpr std::uint16_t kPlaceholderAasdkMajorVersion = 1;
constexpr std::uint16_t kPlaceholderAasdkMinorVersion = 0;

#if defined(PI_AA_HAVE_LOCAL_AASDK_CONTROL_PROTO)
constexpr bool kLocalAasdkControlProtoEnabled = true;

int serialized_proto_size(const google::protobuf::MessageLite& message)
{
    return static_cast<int>(message.ByteSizeLong());
}

void update_auth_complete_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::AuthCompleteIndication indication;
    indication.set_status(aasdk::proto::enums::Status::OK);
    snapshot.auth_complete_proto_size = serialized_proto_size(indication);
}

void update_audio_focus_response_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::AudioFocusResponse response;
    response.set_audio_focus_state(aasdk::proto::enums::AudioFocusState::GAIN_TRANSIENT_GUIDANCE_ONLY);
    snapshot.audio_focus_response_proto_size = serialized_proto_size(response);
}

void update_navigation_focus_response_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::NavigationFocusResponse response;
    response.set_type(1);
    snapshot.navigation_focus_response_proto_size = serialized_proto_size(response);
}

void update_ping_request_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::PingRequest request;
    request.set_timestamp(snapshot.heartbeat_count);
    snapshot.ping_request_proto_size = serialized_proto_size(request);
}

void update_shutdown_request_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::ShutdownRequest request;
    request.set_reason(aasdk::proto::enums::ShutdownReason::QUIT);
    snapshot.shutdown_request_proto_size = serialized_proto_size(request);
}

void update_shutdown_response_proto_size(AasdkControlSnapshot& snapshot)
{
    aasdk::proto::messages::ShutdownResponse response;
    snapshot.shutdown_response_proto_size = serialized_proto_size(response);
}
#else
constexpr bool kLocalAasdkControlProtoEnabled = false;

void update_auth_complete_proto_size(AasdkControlSnapshot&)
{
}

void update_audio_focus_response_proto_size(AasdkControlSnapshot&)
{
}

void update_navigation_focus_response_proto_size(AasdkControlSnapshot&)
{
}

void update_ping_request_proto_size(AasdkControlSnapshot&)
{
}

void update_shutdown_request_proto_size(AasdkControlSnapshot&)
{
}

void update_shutdown_response_proto_size(AasdkControlSnapshot&)
{
}
#endif

} // namespace

void PlaceholderAasdkControlAdapter::on_connect_requested()
{
    snapshot_ = AasdkControlSnapshot{};
    snapshot_.phase = AasdkControlPhase::TransportReady;
    snapshot_.local_aasdk_support_enabled = kLocalAasdkSupportEnabled;
    snapshot_.local_aasdk_control_proto_enabled = kLocalAasdkControlProtoEnabled;
}

void PlaceholderAasdkControlAdapter::on_heartbeat()
{
    ++snapshot_.heartbeat_count;

    switch(snapshot_.phase) {
    case AasdkControlPhase::Idle:
        break;
    case AasdkControlPhase::TransportReady:
        snapshot_.phase = AasdkControlPhase::VersionSent;
        snapshot_.last_outbound = AasdkControlMessage::VersionRequest;
        snapshot_.next_expected_inbound = AasdkControlMessage::VersionResponse;
        break;
    case AasdkControlPhase::VersionSent:
        on_version_response_received(kPlaceholderAasdkMajorVersion, kPlaceholderAasdkMinorVersion);
        break;
    case AasdkControlPhase::HandshakePending:
        on_handshake_received();
        break;
    case AasdkControlPhase::ServiceDiscoveryPending:
        on_service_discovery_request_received();
        break;
    case AasdkControlPhase::Active:
        if(snapshot_.shutdown_request_pending) {
            break;
        }

        if(!snapshot_.audio_focus_response_sent) {
            if(!snapshot_.audio_focus_request_pending) {
                on_audio_focus_request_received();
            }
            break;
        }

        if(!snapshot_.navigation_focus_response_sent) {
            if(!snapshot_.navigation_focus_request_pending) {
                on_navigation_focus_request_received();
            }
            break;
        }

        on_ping_response_received();
        break;
    }
}

void PlaceholderAasdkControlAdapter::on_version_response_received(std::uint16_t major_code, std::uint16_t minor_code)
{
    if(snapshot_.phase != AasdkControlPhase::VersionSent) {
        return;
    }

    (void)major_code;
    (void)minor_code;
    snapshot_.phase = AasdkControlPhase::HandshakePending;
    snapshot_.last_inbound = AasdkControlMessage::VersionResponse;
    snapshot_.last_outbound = AasdkControlMessage::SslHandshake;
    snapshot_.next_expected_inbound = AasdkControlMessage::SslHandshake;
    snapshot_.handshake_roundtrip_count = 1;
}

void PlaceholderAasdkControlAdapter::on_handshake_received()
{
    if(snapshot_.phase != AasdkControlPhase::HandshakePending) {
        return;
    }

    snapshot_.phase = AasdkControlPhase::ServiceDiscoveryPending;
    snapshot_.last_inbound = AasdkControlMessage::SslHandshake;
    snapshot_.last_outbound = AasdkControlMessage::AuthComplete;
    snapshot_.next_expected_inbound = AasdkControlMessage::ServiceDiscoveryRequest;
    snapshot_.auth_complete_sent = true;
    update_auth_complete_proto_size(snapshot_);
}

void PlaceholderAasdkControlAdapter::on_service_discovery_request_received()
{
    if(snapshot_.phase != AasdkControlPhase::ServiceDiscoveryPending) {
        return;
    }

    snapshot_.phase = AasdkControlPhase::Active;
    snapshot_.last_inbound = AasdkControlMessage::ServiceDiscoveryRequest;
    snapshot_.last_outbound = AasdkControlMessage::ServiceDiscoveryResponse;
    snapshot_.next_expected_inbound = AasdkControlMessage::AudioFocusRequest;
    snapshot_.service_discovery_sent = true;
}

void PlaceholderAasdkControlAdapter::on_audio_focus_request_received()
{
    if(snapshot_.phase != AasdkControlPhase::Active) {
        return;
    }

    ++snapshot_.audio_focus_request_count;
    snapshot_.audio_focus_request_pending = true;
    snapshot_.last_inbound = AasdkControlMessage::AudioFocusRequest;
    snapshot_.next_expected_inbound = AasdkControlMessage::AudioFocusRequest;
}

void PlaceholderAasdkControlAdapter::on_navigation_focus_request_received()
{
    if(snapshot_.phase != AasdkControlPhase::Active) {
        return;
    }

    ++snapshot_.navigation_focus_request_count;
    snapshot_.navigation_focus_request_pending = true;
    snapshot_.last_inbound = AasdkControlMessage::NavigationFocusRequest;
    snapshot_.next_expected_inbound = AasdkControlMessage::NavigationFocusRequest;
}

void PlaceholderAasdkControlAdapter::on_ping_response_received()
{
    if(
        snapshot_.phase != AasdkControlPhase::Active
        || snapshot_.shutdown_request_pending
        || snapshot_.audio_focus_request_pending
        || snapshot_.navigation_focus_request_pending
    ) {
        return;
    }

    snapshot_.last_inbound = AasdkControlMessage::PingResponse;
    snapshot_.last_outbound = AasdkControlMessage::PingRequest;
    snapshot_.next_expected_inbound = AasdkControlMessage::PingResponse;
    ++snapshot_.ping_roundtrip_count;
    update_ping_request_proto_size(snapshot_);
}

void PlaceholderAasdkControlAdapter::on_shutdown_request_received()
{
    if(snapshot_.phase != AasdkControlPhase::Active || snapshot_.shutdown_request_pending || snapshot_.shutdown_response_sent) {
        return;
    }

    ++snapshot_.shutdown_request_count;
    snapshot_.shutdown_request_pending = true;
    snapshot_.last_inbound = AasdkControlMessage::ShutdownRequest;
    snapshot_.next_expected_inbound = AasdkControlMessage::None;
    update_shutdown_request_proto_size(snapshot_);
}

void PlaceholderAasdkControlAdapter::on_audio_focus_response_sent()
{
    if(snapshot_.phase != AasdkControlPhase::Active || !snapshot_.audio_focus_request_pending) {
        return;
    }

    snapshot_.audio_focus_request_pending = false;
    ++snapshot_.audio_focus_response_count;
    snapshot_.audio_focus_response_sent = true;
    snapshot_.last_outbound = AasdkControlMessage::AudioFocusResponse;
    update_audio_focus_response_proto_size(snapshot_);
    snapshot_.next_expected_inbound = snapshot_.navigation_focus_request_pending
        ? AasdkControlMessage::NavigationFocusRequest
        : AasdkControlMessage::PingResponse;
}

void PlaceholderAasdkControlAdapter::on_navigation_focus_response_sent()
{
    if(snapshot_.phase != AasdkControlPhase::Active || !snapshot_.navigation_focus_request_pending) {
        return;
    }

    snapshot_.navigation_focus_request_pending = false;
    ++snapshot_.navigation_focus_response_count;
    snapshot_.navigation_focus_response_sent = true;
    snapshot_.last_outbound = AasdkControlMessage::NavigationFocusResponse;
    update_navigation_focus_response_proto_size(snapshot_);
    snapshot_.next_expected_inbound = AasdkControlMessage::PingResponse;
}

void PlaceholderAasdkControlAdapter::on_voice_session_stopped()
{
    on_shutdown_request_received();
}

void PlaceholderAasdkControlAdapter::on_shutdown_response_sent()
{
    if(snapshot_.phase != AasdkControlPhase::Active || !snapshot_.shutdown_request_pending) {
        return;
    }

    snapshot_.shutdown_request_pending = false;
    ++snapshot_.shutdown_response_count;
    snapshot_.shutdown_response_sent = true;
    snapshot_.last_outbound = AasdkControlMessage::ShutdownResponse;
    update_shutdown_response_proto_size(snapshot_);
    snapshot_.next_expected_inbound = AasdkControlMessage::None;
}

AasdkControlSnapshot PlaceholderAasdkControlAdapter::snapshot() const
{
    return snapshot_;
}

std::unique_ptr<IAasdkControlAdapter> create_aasdk_control_adapter()
{
    return std::make_unique<PlaceholderAasdkControlAdapter>();
}

AasdkControlPhase phase_from_snapshot(const AasdkControlSnapshot& snapshot)
{
    return snapshot.phase;
}

std::string phase_name(AasdkControlPhase phase)
{
    switch(phase) {
    case AasdkControlPhase::Idle:
        return "idle";
    case AasdkControlPhase::TransportReady:
        return "transport_ready";
    case AasdkControlPhase::VersionSent:
        return "version_sent";
    case AasdkControlPhase::HandshakePending:
        return "handshake_pending";
    case AasdkControlPhase::ServiceDiscoveryPending:
        return "service_discovery_pending";
    case AasdkControlPhase::Active:
        return "active";
    }

    return "unknown";
}

std::string control_message_name(AasdkControlMessage message)
{
    switch(message) {
    case AasdkControlMessage::None:
        return "none";
    case AasdkControlMessage::VersionRequest:
        return "version_request";
    case AasdkControlMessage::VersionResponse:
        return "version_response";
    case AasdkControlMessage::SslHandshake:
        return "ssl_handshake";
    case AasdkControlMessage::AuthComplete:
        return "auth_complete";
    case AasdkControlMessage::ServiceDiscoveryRequest:
        return "service_discovery_request";
    case AasdkControlMessage::ServiceDiscoveryResponse:
        return "service_discovery_response";
    case AasdkControlMessage::AudioFocusRequest:
        return "audio_focus_request";
    case AasdkControlMessage::AudioFocusResponse:
        return "audio_focus_response";
    case AasdkControlMessage::NavigationFocusRequest:
        return "navigation_focus_request";
    case AasdkControlMessage::NavigationFocusResponse:
        return "navigation_focus_response";
    case AasdkControlMessage::PingRequest:
        return "ping_request";
    case AasdkControlMessage::PingResponse:
        return "ping_response";
    case AasdkControlMessage::ShutdownRequest:
        return "shutdown_request";
    case AasdkControlMessage::ShutdownResponse:
        return "shutdown_response";
    }

    return "unknown";
}

} // namespace openautolink
