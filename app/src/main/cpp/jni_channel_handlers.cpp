/*
 * jni_channel_handlers.cpp — Handler implementations for all aasdk channels.
 *
 * Each handler: receives on its channel, responds to AA protocol messages,
 * and dispatches events to JniSession for JNI callbacks to Kotlin.
 */
#include "jni_channel_handlers.h"
#include "jni_session.h"

#include <android/log.h>

#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorRequest.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationStatus.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnEvent.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnDistanceEvent.pb.h>
#include <aap_protobuf/service/mediaplayback/message/MediaPlaybackMetadata.pb.h>
#include <aap_protobuf/service/mediaplayback/message/MediaPlaybackStatus.pb.h>
#include <aap_protobuf/service/phonestatus/message/PhoneStatus.pb.h>
#include <aap_protobuf/service/bluetooth/message/BluetoothPairingRequest.pb.h>
#include <aap_protobuf/service/bluetooth/message/BluetoothPairingResponse.pb.h>
#include <aap_protobuf/service/bluetooth/message/BluetoothAuthenticationData.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

#define LOG_TAG "OAL-Handlers"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace openautolink::jni {

// ============================================================================
// Audio Sink Handler
// ============================================================================

JniAudioSinkHandler::JniAudioSinkHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::mediasink::audio::AudioMediaSinkService> channel,
    JniSession& session,
    AudioType type)
    : strand_(strand), channel_(std::move(channel)), session_(session), type_(type)
{
}

void JniAudioSinkHandler::start()
{
    channel_->receive(shared_from_this());
}

int JniAudioSinkHandler::purposeFromType() const
{
    switch (type_) {
        case AudioType::Media:    return 0;
        case AudioType::Guidance: return 1;
        case AudioType::System:   return 2;
        case AudioType::Telephony: return 4;
    }
    return 0;
}

void JniAudioSinkHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Audio channel open (type=%d)", static_cast<int>(type_));
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniAudioSinkHandler::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup& /*request*/)
{
    LOGI("Audio setup (type=%d)", static_cast<int>(type_));
    aap_protobuf::service::media::shared::message::Config config;
    config.set_status(aap_protobuf::shared::STATUS_OK);
    config.set_max_unacked(30);
    config.add_configuration_indices(0);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelSetupResponse(config, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniAudioSinkHandler::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start& /*indication*/)
{
    LOGI("Audio start (type=%d)", static_cast<int>(type_));
    channel_->receive(shared_from_this());
}

void JniAudioSinkHandler::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop& /*indication*/)
{
    LOGI("Audio stop (type=%d)", static_cast<int>(type_));
    channel_->receive(shared_from_this());
}

void JniAudioSinkHandler::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType /*timestamp*/,
    const aasdk::common::DataConstBuffer& buffer)
{
    // ACK immediately for flow control
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(0);
    ack.set_value(1);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const auto&) {});
    channel_->sendMediaAckIndication(ack, std::move(promise));

    // Dispatch to Kotlin via JniSession
    int purpose = purposeFromType();
    int sampleRate = (type_ == AudioType::Media) ? 48000 : 16000;
    int channels = (type_ == AudioType::Media) ? 2 : 1;
    session_.dispatchAudioFrame(buffer.cdata, buffer.size, purpose, sampleRate, channels);

    channel_->receive(shared_from_this());
}

void JniAudioSinkHandler::onMediaIndication(const aasdk::common::DataConstBuffer& buffer)
{
    onMediaWithTimestampIndication(0, buffer);
}

void JniAudioSinkHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Audio channel error (type=%d): %s", static_cast<int>(type_), e.what());
}

// ============================================================================
// Sensor Source Handler
// ============================================================================

JniSensorHandler::JniSensorHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::sensorsource::SensorSourceService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniSensorHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniSensorHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Sensor channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniSensorHandler::onSensorStartRequest(
    const aap_protobuf::service::sensorsource::message::SensorRequest& request)
{
    LOGI("Sensor start request: type=%d", request.type());

    // Respond with OK — phone expects acknowledgement before sending sensor polls
    aap_protobuf::service::sensorsource::message::SensorStartResponseMessage response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendSensorStartResponse(response, std::move(promise));

    channel_->receive(shared_from_this());
}

void JniSensorHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Sensor channel error: %s", e.what());
}

// ============================================================================
// Input Source Handler
// ============================================================================

JniInputHandler::JniInputHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::inputsource::InputSourceService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniInputHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniInputHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Input channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniInputHandler::onKeyBindingRequest(
    const aap_protobuf::service::media::sink::message::KeyBindingRequest& /*request*/)
{
    LOGI("Key binding request");
    channel_->receive(shared_from_this());
}

void JniInputHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Input channel error: %s", e.what());
}

// ============================================================================
// Navigation Status Handler
// ============================================================================

JniNavStatusHandler::JniNavStatusHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::navigationstatus::NavigationStatusService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniNavStatusHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniNavStatusHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Nav status channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniNavStatusHandler::onStatusUpdate(
    const aap_protobuf::service::navigationstatus::message::NavigationStatus& navStatus)
{
    int status = navStatus.status();
    LOGI("Nav status: %d", status);
    session_.dispatchNavStatus(status);
    channel_->receive(shared_from_this());
}

void JniNavStatusHandler::onTurnEvent(
    const aap_protobuf::service::navigationstatus::message::NavigationNextTurnEvent& turnEvent)
{
    std::string road = turnEvent.road();

    // Map turn event to maneuver string (same logic as bridge)
    std::string maneuver = "unknown";
    if (turnEvent.has_event()) {
        auto ev = turnEvent.event();
        std::string side;
        if (turnEvent.has_turn_side()) {
            switch (turnEvent.turn_side()) {
                case 1: side = "_left"; break;
                case 2: side = "_right"; break;
                default: break;
            }
        }
        switch (ev) {
            case 0: maneuver = "unknown"; break;
            case 1: maneuver = "depart"; break;
            case 2: maneuver = "name_change"; break;
            case 3: maneuver = "slight_turn" + side; break;
            case 4: maneuver = "turn" + side; break;
            case 5: maneuver = "sharp_turn" + side; break;
            case 6: maneuver = "u_turn" + side; break;
            case 7: maneuver = "on_ramp" + side; break;
            case 8: maneuver = "off_ramp" + side; break;
            case 9: maneuver = "fork" + side; break;
            case 10: maneuver = "merge" + side; break;
            case 11: maneuver = "roundabout_enter"; break;
            case 12: maneuver = "roundabout_exit"; break;
            case 13: maneuver = "roundabout_enter_and_exit"; break;
            case 14: maneuver = "straight"; break;
            case 19: maneuver = "destination"; break;
            default: maneuver = "turn_" + std::to_string(ev) + side; break;
        }
    }

    // Extract icon PNG if present
    const uint8_t* iconData = nullptr;
    size_t iconSize = 0;
    if (turnEvent.has_image() && !turnEvent.image().empty()) {
        iconData = reinterpret_cast<const uint8_t*>(turnEvent.image().data());
        iconSize = turnEvent.image().size();
    }

    LOGI("Nav turn: %s road=%s icon=%zu", maneuver.c_str(), road.c_str(), iconSize);
    session_.dispatchNavTurn(maneuver, road, iconData, iconSize);

    channel_->receive(shared_from_this());
}

void JniNavStatusHandler::onDistanceEvent(
    const aap_protobuf::service::navigationstatus::message::NavigationNextTurnDistanceEvent& distanceEvent)
{
    int distanceMeters = distanceEvent.distance_meters();
    int etaSeconds = distanceEvent.time_to_turn_seconds();

    std::string displayDistance;
    std::string displayUnit;
    if (distanceEvent.has_display_value()) {
        displayDistance = distanceEvent.display_value();
    }
    if (distanceEvent.has_display_units()) {
        displayUnit = distanceEvent.display_units();
    }

    session_.dispatchNavDistance(distanceMeters, etaSeconds, displayDistance, displayUnit);
    channel_->receive(shared_from_this());
}

void JniNavStatusHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Nav channel error: %s", e.what());
}

// ============================================================================
// Microphone Handler
// ============================================================================

JniMicHandler::JniMicHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::mediasource::MediaSourceService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniMicHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniMicHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Mic channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniMicHandler::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup& /*request*/)
{
    LOGI("Mic setup");
    aap_protobuf::service::media::shared::message::Config config;
    config.set_status(aap_protobuf::shared::STATUS_OK);
    config.set_max_unacked(30);
    config.add_configuration_indices(0);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelSetupResponse(config, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniMicHandler::onMediaSourceOpenRequest(
    const aap_protobuf::service::media::source::message::MicrophoneRequest& request)
{
    bool wantOpen = request.open();
    LOGI("Mic %s", wantOpen ? "open" : "close");
    open_ = wantOpen;
    session_.dispatchMicRequest(wantOpen);
    channel_->receive(shared_from_this());
}

void JniMicHandler::onMediaChannelAckIndication(
    const aap_protobuf::service::media::source::message::Ack& /*indication*/)
{
    channel_->receive(shared_from_this());
}

void JniMicHandler::feedAudio(const uint8_t* data, size_t size)
{
    if (!open_) return;

    aasdk::common::Data audioData(data, data + size);
    auto ts = static_cast<aasdk::messenger::Timestamp::ValueType>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const auto&) {});
    channel_->sendMediaSourceWithTimestampIndication(ts, audioData, std::move(promise));
}

void JniMicHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Mic channel error: %s", e.what());
}

// ============================================================================
// Media Playback Status Handler
// ============================================================================

JniMediaStatusHandler::JniMediaStatusHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::mediaplaybackstatus::MediaPlaybackStatusService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniMediaStatusHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniMediaStatusHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Media status channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniMediaStatusHandler::onMetadataUpdate(
    const aap_protobuf::service::mediaplayback::message::MediaPlaybackMetadata& metadata)
{
    std::string title = metadata.has_track_name() ? metadata.track_name() : "";
    std::string artist = metadata.has_artist_name() ? metadata.artist_name() : "";
    std::string album = metadata.has_album_name() ? metadata.album_name() : "";

    const uint8_t* artData = nullptr;
    size_t artSize = 0;
    if (metadata.has_album_art() && !metadata.album_art().empty()) {
        artData = reinterpret_cast<const uint8_t*>(metadata.album_art().data());
        artSize = metadata.album_art().size();
    }

    session_.dispatchMediaMetadata(title, artist, album, artData, artSize);
    channel_->receive(shared_from_this());
}

void JniMediaStatusHandler::onPlaybackUpdate(
    const aap_protobuf::service::mediaplayback::message::MediaPlaybackStatus& playback)
{
    int state = playback.has_state() ? static_cast<int>(playback.state()) : 0;
    long long positionMs = playback.has_media_position_ms() ? playback.media_position_ms() : 0;
    session_.dispatchMediaPlayback(state, positionMs);
    channel_->receive(shared_from_this());
}

void JniMediaStatusHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Media status channel error: %s", e.what());
}

// ============================================================================
// Phone Status Handler
// ============================================================================

JniPhoneStatusHandler::JniPhoneStatusHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::phonestatus::PhoneStatusService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniPhoneStatusHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniPhoneStatusHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Phone status channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniPhoneStatusHandler::onPhoneStatusUpdate(
    const aap_protobuf::service::phonestatus::message::PhoneStatus& status)
{
    int signal = status.has_signal_strength() ? status.signal_strength() : -1;
    int callState = status.has_call_state() ? static_cast<int>(status.call_state()) : 0;
    session_.dispatchPhoneStatus(signal, callState);
    channel_->receive(shared_from_this());
}

void JniPhoneStatusHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Phone status channel error: %s", e.what());
}

// ============================================================================
// Bluetooth Handler
// ============================================================================

JniBluetoothHandler::JniBluetoothHandler(
    boost::asio::io_service::strand& strand,
    std::shared_ptr<aasdk::channel::bluetooth::BluetoothService> channel,
    JniSession& session)
    : strand_(strand), channel_(std::move(channel)), session_(session)
{
}

void JniBluetoothHandler::start()
{
    channel_->receive(shared_from_this());
}

void JniBluetoothHandler::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Bluetooth channel open");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(shared_from_this());
}

void JniBluetoothHandler::onBluetoothPairingRequest(
    const aap_protobuf::service::bluetooth::message::BluetoothPairingRequest& /*request*/)
{
    LOGI("Bluetooth pairing request");
    // In JNI mode, BT pairing is handled by Nearby — just acknowledge
    channel_->receive(shared_from_this());
}

void JniBluetoothHandler::onBluetoothAuthenticationResult(
    const aap_protobuf::service::bluetooth::message::BluetoothAuthenticationResult& /*request*/)
{
    LOGI("Bluetooth auth result");
    channel_->receive(shared_from_this());
}

void JniBluetoothHandler::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Bluetooth channel error: %s", e.what());
}

} // namespace openautolink::jni
