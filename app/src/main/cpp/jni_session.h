/*
 * jni_session.h — aasdk session + event handler for JNI.
 *
 * Implements all aasdk channel event handlers and dispatches
 * AA events to Kotlin via JNI callbacks. This is the equivalent
 * of the bridge's HeadlessAutoEntity + handler classes, but in
 * a single class since we dispatch everything through JNI.
 */
#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <string>

#include <jni.h>
#include <boost/asio.hpp>

#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Video/Channel/VideoChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/AudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/MediaAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/GuidanceAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/SystemAudioChannel.hpp>
#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/InputSource/InputSourceService.hpp>
#include <aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp>
#include <aasdk/Channel/Bluetooth/BluetoothService.hpp>
#include <aasdk/Channel/Bluetooth/IBluetoothServiceEventHandler.hpp>
#include <aasdk/Channel/NavigationStatus/NavigationStatusService.hpp>
#include <aasdk/Channel/NavigationStatus/INavigationStatusServiceEventHandler.hpp>
#include <aasdk/Channel/MediaPlaybackStatus/MediaPlaybackStatusService.hpp>
#include <aasdk/Channel/MediaPlaybackStatus/IMediaPlaybackStatusServiceEventHandler.hpp>
#include <aasdk/Channel/PhoneStatus/PhoneStatusService.hpp>
#include <aasdk/Channel/PhoneStatus/IPhoneStatusServiceEventHandler.hpp>

namespace openautolink::jni {

class JniTransport;

/**
 * Owns the aasdk session lifecycle and implements all channel event handlers.
 * Dispatches AA events to Kotlin via JNI callbacks.
 *
 * Implements:
 * - IControlServiceChannelEventHandler (version, handshake, SDR, ping, bye)
 * - IVideoMediaSinkServiceEventHandler (video frames)
 * - IAudioMediaSinkServiceEventHandler (audio frames — 3 instances)
 * - IMediaSourceServiceEventHandler (mic open/close)
 * - ISensorSourceServiceEventHandler (sensor requests)
 * - IInputSourceServiceEventHandler (key bindings)
 * - IBluetoothServiceEventHandler (BT pairing)
 * - INavigationStatusServiceEventHandler (nav state)
 * - IMediaPlaybackStatusServiceEventHandler (media metadata)
 * - IPhoneStatusServiceEventHandler (signal/call state)
 *
 * Lifecycle:
 *   create() → start(transport_pipe) → [streaming] → stop() → destroy()
 */
class JniSession
    : public aasdk::channel::control::IControlServiceChannelEventHandler
    , public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler
    , public std::enable_shared_from_this<JniSession>
{
public:
    explicit JniSession(JavaVM* jvm);
    ~JniSession();

    /**
     * Start AA session with a connected transport pipe.
     * @param env JNI environment
     * @param transportPipe Kotlin AasdkTransportPipe (has readBytes/writeBytes)
     * @param callback Kotlin AasdkSessionCallback (receives events)
     * @param sdrConfig Kotlin AasdkSdrConfig (service discovery params)
     */
    void start(JNIEnv* env, jobject transportPipe, jobject callback, jobject sdrConfig);

    /** Stop the session gracefully (sends ByeBye). */
    void stop();

    /** Send touch event to phone. */
    void sendTouchEvent(int action, int pointerId, float x, float y, int pointerCount);

    /** Send key event to phone. */
    void sendKeyEvent(int keyCode, bool isDown);

    /** Send GPS location to phone. */
    void sendGpsLocation(double lat, double lon, double alt,
                         float speed, float bearing, long long timestampMs);

    /** Send vehicle sensor data to phone. */
    void sendVehicleSensor(int sensorType, const uint8_t* data, size_t length);

    /** Send microphone audio data to phone. */
    void sendMicAudio(const uint8_t* data, size_t length);

    /** Request a video keyframe (IDR). */
    void requestKeyframe();

    /** Is session actively streaming? */
    bool isStreaming() const { return streaming_; }

    // ---- IControlServiceChannelEventHandler ----
    void onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                           aap_protobuf::shared::MessageStatus status) override;
    void onHandshake(const aasdk::common::DataConstBuffer& payload) override;
    void onServiceDiscoveryRequest(
        const aap_protobuf::service::control::message::ServiceDiscoveryRequest& request) override;
    void onAudioFocusRequest(
        const aap_protobuf::service::control::message::AudioFocusRequest& request) override;
    void onByeByeRequest(
        const aap_protobuf::service::control::message::ByeByeRequest& request) override;
    void onByeByeResponse(
        const aap_protobuf::service::control::message::ByeByeResponse& response) override;
    void onBatteryStatusNotification(
        const aap_protobuf::service::control::message::BatteryStatusNotification& notification) override;
    void onNavigationFocusRequest(
        const aap_protobuf::service::control::message::NavFocusRequestNotification& request) override;
    void onVoiceSessionRequest(
        const aap_protobuf::service::control::message::VoiceSessionNotification& request) override;
    void onPingRequest(
        const aap_protobuf::service::control::message::PingRequest& request) override;
    void onPingResponse(
        const aap_protobuf::service::control::message::PingResponse& response) override;
    void onChannelError(const aasdk::error::Error& e) override;

    // ---- IVideoMediaSinkServiceEventHandler ----
    void onChannelOpenRequest(
        const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
    void onMediaChannelSetupRequest(
        const aap_protobuf::service::media::shared::message::Setup& request) override;
    void onMediaChannelStartIndication(
        const aap_protobuf::service::media::shared::message::Start& indication) override;
    void onMediaChannelStopIndication(
        const aap_protobuf::service::media::shared::message::Stop& indication) override;
    void onMediaWithTimestampIndication(
        aasdk::messenger::Timestamp::ValueType timestamp,
        const aasdk::common::DataConstBuffer& buffer) override;
    void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override;
    void onVideoFocusRequest(
        const aap_protobuf::service::media::video::message::VideoFocusRequestNotification& request) override;

private:
    void ioServiceThreadFunc();
    void buildServiceDiscoveryResponse(
        aap_protobuf::service::control::message::ServiceDiscoveryResponse& response);
    void startAllHandlers();

    // JNI helpers
    JNIEnv* getEnv(bool& attached);
    void releaseEnv(bool attached);
    void callVoidCallback(jmethodID method);

    JavaVM* jvm_;
    jobject callbackRef_ = nullptr;

    // Boost.Asio event loop
    std::unique_ptr<boost::asio::io_service> ioService_;
    std::unique_ptr<boost::asio::io_service::work> ioWork_;
    std::thread ioThread_;
    boost::asio::io_service::strand* strand_ = nullptr;

    // aasdk pipeline
    std::shared_ptr<JniTransport> transport_;
    aasdk::messenger::ICryptor::Pointer cryptor_;
    aasdk::transport::ITransport::Pointer rawTransport_;
    aasdk::messenger::IMessenger::Pointer messenger_;

    // Channels
    std::shared_ptr<aasdk::channel::control::ControlServiceChannel> controlChannel_;
    std::shared_ptr<aasdk::channel::mediasink::video::channel::VideoChannel> videoChannel_;
    std::shared_ptr<aasdk::channel::mediasink::audio::AudioMediaSinkService> mediaAudioChannel_;
    std::shared_ptr<aasdk::channel::mediasink::audio::AudioMediaSinkService> guidanceAudioChannel_;
    std::shared_ptr<aasdk::channel::mediasink::audio::AudioMediaSinkService> systemAudioChannel_;
    std::shared_ptr<aasdk::channel::inputsource::InputSourceService> inputChannel_;
    std::shared_ptr<aasdk::channel::sensorsource::SensorSourceService> sensorChannel_;
    std::shared_ptr<aasdk::channel::navigationstatus::NavigationStatusService> navChannel_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> streaming_{false};

    // SDR config (from Kotlin)
    struct SdrConfig {
        int videoWidth = 1920;
        int videoHeight = 1080;
        int videoFps = 60;
        int videoDpi = 160;
        int marginWidth = 0;
        int marginHeight = 0;
        int pixelAspectE4 = 0;
        std::string btMac;
        std::string vehicleMake;
        std::string vehicleModel;
        std::string vehicleYear;
        int driverPosition = 0;
    };
    SdrConfig sdrConfig_;

    // Cached JNI method IDs for the callback object
    struct CallbackMethods {
        jmethodID onSessionStarted = nullptr;
        jmethodID onSessionStopped = nullptr;
        jmethodID onVideoFrame = nullptr;
        jmethodID onAudioFrame = nullptr;
        jmethodID onMicRequest = nullptr;
        jmethodID onNavigationStatus = nullptr;
        jmethodID onNavigationTurn = nullptr;
        jmethodID onNavigationDistance = nullptr;
        jmethodID onMediaMetadata = nullptr;
        jmethodID onMediaPlayback = nullptr;
        jmethodID onPhoneStatus = nullptr;
        jmethodID onPhoneBattery = nullptr;
        jmethodID onVoiceSession = nullptr;
        jmethodID onAudioFocusRequest = nullptr;
        jmethodID onError = nullptr;
    };
    CallbackMethods cbMethods_;
};

} // namespace openautolink::jni
